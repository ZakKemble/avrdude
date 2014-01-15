/*
 * avrdude - A Downloader/Uploader for AVR device programmers
 * Copyright (C) 2012 Kirill Levchenko
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

/* $Id$ */

#include "ac_cfg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <unistd.h>

#if HAVE_STDINT_H
#include <stdint.h>
#elif HAVE_INTTYPES_H
#include <inttypes.h>
#endif

#include "flip.h"
#include "dfu.h"

#include "avrdude.h"
#include "avr.h"
#include "pgm.h"

#include "usbdevs.h" /* for USB_VENDOR_ATMEL */

/* There are three versions of the FLIP protocol:
 *
 * Version 0: C51 parts
 * Version 1: megaAVR parts ("USB DFU Bootloader Datasheet" [doc7618])
 * Version 2: XMEGA parts (AVR4023 [doc8457])
 *
 * We currently only support Version 2, as documented in AVR4023.
 *
 * Additional references:
 *   flip_protocol.h from the Atmel Software Framework.
 *   udi_dfu_atmel.c from XMEGA bootloaders archive.
 */

/* EXPORTED CONSTANT STRINGS */

const char flip2_desc[] = "FLIPv2 USB DFU protocol (AVR4023)";

/* PRIVATE DATA STRUCTURES */

struct flip2
{
  struct dfu_dev *dfu;
  unsigned char part_sig[3];
  unsigned char part_rev;
  unsigned char boot_ver;
};

#define FLIP2(pgm) ((struct flip2 *)(pgm->cookie))

/* The FLIP2 protocol assigns specific meaning to certain combinations of
 * status and state bytes in the DFU_GETSTATUS response. These constants en-
 * code these combinations as a 16-bit value: the high order byte is the
 * status and the low order byte is the state of the status-state pairing.
 */

#define FLIP2_STATUS_OK 0x0000
#define FLIP2_STATUS_STALL 0x0F0A
#define FLIP2_STATUS_MEM_UKNOWN 0x030A
#define FLIP2_STATUS_MEM_PROTECTED 0x0300
#define FLIP2_STATUS_OUTOFRANGE 0x080A
#define FLIP2_STATUS_BLANK_FAIL 0x0500
#define FLIP2_STATUS_ERASE_ONGOING 0x0904

/* FLIP2 data structures and constants. */

struct flip2_cmd {
  unsigned char group_id;
  unsigned char cmd_id;
  unsigned char args[4];
};

#define FLIP2_CMD_GROUP_DOWNLOAD 0x01
#define FLIP2_CMD_GROUP_UPLOAD 0x03
#define FLIP2_CMD_GROUP_EXEC 0x04
#define FLIP2_CMD_GROUP_SELECT 0x06

#define FLIP2_CMD_PROG_START 0x00
#define FLIP2_CMD_READ_MEMORY 0x00
#define FLIP2_CMD_SELECT_MEMORY 0x03
#define FLIP2_CMD_CHIP_ERASE 0x00
#define FLIP2_CMD_START_APP 0x03

#define FLIP2_SELECT_MEMORY_UNIT 0x00
#define FLIP2_SELECT_MEMORY_PAGE 0x01

enum flip2_mem_unit {
  FLIP2_MEM_UNIT_UNKNOWN = -1,
  FLIP2_MEM_UNIT_FLASH = 0x00,
  FLIP2_MEM_UNIT_EEPROM = 0x01,
  FLIP2_MEM_UNIT_SECURITY = 0x02,
  FLIP2_MEM_UNIT_CONFIGURATION = 0x03,
  FLIP2_MEM_UNIT_BOOTLOADER = 0x04,
  FLIP2_MEM_UNIT_SIGNATURE = 0x05,
  FLIP2_MEM_UNIT_USER = 0x06,
  FLIP2_MEM_UNIT_INT_RAM = 0x07,
  FLIP2_MEM_UNIT_EXT_MEM_CS0 = 0x08,
  FLIP2_MEM_UNIT_EXT_MEM_CS1 = 0x09,
  FLIP2_MEM_UNIT_EXT_MEM_CS2 = 0x0A,
  FLIP2_MEM_UNIT_EXT_MEM_CS3 = 0x0B,
  FLIP2_MEM_UNIT_EXT_MEM_CS4 = 0x0C,
  FLIP2_MEM_UNIT_EXT_MEM_CS5 = 0x0D,
  FLIP2_MEM_UNIT_EXT_MEM_CS6 = 0x0E,
  FLIP2_MEM_UNIT_EXT_MEM_CS7 = 0x0F,
  FLIP2_MEM_UNIT_EXT_MEM_DF = 0x10
};

/* EXPORTED PROGRAMMER FUNCTION PROTOTYPES */

static int flip2_open(PROGRAMMER *pgm, char *port_spec);
static int flip2_initialize(PROGRAMMER* pgm, AVRPART *part);
static void flip2_close(PROGRAMMER* pgm);
static void flip2_enable(PROGRAMMER* pgm);
static void flip2_disable(PROGRAMMER* pgm);
static void flip2_display(PROGRAMMER* pgm, const char *prefix);
static int flip2_program_enable(PROGRAMMER* pgm, AVRPART *part);
static int flip2_chip_erase(PROGRAMMER* pgm, AVRPART *part);
static int flip2_read_byte(PROGRAMMER* pgm, AVRPART *part, AVRMEM *mem,
  unsigned long addr, unsigned char *value);
static int flip2_write_byte(PROGRAMMER* pgm, AVRPART *part, AVRMEM *mem,
  unsigned long addr, unsigned char value);
static int flip2_page_erase(PROGRAMMER* pgm, AVRPART *part, AVRMEM *mem,
  unsigned int base_addr);
static int flip2_paged_load(PROGRAMMER* pgm, AVRPART *part, AVRMEM *mem,
  unsigned int page_size, unsigned int addr, unsigned int n_bytes);
static int flip2_paged_write(PROGRAMMER* pgm, AVRPART *part, AVRMEM *mem,
  unsigned int page_size, unsigned int addr, unsigned int n_bytes);
static int flip2_read_sig_bytes(PROGRAMMER* pgm, AVRPART *part, AVRMEM *mem);
static void flip2_setup(PROGRAMMER * pgm);
static void flip2_teardown(PROGRAMMER * pgm);

/* INTERNAL PROGRAMMER FUNCTION PROTOTYPES */

static void flip2_show_info(struct flip2 *flip2);

static int flip2_read_memory(struct dfu_dev *dfu,
  enum flip2_mem_unit mem_unit, uint32_t addr, void *ptr, int size);
static int flip2_write_memory(struct dfu_dev *dfu,
  enum flip2_mem_unit mem_unit, uint32_t addr, const void *ptr, int size);

static int flip2_set_mem_unit(struct dfu_dev *dfu,
  enum flip2_mem_unit mem_unit);
static int flip2_set_mem_page(struct dfu_dev *dfu, unsigned short page_addr);
static int flip2_read_max1k(struct dfu_dev *dfu,
  unsigned short offset, void *ptr, unsigned short size);
static int flip2_write_max1k(struct dfu_dev *dfu,
  unsigned short offset, const void *ptr, unsigned short size);

static const char * flip2_status_str(const struct dfu_status *status);
static const char * flip2_mem_unit_str(enum flip2_mem_unit mem_unit);
static enum flip2_mem_unit flip2_mem_unit(const char *name);

/* THE INITPGM FUNCTION DEFINITIONS */

void flip2_initpgm(PROGRAMMER *pgm)
{
  strcpy(pgm->type, "flip2");

  /* Mandatory Functions */
  pgm->initialize       = flip2_initialize;
  pgm->enable           = flip2_enable;
  pgm->disable          = flip2_disable;
  pgm->display          = flip2_display;
  pgm->program_enable   = flip2_program_enable;
  pgm->chip_erase       = flip2_chip_erase;
  pgm->open             = flip2_open;
  pgm->close            = flip2_close;
  pgm->page_erase       = flip2_page_erase;
  pgm->paged_load       = flip2_paged_load;
  pgm->paged_write      = flip2_paged_write;
  pgm->read_byte        = flip2_read_byte;
  pgm->write_byte       = flip2_write_byte;
  pgm->read_sig_bytes   = flip2_read_sig_bytes;
  pgm->setup            = flip2_setup;
  pgm->teardown         = flip2_teardown;
}

/* EXPORTED PROGRAMMER FUNCTION DEFINITIONS */

int flip2_open(PROGRAMMER *pgm, char *port_spec)
{
  FLIP2(pgm)->dfu = dfu_open(port_spec);
  return (FLIP2(pgm)->dfu != NULL) ? 0 : -1;
}

int flip2_initialize(PROGRAMMER* pgm, AVRPART *part)
{
  unsigned short vid, pid;
  int result;

  /* A note about return values. Negative return values from this function are
   * interpreted as failure by main(), from where this function is called.
   * However such failures are interpreted as a device signature check failure
   * and the user is adviced to use the -F option to override this check. In
   * our case, this is misleading, so we defer reporting an error until another
   * function is called. Thus, we always return 0 (success) from initialize().
   * I don't like this, but I don't want to mess with main().
   */

  /* The dfu_init() function will try to find the target part either based on
   * a USB address provided by the user with the -P option or by matching the
   * VID and PID of the device. The VID may be specified in the programmer
   * definition; if not specified, it defaults to USB_VENDOR_ATMEL (defined
   * in usbdevs.h). The PID may be specified either in the programmer
   * definition or the part definition; the programmer definition takes
   * priority. The default PID value is 0, which causes dfu_init() to ignore
   * the PID when matching a target device.
   */

  vid = (pgm->usbvid != 0) ? pgm->usbvid : USB_VENDOR_ATMEL;
  pid = (pgm->usbpid != 0) ? pgm->usbpid : part->usbpid;

  result = dfu_init(FLIP2(pgm)->dfu, vid, pid);

  if (result != 0)
    goto flip2_initialize_fail;

  result = flip2_read_memory(FLIP2(pgm)->dfu,
    FLIP2_MEM_UNIT_SIGNATURE, 0, FLIP2(pgm)->part_sig, 4);

  if (result != 0)
    goto flip2_initialize_fail;

  result = flip2_read_memory(FLIP2(pgm)->dfu,
    FLIP2_MEM_UNIT_BOOTLOADER, 0, &FLIP2(pgm)->boot_ver, 1);

  if (result != 0)
    goto flip2_initialize_fail;

  if (verbose)
    flip2_show_info(FLIP2(pgm));

  return 0;

flip2_initialize_fail:
  dfu_close(FLIP2(pgm)->dfu);
  FLIP2(pgm)->dfu = NULL;
  return 0;
}

void flip2_close(PROGRAMMER* pgm)
{
  if (FLIP2(pgm)->dfu != NULL) {
    dfu_close(FLIP2(pgm)->dfu);
    FLIP2(pgm)->dfu = NULL;
  }
}

void flip2_enable(PROGRAMMER* pgm)
{
  /* Nothing to do. */
}

void flip2_disable(PROGRAMMER* pgm)
{
  /* Nothing to do. */
}

void flip2_display(PROGRAMMER* pgm, const char *prefix)
{
  /* Nothing to do. */
}

int flip2_program_enable(PROGRAMMER* pgm, AVRPART *part)
{
  /* I couldn't find anything that uses this function, although it is marked
   * as "mandatory" in pgm.c. In case anyone does use it, we'll report an
   * error if we failed to initialize.
   */

  return (FLIP2(pgm)->dfu != NULL) ? 0 : -1;
}

int flip2_chip_erase(PROGRAMMER* pgm, AVRPART *part)
{
  struct dfu_status status;
  int cmd_result = 0;
  int aux_result;

  if (verbose > 1)
    fprintf(stderr, "%s: flip_chip_erase()\n", progname);

  struct flip2_cmd cmd = {
    FLIP2_CMD_GROUP_EXEC, FLIP2_CMD_CHIP_ERASE, { 0xFF, 0, 0, 0 }
  };

  for (;;) {
    cmd_result = dfu_dnload(FLIP2(pgm)->dfu, &cmd, sizeof(cmd));
    aux_result = dfu_getstatus(FLIP2(pgm)->dfu, &status);

    if (aux_result != 0)
      return aux_result;

    if (status.bStatus != DFU_STATUS_OK) {
      if (status.bStatus == ((FLIP2_STATUS_ERASE_ONGOING >> 8) & 0xFF) &&
          status.bState == ((FLIP2_STATUS_ERASE_ONGOING >> 0) & 0xFF))
      {
        continue;
      } else
        fprintf(stderr, "%s: Error: DFU status %s\n", progname,
          flip2_status_str(&status));
      dfu_clrstatus(FLIP2(pgm)->dfu);
    } else
      break;
  }

  return cmd_result;
}

int flip2_read_byte(PROGRAMMER* pgm, AVRPART *part, AVRMEM *mem,
  unsigned long addr, unsigned char *value)
{
  enum flip2_mem_unit mem_unit;

  if (FLIP2(pgm)->dfu == NULL)
    return -1;

  mem_unit = flip2_mem_unit(mem->desc);

  if (mem_unit == FLIP2_MEM_UNIT_UNKNOWN) {
    fprintf(stderr, "%s: Error: "
      "\"%s\" memory not accessible using FLIP",
      progname, mem->desc);
    if (strcmp(mem->desc, "flash") == 0)
      fprintf(stderr, " (did you mean \"application\"?)");
    fprintf(stderr, "\n");
    return -1;
  }

  return flip2_read_memory(FLIP2(pgm)->dfu, mem_unit, addr, value, 1);
}

int flip2_write_byte(PROGRAMMER* pgm, AVRPART *part, AVRMEM *mem,
  unsigned long addr, unsigned char value)
{
  enum flip2_mem_unit mem_unit;

  if (FLIP2(pgm)->dfu == NULL)
    return -1;

  mem_unit = flip2_mem_unit(mem->desc);

  if (mem_unit == FLIP2_MEM_UNIT_UNKNOWN) {
    fprintf(stderr, "%s: Error: "
      "\"%s\" memory not accessible using FLIP",
      progname, mem->desc);
    if (strcmp(mem->desc, "flash") == 0)
      fprintf(stderr, " (did you mean \"application\"?)");
    fprintf(stderr, "\n");
    return -1;
  }

  return flip2_write_memory(FLIP2(pgm)->dfu, mem_unit, addr, &value, 1);
}

int flip2_page_erase(PROGRAMMER* pgm, AVRPART *part, AVRMEM *mem,
  unsigned int base_addr)
{
  return 0;
}

int flip2_paged_load(PROGRAMMER* pgm, AVRPART *part, AVRMEM *mem,
  unsigned int page_size, unsigned int addr, unsigned int n_bytes)
{
  enum flip2_mem_unit mem_unit;
  int result;

  if (FLIP2(pgm)->dfu == NULL)
    return -1;

  mem_unit = flip2_mem_unit(mem->desc);

  if (mem_unit == FLIP2_MEM_UNIT_UNKNOWN) {
    fprintf(stderr, "%s: Error: "
      "\"%s\" memory not accessible using FLIP",
      progname, mem->desc);
    if (strcmp(mem->desc, "flash") == 0)
      fprintf(stderr, " (did you mean \"application\"?)");
    fprintf(stderr, "\n");
    return -1;
  }

  if (n_bytes > INT_MAX) {
    /* This should never happen, unless the int type is only 16 bits. */
    fprintf(stderr, "%s: Error: Attempting to read more than %d bytes\n",
      progname, INT_MAX);
    exit(1);
  }

  result = flip2_read_memory(FLIP2(pgm)->dfu, mem_unit, addr,
    mem->buf + addr, n_bytes);

  return (result == 0) ? n_bytes : -1;
}

int flip2_paged_write(PROGRAMMER* pgm, AVRPART *part, AVRMEM *mem,
  unsigned int page_size, unsigned int addr, unsigned int n_bytes)
{
  enum flip2_mem_unit mem_unit;
  int result;

  if (FLIP2(pgm)->dfu == NULL)
    return -1;

  mem_unit = flip2_mem_unit(mem->desc);

  if (mem_unit == FLIP2_MEM_UNIT_UNKNOWN) {
    fprintf(stderr, "%s: Error: "
      "\"%s\" memory not accessible using FLIP",
      progname, mem->desc);
    if (strcmp(mem->desc, "flash") == 0)
      fprintf(stderr, " (did you mean \"application\"?)");
    fprintf(stderr, "\n");
    return -1;
  }

  if (n_bytes > INT_MAX) {
    /* This should never happen, unless the int type is only 16 bits. */
    fprintf(stderr, "%s: Error: Attempting to read more than %d bytes\n",
      progname, INT_MAX);
    exit(1);
  }

  result = flip2_write_memory(FLIP2(pgm)->dfu, mem_unit, addr,
    mem->buf + addr, n_bytes);

  return (result == 0) ? n_bytes : -1;
}

int flip2_read_sig_bytes(PROGRAMMER* pgm, AVRPART *part, AVRMEM *mem)
{
  if (FLIP2(pgm)->dfu == NULL)
    return -1;

  if (mem->size < sizeof(FLIP2(pgm)->part_sig)) {
    fprintf(stderr, "%s: Error: Signature read must be at least %u bytes\n",
      progname, (unsigned int) sizeof(FLIP2(pgm)->part_sig));
    return -1;
  }

  memcpy(mem->buf, FLIP2(pgm)->part_sig, sizeof(FLIP2(pgm)->part_sig));
  return 0;
}

void flip2_setup(PROGRAMMER * pgm)
{
  pgm->cookie = calloc(1, sizeof(struct flip2));

  if (pgm->cookie == NULL) {
    perror(progname);
    exit(1);
  }
}

void flip2_teardown(PROGRAMMER * pgm)
{
  free(pgm->cookie);
  pgm->cookie = NULL;
}

/* INTERNAL FUNCTION DEFINITIONS
 */

void flip2_show_info(struct flip2 *flip2)
{
  dfu_show_info(flip2->dfu);

  fprintf(stderr, "    Part signature      : 0x%02X%02X%02X\n",
    (int) flip2->part_sig[0],
    (int) flip2->part_sig[1],
    (int) flip2->part_sig[2]);

  if (flip2->part_rev < 26)
    fprintf(stderr, "    Part revision       : %c\n",
      (char) (flip2->part_rev + 'A'));
  else
    fprintf(stderr, "    Part revision       : %c%c\n",
      (char) (flip2->part_rev / 26 - 1 + 'A'),
      (char) (flip2->part_rev % 26 + 'A'));

  fprintf(stderr, "    Bootloader version  : 2.%hu.%hu\n",
    ((unsigned short) flip2->boot_ver >> 4) & 0xF,
    ((unsigned short) flip2->boot_ver >> 0) & 0xF);

  fprintf(stderr, "    USB max packet size : %hu\n",
    (unsigned short) flip2->dfu->dev_desc.bMaxPacketSize0);
}

int flip2_read_memory(struct dfu_dev *dfu,
  enum flip2_mem_unit mem_unit, uint32_t addr, void *ptr, int size)
{
  unsigned short prev_page_addr;
  unsigned short page_addr;
  const char * mem_name;
  int read_size;
  int result;

  if (verbose > 1)
    fprintf(stderr,
            "%s: flip_read_memory(%s, 0x%04x, %d)\n",
            progname, flip2_mem_unit_str(mem_unit), addr, size);

  result = flip2_set_mem_unit(dfu, mem_unit);

  if (result != 0) {
    if ((mem_name = flip2_mem_unit_str(mem_unit)) != NULL)
      fprintf(stderr, "%s: Error: Failed to set memory unit 0x%02X (%s)\n",
        progname, (int) mem_unit, mem_name);
    else
      fprintf(stderr, "%s: Error: Failed to set memory unit 0x%02X\n",
        progname, (int) mem_unit);
    return -1;
  }

  page_addr = addr >> 16;
  result = flip2_set_mem_page(dfu, page_addr);

  if (result != 0) {
    fprintf(stderr, "%s: Error: Failed to set memory page 0x%04hX\n",
        progname, page_addr);
    return -1;
  }

  while (size > 0) {
    prev_page_addr = page_addr;
    page_addr = addr >> 16;

    if (page_addr != prev_page_addr) {
      result = flip2_set_mem_page(dfu, page_addr);
      if (result != 0) {
        fprintf(stderr, "%s: Error: Failed to set memory page 0x%04hX\n",
            progname, page_addr);
        return -1;
      }
    }

    read_size = (size > 0x400) ? 0x400 : size;
    result = flip2_read_max1k(dfu, addr & 0xFFFF, ptr, read_size);

    if (result != 0) {
      fprintf(stderr, "%s: Error: Failed to read 0x%04X bytes at 0x%04lX\n",
          progname, read_size, (unsigned long) addr);
      return -1;
    }

    ptr += read_size;
    addr += read_size;
    size -= read_size;
  }

  return 0;
}

int flip2_write_memory(struct dfu_dev *dfu,
  enum flip2_mem_unit mem_unit, uint32_t addr, const void *ptr, int size)
{
  unsigned short prev_page_addr;
  unsigned short page_addr;
  const char * mem_name;
  int write_size;
  int result;

  if (verbose > 1)
    fprintf(stderr,
            "%s: flip_write_memory(%s, 0x%04x, %d)\n",
            progname, flip2_mem_unit_str(mem_unit), addr, size);

  result = flip2_set_mem_unit(dfu, mem_unit);

  if (result != 0) {
    if ((mem_name = flip2_mem_unit_str(mem_unit)) != NULL)
      fprintf(stderr, "%s: Error: Failed to set memory unit 0x%02X (%s)\n",
        progname, (int) mem_unit, mem_name);
    else
      fprintf(stderr, "%s: Error: Failed to set memory unit 0x%02X\n",
        progname, (int) mem_unit);
    return -1;
  }

  page_addr = addr >> 16;
  result = flip2_set_mem_page(dfu, page_addr);

  if (result != 0) {
    fprintf(stderr, "%s: Error: Failed to set memory page 0x%04hX\n",
        progname, page_addr);
    return -1;
  }

  while (size > 0) {
    prev_page_addr = page_addr;
    page_addr = addr >> 16;

    if (page_addr != prev_page_addr) {
      result = flip2_set_mem_page(dfu, page_addr);
      if (result != 0) {
        fprintf(stderr, "%s: Error: Failed to set memory page 0x%04hX\n",
            progname, page_addr);
        return -1;
      }
    }

    write_size = (size > 0x800) ? 0x800 : size;
    result = flip2_write_max1k(dfu, addr & 0xFFFF, ptr, write_size);

    if (result != 0) {
      fprintf(stderr, "%s: Error: Failed to write 0x%04X bytes at 0x%04lX\n",
          progname, write_size, (unsigned long) addr);
      return -1;
    }

    ptr += write_size;
    addr += write_size;
    size -= write_size;
  }

  return 0;
}

int flip2_set_mem_unit(struct dfu_dev *dfu, enum flip2_mem_unit mem_unit)
{
  struct dfu_status status;
  int cmd_result = 0;
  int aux_result;

  struct flip2_cmd cmd = {
    FLIP2_CMD_GROUP_SELECT, FLIP2_CMD_SELECT_MEMORY, { 0, 0, 0, 0 }
  };

  cmd.args[0] = FLIP2_SELECT_MEMORY_UNIT;
  cmd.args[1] = mem_unit;

  cmd_result = dfu_dnload(dfu, &cmd, sizeof(cmd));

  aux_result = dfu_getstatus(dfu, &status);

  if (aux_result != 0)
    return aux_result;

  if (status.bStatus != DFU_STATUS_OK) {
    if (status.bStatus == ((FLIP2_STATUS_OUTOFRANGE >> 8) & 0xFF) &&
        status.bState == ((FLIP2_STATUS_OUTOFRANGE >> 0) & 0xFF))
    {
      fprintf(stderr, "%s: Error: Unknown memory unit (0x%02x)\n",
        progname, (unsigned int) mem_unit);
    } else
      fprintf(stderr, "%s: Error: DFU status %s\n", progname,
        flip2_status_str(&status));
    dfu_clrstatus(dfu);
  }

  return cmd_result;
}

int flip2_set_mem_page(struct dfu_dev *dfu,
  unsigned short page_addr)
{
  struct dfu_status status;
  int cmd_result = 0;
  int aux_result;

  struct flip2_cmd cmd = {
    FLIP2_CMD_GROUP_SELECT, FLIP2_CMD_SELECT_MEMORY, { 0, 0, 0, 0 }
  };

  cmd.args[0] = FLIP2_SELECT_MEMORY_PAGE;
  cmd.args[1] = (page_addr >> 8) & 0xFF;
  cmd.args[2] = (page_addr >> 0) & 0xFF;

  cmd_result = dfu_dnload(dfu, &cmd, sizeof(cmd));

  aux_result = dfu_getstatus(dfu, &status);

  if (aux_result != 0)
    return aux_result;

  if (status.bStatus != DFU_STATUS_OK) {
    if (status.bStatus == ((FLIP2_STATUS_OUTOFRANGE >> 8) & 0xFF) &&
        status.bState == ((FLIP2_STATUS_OUTOFRANGE >> 0) & 0xFF))
    {
      fprintf(stderr, "%s: Error: Page address out of range (0x%04hx)\n",
        progname, page_addr);
    } else
      fprintf(stderr, "%s: Error: DFU status %s\n", progname,
        flip2_status_str(&status));
    dfu_clrstatus(dfu);
  }

  return cmd_result;
}

int flip2_read_max1k(struct dfu_dev *dfu,
  unsigned short offset, void *ptr, unsigned short size)
{
  struct dfu_status status;
  int cmd_result = 0;
  int aux_result;

  struct flip2_cmd cmd = {
    FLIP2_CMD_GROUP_UPLOAD, FLIP2_CMD_READ_MEMORY, { 0, 0, 0, 0 }
  };

  cmd.args[0] = (offset >> 8) & 0xFF;
  cmd.args[1] = (offset >> 0) & 0xFF;
  cmd.args[2] = ((offset+size-1) >> 8) & 0xFF;
  cmd.args[3] = ((offset+size-1) >> 0) & 0xFF;

  cmd_result = dfu_dnload(dfu, &cmd, sizeof(cmd));

  if (cmd_result != 0)
    goto flip2_read_max1k_status;

  cmd_result = dfu_upload(dfu, (char*) ptr, size);

flip2_read_max1k_status:

  aux_result = dfu_getstatus(dfu, &status);

  if (aux_result != 0)
    return aux_result;

  if (status.bStatus != DFU_STATUS_OK) {
    if (status.bStatus == ((FLIP2_STATUS_OUTOFRANGE >> 8) & 0xFF) &&
        status.bState == ((FLIP2_STATUS_OUTOFRANGE >> 0) & 0xFF))
    {
      fprintf(stderr, "%s: Error: Address out of range [0x%04hX,0x%04hX]\n",
        progname, offset, offset+size-1);
    } else
      fprintf(stderr, "%s: Error: DFU status %s\n", progname,
        flip2_status_str(&status));
    dfu_clrstatus(dfu);
  }

  return cmd_result;
}

int flip2_write_max1k(struct dfu_dev *dfu,
  unsigned short offset, const void *ptr, unsigned short size)
{
  char buffer[64+64+0x400];
  unsigned short data_offset;
  struct dfu_status status;
  int cmd_result = 0;
  int aux_result;

  struct flip2_cmd cmd = {
    FLIP2_CMD_GROUP_DOWNLOAD, FLIP2_CMD_PROG_START, { 0, 0, 0, 0 }
  };

  cmd.args[0] = (offset >> 8) & 0xFF;
  cmd.args[1] = (offset >> 0) & 0xFF;
  cmd.args[2] = ((offset+size-1) >> 8) & 0xFF;
  cmd.args[3] = ((offset+size-1) >> 0) & 0xFF;

  if (size > 0x400) {
    fprintf(stderr, "%s: Error: Write block too large (%hu > 1024)\n",
      progname, size);
    exit(1);
  }

  /* There are some special padding requirements for writes. The first packet
   * must consist only of the FLIP2 command data, which must be padded to
   * fill out the USB packet (the packet size is given by bMaxPacketSize0 in
   * the device descriptor). In addition, the data must be padded so that the
   * first byte of data to be written is at located at position (offset mod
   * bMaxPacketSize0) within the packet.
   */

  data_offset = dfu->dev_desc.bMaxPacketSize0;
  data_offset += offset % dfu->dev_desc.bMaxPacketSize0;

  memcpy(buffer, &cmd, sizeof(cmd));
  memset(buffer + sizeof(cmd), 0, data_offset - sizeof(cmd));
  memcpy(buffer + data_offset, ptr, size);

  cmd_result = dfu_dnload(dfu, buffer, data_offset + size);

  aux_result = dfu_getstatus(dfu, &status);

  if (aux_result != 0)
    return aux_result;

  if (status.bStatus != DFU_STATUS_OK) {
    if (status.bStatus == ((FLIP2_STATUS_OUTOFRANGE >> 8) & 0xFF) &&
        status.bState == ((FLIP2_STATUS_OUTOFRANGE >> 0) & 0xFF))
    {
      fprintf(stderr, "%s: Error: Address out of range [0x%04hX,0x%04hX]\n",
        progname, offset, offset+size-1);
    } else
      fprintf(stderr, "%s: Error: DFU status %s\n", progname,
        flip2_status_str(&status));
    dfu_clrstatus(dfu);
  }

  return cmd_result;
}

const char * flip2_status_str(const struct dfu_status *status)
{
  unsigned short selector;

  selector = (unsigned short) status->bStatus << 8;
  selector |= status->bState;

  switch (selector) {
    case FLIP2_STATUS_OK: return "OK";
    case FLIP2_STATUS_STALL: return "STALL";
    case FLIP2_STATUS_MEM_UKNOWN: return "MEM_UKNOWN";
    case FLIP2_STATUS_MEM_PROTECTED: return "MEM_PROTECTED";
    case FLIP2_STATUS_OUTOFRANGE: return "OUTOFRANGE";
    case FLIP2_STATUS_BLANK_FAIL: return "BLANK_FAIL";
    case FLIP2_STATUS_ERASE_ONGOING: return "ERASE_ONGOING";
    default: return dfu_status_str(status->bStatus);
  }
}

const char * flip2_mem_unit_str(enum flip2_mem_unit mem_unit)
{
  switch (mem_unit) {
  case FLIP2_MEM_UNIT_FLASH: return "Flash";
  case FLIP2_MEM_UNIT_EEPROM: return "EEPROM";
  case FLIP2_MEM_UNIT_SECURITY: return "security";
  case FLIP2_MEM_UNIT_CONFIGURATION: return "configuration";
  case FLIP2_MEM_UNIT_BOOTLOADER: return "bootloader version";
  case FLIP2_MEM_UNIT_SIGNATURE: return "signature";
  case FLIP2_MEM_UNIT_USER: return "user";
  case FLIP2_MEM_UNIT_INT_RAM: return "internal RAM";
  case FLIP2_MEM_UNIT_EXT_MEM_CS0: return "EXT_MEM_CS0";
  case FLIP2_MEM_UNIT_EXT_MEM_CS1: return "EXT_MEM_CS1";
  case FLIP2_MEM_UNIT_EXT_MEM_CS2: return "EXT_MEM_CS2";
  case FLIP2_MEM_UNIT_EXT_MEM_CS3: return "EXT_MEM_CS3";
  case FLIP2_MEM_UNIT_EXT_MEM_CS4: return "EXT_MEM_CS4";
  case FLIP2_MEM_UNIT_EXT_MEM_CS5: return "EXT_MEM_CS5";
  case FLIP2_MEM_UNIT_EXT_MEM_CS6: return "EXT_MEM_CS6";
  case FLIP2_MEM_UNIT_EXT_MEM_CS7: return "EXT_MEM_CS7";
  case FLIP2_MEM_UNIT_EXT_MEM_DF: return "EXT_MEM_DF";
  default: return "unknown";
  }
}

enum flip2_mem_unit flip2_mem_unit(const char *name) {
  if (strcasecmp(name, "application") == 0)
    return FLIP2_MEM_UNIT_FLASH;
  if (strcasecmp(name, "eeprom") == 0)
    return FLIP2_MEM_UNIT_EEPROM;
  if (strcasecmp(name, "signature") == 0)
    return FLIP2_MEM_UNIT_SIGNATURE;
  return FLIP2_MEM_UNIT_UNKNOWN;
}

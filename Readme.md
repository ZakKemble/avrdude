AVRDUDE
=======

http://savannah.nongnu.org/projects/avrdude

AVRDUDE with some modifications for custom non-standard baud rates on Linux (as long as the hardware supports it).    
Custom bauds already work on Windows.

### Compiling
## Requirements:
autoconf (and some other auto stuff which is normally installed along with autoconf)
libtoolize
`apt-get install autoconf libtool`

## Optional extras (with a guess at what they do):
libelf - Support for uploading .elf files  
libusb - Support for programmers that use libUSB like USBasp  
libftdi1 - Support for special functions of FTDI chips (not needed if you're only using the serial function)  
libhid - Support for programmers that use some kind of HID thing  
`apt-get install libelf libusb libftdi1 libhid`

NOTE: `libftdi1` might be called `libftdi`

## Making
`chmod +x ./bootstrap`

`./bootstrap`

`./configure`

`make`

Test run  
`./avrdude`

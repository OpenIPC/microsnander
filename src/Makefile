# MicroSNANDer: SPI NOR/NAND programmer for CH341A, CH347 and EZP. It talks to
# the programmers through the Linux usbfs (/dev/bus/usb) itself, no libusb.
#
#   make                         native build
#   make CC=<cross>gcc LDFLAGS=-static
#                                static build (ci/build-musl.sh, the release binaries)
#   make LIBUSB=1                with libusb-1.0 instead of the usbfs code
#                                (Debian/Ubuntu: apt-get install libusb-1.0-0-dev;
#                                LIBUSB_CFLAGS, LIBUSB_LIBS for another one)
#

ifeq ($(LIBUSB),1)
LIBUSB_CFLAGS ?=
LIBUSB_LIBS ?= -lusb-1.0
USB_CFLAGS = -DMS_LIBUSB $(LIBUSB_CFLAGS)
USB_OBJS =
else
LIBUSB_LIBS =
USB_CFLAGS =
USB_OBJS = usbfs.o
endif

# CFLAGS is the user's (optimization, extra flags); the flags the build needs
# are in ALL_CFLAGS, so a CFLAGS given on the command line cannot drop them.
CFLAGS ?= -Os
ALL_CFLAGS = $(CFLAGS) -std=gnu99 -Wall -D_FILE_OFFSET_BITS=64 $(USB_CFLAGS)
LDLIBS = $(LIBUSB_LIBS) -pthread

OBJS = flashcmd_api.o spi_controller.o spi_nand_flash.o spi_nor_flash.o programmer.o ch341a_spi.o ch347_spi.o ezp.o usb.o $(USB_OBJS) voltage.o sigmastar.o selftest.o ops.o web.o timer.o main.o

all: microsnander

microsnander: $(OBJS)
	$(CC) $(ALL_CFLAGS) $(LDFLAGS) -s -o $@ $(OBJS) $(LDLIBS)

.c.o:
	$(CC) $(ALL_CFLAGS) -c $<

# the objects are built for one USB backend: a switch of LIBUSB rebuilds them
USB_BACKEND = $(if $(filter 1,$(LIBUSB)),libusb,usbfs)
usb-backend: FORCE
	@[ "$$(cat $@ 2>/dev/null)" = "$(USB_BACKEND)" ] || echo $(USB_BACKEND) > $@
$(OBJS): usb-backend usb.h usbfs.h

# the page of the web mode, as a C string; @VER@ in it (the fonts' ?v=) is _VER of main.c
VER := $(shell sed -n 's/^\#define[[:space:]]*_VER[[:space:]]*"\(.*\)"/\1/p' main.c)
web_page.h: web_page.html main.c
	sed 's/@VER@/$(VER)/g; s/\\/\\\\/g; s/"/\\"/g; s/^/"/; s/$$/\\n"/' $< | { echo 'static const char web_page[] ='; cat; echo ';'; } > $@

# the page's fonts (brass 1.5.0, fonts/OFL.txt) as arrays; written aside and
# moved, so a failed run leaves no header
WEB_FONTS = $(sort $(wildcard fonts/*.woff2))
web_fonts.h: web_fonts.sh fonts $(WEB_FONTS)
	sh web_fonts.sh $(WEB_FONTS) > $@.tmp
	mv -f $@.tmp $@

web.o: web_page.h web_fonts.h

clean:
	rm -f *.o microsnander* web_page.h web_fonts.h web_fonts.h.tmp usb-backend

.PHONY: all clean FORCE

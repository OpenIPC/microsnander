# MicroSNANDer: SPI NOR/NAND programmer for CH341A, CH347 and EZP, built with libusb-1.0.
#
#   make                         native build against the system libusb
#                                (Debian/Ubuntu: apt-get install libusb-1.0-0-dev)
#   make CC=<cross>gcc LDFLAGS=-static LIBUSB_CFLAGS=-I<prefix>/include \
#        LIBUSB_LIBS=<prefix>/lib/libusb-1.0.a
#                                static build against a libusb built for the
#                                target (ci/build-musl.sh, the release binaries)
#

LIBUSB_CFLAGS ?=
LIBUSB_LIBS ?= -lusb-1.0

# CFLAGS is the user's (optimization, extra flags); the flags the build needs
# are in ALL_CFLAGS, so a CFLAGS given on the command line cannot drop them.
CFLAGS ?= -Os
ALL_CFLAGS = $(CFLAGS) -std=gnu99 -Wall -D_FILE_OFFSET_BITS=64 $(LIBUSB_CFLAGS)
LDLIBS = $(LIBUSB_LIBS) -pthread

OBJS = flashcmd_api.o spi_controller.o spi_nand_flash.o spi_nor_flash.o programmer.o ch341a_spi.o ch347_spi.o ezp.o usb.o voltage.o ops.o web.o timer.o main.o

all: microsnander

microsnander: $(OBJS)
	$(CC) $(ALL_CFLAGS) $(LDFLAGS) -s -o $@ $(OBJS) $(LDLIBS)

.c.o:
	$(CC) $(ALL_CFLAGS) -c $<

# the page of the web mode, as a C string
web_page.h: web_page.html
	sed 's/\\/\\\\/g; s/"/\\"/g; s/^/"/; s/$$/\\n"/' $< | { echo 'static const char web_page[] ='; cat; echo ';'; } > $@

web.o: web_page.h

clean:
	rm -f *.o microsnander* web_page.h

.PHONY: all clean

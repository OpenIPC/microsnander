# MicroSNANDer: SPI NOR/NAND programmer for CH341A, built with libusb-1.0.
#
#   make                         native build against the system libusb
#                                (Debian/Ubuntu: apt-get install libusb-1.0-0-dev)
#   make EEPROM_SUPPORT=y        with the I2C and Microwire EEPROM support
#   make CC=<cross>gcc LDFLAGS=-static LIBUSB_CFLAGS=-I<prefix>/include \
#        LIBUSB_LIBS=<prefix>/lib/libusb-1.0.a
#                                static build against a libusb built for the
#                                target (ci/build-musl.sh, the release binaries)
#

EEPROM_SUPPORT = n

LIBUSB_CFLAGS ?=
LIBUSB_LIBS ?= -lusb-1.0

# CFLAGS is the user's (optimization, extra flags); the flags the build needs
# are in ALL_CFLAGS, so a CFLAGS given on the command line cannot drop them.
CFLAGS ?= -Os
ALL_CFLAGS = $(CFLAGS) -std=gnu99 -Wall -D_FILE_OFFSET_BITS=64 $(LIBUSB_CFLAGS)
LDLIBS = $(LIBUSB_LIBS) -pthread

OBJS = flashcmd_api.o spi_controller.o spi_nand_flash.o spi_nor_flash.o ch341a_spi.o timer.o main.o

ifeq ($(EEPROM_SUPPORT),y)
    ALL_CFLAGS += -DEEPROM_SUPPORT
    OBJS += ch341a_i2c.o i2c_eeprom.o
    OBJS += bitbang_microwire.o mw_eeprom.o ch341a_gpio.o
endif

all: microsnander

microsnander: $(OBJS)
	$(CC) $(ALL_CFLAGS) $(LDFLAGS) -s -o $@ $(OBJS) $(LDLIBS)

.c.o:
	$(CC) $(ALL_CFLAGS) -c $<

clean:
	rm -f *.o microsnander*

.PHONY: all clean

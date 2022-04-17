# To assist in cross-compiling
# apt-get inatall libusb-1.0-0 libusb-1.0-0-dev
#
CC=gcc
LDFLAGS=-pthread -lusb-1.0
TOPDIR=$(shell pwd)

EEPROM_SUPPORT = n

BIGFILES=-D_FILE_OFFSET_BITS=64
CFLAGS=-Os -std=gnu99 -Wall $(BIGFILES)

OBJS= flashcmd_api.o spi_controller.o spi_nand_flash.o spi_nor_flash.o ch341a_spi.o timer.o main.o

ifeq ($(EEPROM_SUPPORT),y)
    CFLAGS += -DEEPROM_SUPPORT
    OBJS += ch341a_i2c.o i2c_eeprom.o
    OBJS += bitbang_microwire.o mw_eeprom.o ch341a_gpio.o
endif

all: microsnander

microsnander: $(OBJS)
	$(CC) $(CFLAGS) -s -o $@ $(OBJS) $(LDFLAGS)

.c.o:
	$(CC) $(CFLAGS) -c $<

clean: 
	rm -f *.o microsnander*

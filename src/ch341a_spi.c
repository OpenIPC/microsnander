/*
 * This file is part of the flashrom project.
 *
 * Copyright (C) 2011 asbokid <ballymunboy@gmail.com>
 * Copyright (C) 2014 Pluto Yang <yangyj.ee@gmail.com>
 * Copyright (C) 2015-2016 Stefan Tauner
 * Copyright (C) 2015 Urja Rannikko <urjaman@gmail.com>
 * Copyright (C) 2018-2021 McMCC <mcmcc@mail.ru>
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
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 */
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include "ch341a_spi.h"
#include "usb.h"
#include <stdbool.h>

/* LIBUSB_CALL ensures the right calling conventions on libusb callbacks.
 * However, the macro is not defined everywhere. m(
 */
#ifndef LIBUSB_CALL
#define LIBUSB_CALL
#endif

#define	 USB_TIMEOUT			1000	/* base timeout, ms; usb_timeout() adds time for the data */
/* The CH341A sends the SPI stream at its SPI clock, ~60 KB/s of data. One
   transfer of a whole 64 KB NOR sector needs ~1.1 s and runs out of a flat
   1 s timeout, so a long stream goes in parts of at most this many bytes
   (write plus read). CS stays low between the parts: the chip sees one
   continuous stream. */
#define	 CH341_MAX_STREAM		(16 * 1024)
#define	 WRITE_EP			0x02
#define	 READ_EP			0x82

#define	 CH341_PACKET_LENGTH		0x20
#define	 CH341_MAX_PACKETS		256
#define	 CH341_MAX_PACKET_LEN		(CH341_PACKET_LENGTH * CH341_MAX_PACKETS)

#define	 CH341A_CMD_SET_OUTPUT		0xA1
#define	 CH341A_CMD_IO_ADDR		0xA2
#define	 CH341A_CMD_PRINT_OUT		0xA3
#define	 CH341A_CMD_SPI_STREAM		0xA8
#define	 CH341A_CMD_SIO_STREAM		0xA9
#define	 CH341A_CMD_I2C_STREAM		0xAA
#define	 CH341A_CMD_UIO_STREAM		0xAB

#define	 CH341A_CMD_I2C_STM_START	0x74
#define	 CH341A_CMD_I2C_STM_STOP	0x75
#define	 CH341A_CMD_I2C_STM_OUT		0x80
#define	 CH341A_CMD_I2C_STM_IN		0xC0
#define	 CH341A_CMD_I2C_STM_MAX		( min( 0x3F, CH341_PACKET_LENGTH ) )
#define	 CH341A_CMD_I2C_STM_SET		0x60 // bit 2: SPI with two data pairs D5,D4=out, D7,D6=in
#define	 CH341A_CMD_I2C_STM_US		0x40
#define	 CH341A_CMD_I2C_STM_MS		0x50
#define	 CH341A_CMD_I2C_STM_DLY		0x0F
#define	 CH341A_CMD_I2C_STM_END		0x00

#define	 CH341A_CMD_UIO_STM_IN		0x00
#define	 CH341A_CMD_UIO_STM_DIR		0x40
#define	 CH341A_CMD_UIO_STM_OUT		0x80
#define	 CH341A_CMD_UIO_STM_US		0xC0
#define	 CH341A_CMD_UIO_STM_END		0x20

#define	 CH341A_STM_I2C_20K		0x00
#define	 CH341A_STM_I2C_100K		0x01
#define	 CH341A_STM_I2C_400K		0x02
#define	 CH341A_STM_I2C_750K		0x03
#define	 CH341A_STM_SPI_DBL		0x04


/* Number of parallel IN transfers. 32 seems to produce the most stable throughput on Windows. */
#define USB_IN_TRANSFERS		32

struct dev_entry {
	uint16_t vendor_id;
	uint16_t device_id;
	const char *vendor_name;
	const char *device_name;
};

/* We need to use many queued IN transfers for any resemblance of performance (especially on Windows)
 * because USB spec says that transfers end on non-full packets and the device sends the 31 reply
 * data bytes to each 32-byte packet with command + 31 bytes of data... */
static struct libusb_transfer *transfer_out = NULL;
static int kernel_driver_detached;	/* reattach it at shutdown */
static struct libusb_transfer *transfer_ins[USB_IN_TRANSFERS] = {0};
struct libusb_device_handle *handle = NULL;

const struct dev_entry devs_ch341a_spi[] = {
	{0x1A86, 0x5512, "WinChipHead (WCH)", "CH341A"},
	{0},
};

enum trans_state {TRANS_ACTIVE = -2, TRANS_ERR = -1, TRANS_IDLE = 0};

#if 0
static void print_hex(const void *buf, size_t len)
{
	size_t i;
	for (i = 0; i < len; i++) {
		printf(" %02x", ((uint8_t *)buf)[i]);
		if (i % CH341_PACKET_LENGTH == CH341_PACKET_LENGTH - 1)
			printf("\n");
	}
}
#endif

static void cb_common(const char *func, struct libusb_transfer *transfer)
{
	int *transfer_cnt = (int*)transfer->user_data;

	if (transfer->status == LIBUSB_TRANSFER_CANCELLED) {
		/* Silently ACK and exit. */
		*transfer_cnt = TRANS_IDLE;
		return;
	}

	if (transfer->status != LIBUSB_TRANSFER_COMPLETED) {
		printf("\n%s: error: %s\n", func, libusb_error_name(transfer->status));
		*transfer_cnt = TRANS_ERR;
	} else {
		*transfer_cnt = transfer->actual_length;
	}
}

/* callback for bulk out async transfer */
static void LIBUSB_CALL cb_out(struct libusb_transfer *transfer)
{
	cb_common(__func__, transfer);
}

/* callback for bulk in async transfer */
static void LIBUSB_CALL cb_in(struct libusb_transfer *transfer)
{
	cb_common(__func__, transfer);
}

/* Timeout of one transfer, ms: the base plus the data at a slow 8 KB/s, so a
   slow hub or a clone programmer still fits. */
static unsigned int usb_timeout(unsigned int bytes)
{
	return USB_TIMEOUT + bytes / 8;
}

/* in_chunk: the bytes one OUT packet answers (an IN transfer each, so a short
   answer never ends a transfer early) */
static int32_t usb_xfer(const char *func, unsigned int writecnt, unsigned int readcnt, const uint8_t *writearr, uint8_t *readarr,
			unsigned int in_chunk, unsigned int timeout)
{
	if (handle == NULL)
		return -1;

	int state_out = TRANS_IDLE;
	transfer_out->buffer = (uint8_t*)writearr;
	transfer_out->length = writecnt;
	transfer_out->user_data = &state_out;
	transfer_out->timeout = timeout;

	/* Schedule write first */
	if (writecnt > 0) {
		state_out = TRANS_ACTIVE;
		int ret = libusb_submit_transfer(transfer_out);
		if (ret) {
			printf("%s: failed to submit OUT transfer: %s\n", func, libusb_error_name(ret));
			state_out = TRANS_ERR;
			goto err;
		}
	}

	/* Handle all asynchronous packets as long as we have stuff to write or read. The write(s) simply need
	 * to complete but we need to scheduling reads as long as we are not done. */
	unsigned int free_idx = 0; /* The IN transfer we expect to be free next. */
	unsigned int in_idx = 0; /* The IN transfer we expect to be completed next. */
	unsigned int in_done = 0;
	unsigned int in_active = 0;
	unsigned int out_done = 0;
	uint8_t *in_buf = readarr;
	int state_in[USB_IN_TRANSFERS] = {0};
	unsigned int asked[USB_IN_TRANSFERS] = {0};	/* the bytes each IN transfer waits for */
	do {
		/* Schedule new reads as long as there are free transfers and unscheduled bytes to read. */
		while ((in_done + in_active) < readcnt && state_in[free_idx] == TRANS_IDLE) {
			unsigned int cur_todo = min(in_chunk, readcnt - in_done - in_active);
			transfer_ins[free_idx]->length = cur_todo;
			asked[free_idx] = cur_todo;
			transfer_ins[free_idx]->buffer = in_buf;
			transfer_ins[free_idx]->user_data = &state_in[free_idx];
			transfer_ins[free_idx]->timeout = timeout;
			int ret = libusb_submit_transfer(transfer_ins[free_idx]);
			if (ret) {
				state_in[free_idx] = TRANS_ERR;
				printf("%s: failed to submit IN transfer: %s\n",
					 func, libusb_error_name(ret));
				goto err;
			}
			in_buf += cur_todo;
			in_active += cur_todo;
			state_in[free_idx] = TRANS_ACTIVE;
			free_idx = (free_idx + 1) % USB_IN_TRANSFERS; /* Increment (and wrap around). */
		}

		/* Actually get some work done. */
		libusb_handle_events_timeout(NULL, &(struct timeval){1, 0});

		/* Check for the write */
		if (out_done < writecnt) {
			if (state_out == TRANS_ERR) {
				goto err;
			} else if (state_out > 0) {
				out_done += state_out;
				state_out = TRANS_IDLE;
			}
		}
		/* Check for completed transfers. */
		while (state_in[in_idx] != TRANS_IDLE && state_in[in_idx] != TRANS_ACTIVE) {
			if (state_in[in_idx] == TRANS_ERR) {
				goto err;
			}
			/* a short answer: its place in the buffer is taken already, so the bytes
			   after it would land at the wrong offset */
			if ((unsigned int)state_in[in_idx] != asked[in_idx]) {
				printf("%s: short answer, %d of %u bytes\n", func, state_in[in_idx], asked[in_idx]);
				state_in[in_idx] = TRANS_IDLE;
				in_active -= asked[in_idx];
				goto err;
			}
			/* If a transfer is done, record the number of bytes read and reuse it later. */
			in_done += state_in[in_idx];
			in_active -= state_in[in_idx];
			state_in[in_idx] = TRANS_IDLE;
			in_idx = (in_idx + 1) % USB_IN_TRANSFERS; /* Increment (and wrap around). */
		}
	} while ((out_done < writecnt) || (in_done < readcnt));
#if 0
	if (out_done > 0) {
		printf("Wrote %d bytes:\n", out_done);
		print_hex(writearr, out_done);
		printf("\n\n");
	}
	if (in_done > 0) {
		printf("Read %d bytes:\n", in_done);
		print_hex(readarr, in_done);
		printf("\n\n");
	}
#endif
	return 0;
err:
	/* Clean up on errors. */
	printf("%s: Failed to %s %d bytes\n", func, (state_out == TRANS_ERR) ? "write" : "read",
		 (state_out == TRANS_ERR) ? writecnt : readcnt);
	/* First, we must cancel any ongoing requests and wait for them to be canceled. */
	if ((writecnt > 0) && (state_out == TRANS_ACTIVE)) {
		if (libusb_cancel_transfer(transfer_out) != 0)
			state_out = TRANS_ERR;
	}
	if (readcnt > 0) {
		unsigned int i;
		for (i = 0; i < USB_IN_TRANSFERS; i++) {
			if (state_in[i] == TRANS_ACTIVE)
				if (libusb_cancel_transfer(transfer_ins[i]) != 0)
					state_in[i] = TRANS_ERR;
		}
	}

	/* Wait for cancellations to complete. */
	while (1) {
		bool finished = true;
		if ((writecnt > 0) && (state_out == TRANS_ACTIVE))
			finished = false;
		if (readcnt > 0) {
			unsigned int i;
			for (i = 0; i < USB_IN_TRANSFERS; i++) {
				if (state_in[i] == TRANS_ACTIVE)
					finished = false;
			}
		}
		if (finished)
			break;
		libusb_handle_events_timeout(NULL, &(struct timeval){1, 0});
	}
	return -1;
}

static int32_t usb_transfer(const char *func, unsigned int writecnt, unsigned int readcnt, const uint8_t *writearr, uint8_t *readarr)
{
	return usb_xfer(func, writecnt, readcnt, writearr, readarr, CH341_PACKET_LENGTH - 1, usb_timeout(writecnt + readcnt));
}

/*   Set the I2C bus speed (speed(b1b0): 0 = 20kHz; 1 = 100kHz, 2 = 400kHz, 3 = 750kHz).
 *   Set the SPI bus data width (speed(b2): 0 = Single, 1 = Double).  */
int config_stream(unsigned int speed)
{
	if (handle == NULL)
		return -1;

	uint8_t buf[] = {
		CH341A_CMD_I2C_STREAM,
		CH341A_CMD_I2C_STM_SET | (speed & 0x7),
		CH341A_CMD_I2C_STM_END
	};

	int32_t ret = usb_transfer(__func__, sizeof(buf), 0, buf, NULL);
	if (ret < 0) {
		printf("Could not configure stream interface.\n");
	}
	return ret;
}

/* ---- I2C, for the SigmaStar ISP ----
   One stream command packet a transaction: START, the address byte, the data
   or the reads, STOP. STM_OUT with a length sends bytes without answers;
   STM_OUT without one sends one byte and answers its ACK (bit 7 set: NACK);
   STM_IN with a length reads bytes with ACK, STM_IN alone one byte with NACK,
   the last of a read. */

int ch341a_i2c_speed(unsigned int speed)
{
	return config_stream(speed) < 0 ? -1 : 0;	/* the SPI does not run in this mode */
}

/* With a length: no ACK comes back, so a refused byte is not told here. The
   next read and -v find it */
int ch341a_i2c_write(unsigned char addr, const unsigned char *data, unsigned int len)
{
	uint8_t buf[CH341_PACKET_LENGTH];
	unsigned int n = 0;

	if (len > CH341A_I2C_MAX)
		return -1;
	buf[n++] = CH341A_CMD_I2C_STREAM;
	buf[n++] = CH341A_CMD_I2C_STM_START;
	buf[n++] = CH341A_CMD_I2C_STM_OUT | (len + 1);
	buf[n++] = addr << 1;
	memcpy(buf + n, data, len);
	n += len;
	buf[n++] = CH341A_CMD_I2C_STM_STOP;
	buf[n++] = CH341A_CMD_I2C_STM_END;
	return usb_transfer(__func__, n, 0, buf, NULL) < 0 ? -1 : 0;
}

int ch341a_i2c_read(unsigned char addr, unsigned char *data, unsigned int len)
{
	uint8_t buf[CH341_PACKET_LENGTH], in[CH341_PACKET_LENGTH];
	unsigned int n = 0;

	if (len > CH341A_I2C_MAX)
		return -1;
	buf[n++] = CH341A_CMD_I2C_STREAM;
	buf[n++] = CH341A_CMD_I2C_STM_START;
	buf[n++] = CH341A_CMD_I2C_STM_OUT;		/* the address, with its ACK */
	buf[n++] = addr << 1 | 1;
	if (len > 1)
		buf[n++] = CH341A_CMD_I2C_STM_IN | (len - 1);
	if (len)
		buf[n++] = CH341A_CMD_I2C_STM_IN;
	buf[n++] = CH341A_CMD_I2C_STM_STOP;
	buf[n++] = CH341A_CMD_I2C_STM_END;
	if (usb_transfer(__func__, n, len + 1, buf, in) < 0 || (in[0] & 0x80))
		return -1;
	if (len)
		memcpy(data, in + 1, len);
	return 0;
}

/* ch341 requires LSB first, swap the bit order before send and after receive */
static uint8_t swap_byte(uint8_t x)
{
	x = ((x >> 1) & 0x55) | ((x << 1) & 0xaa);
	x = ((x >> 2) & 0x33) | ((x << 2) & 0xcc);
	x = ((x >> 4) & 0x0f) | ((x << 4) & 0xf0);
	return x;
}

/* The assumed map between UIO command bits, pins on CH341A chip and pins on SPI chip:
 * UIO	CH341A	SPI	CH341A SPI name
 * 0	D0/15	CS/1 	(CS0)
 * 1	D1/16	unused	(CS1)
 * 2	D2/17	unused	(CS2)
 * 3	D3/18	SCK/6	(DCK)
 * 4	D4/19	unused	(DOUT2)
 * 5	D5/20	SI/5	(DOUT)
 * - The UIO stream commands seem to only have 6 bits of output, and D6/D7 are the SPI inputs,
 *  mapped as follows:
 *	D6/21	unused	(DIN2)
 *	D7/22	SO/2	(DIN)
 */
int enable_pins(bool enable)
{
	uint8_t buf[] = {
		CH341A_CMD_UIO_STREAM,
		CH341A_CMD_UIO_STM_OUT | 0x37, // CS high (all of them), SCK=0, DOUT*=1
		CH341A_CMD_UIO_STM_OUT | 0x37, // CS high (all of them), SCK=0, DOUT*=1
		CH341A_CMD_UIO_STM_OUT | 0x37, // CS high (all of them), SCK=0, DOUT*=1
		CH341A_CMD_UIO_STM_OUT | 0x37, // CS high (all of them), SCK=0, DOUT*=1
		CH341A_CMD_UIO_STM_OUT | 0x37, // CS high (all of them), SCK=0, DOUT*=1
		CH341A_CMD_UIO_STM_OUT | 0x36, // CS low (all of them), SCK=0, DOUT*=1
		CH341A_CMD_UIO_STM_DIR | (enable ? 0x3F : 0x00), // Interface output enable / disable
		CH341A_CMD_UIO_STM_END,
	};

	int32_t ret = usb_transfer(__func__, sizeof(buf), 0, buf, NULL);
	if (ret < 0) {
		printf("Could not %sable output pins.\n", enable ? "en" : "dis");
	}
	return ret;
}

/*
 * Chip select rides with the data: CS low is not a USB round trip of its
 * own but the pin commands at the start of the next stream (CS high, then
 * low, as enable_pins(true) sends them), and CS high is the CS high of that
 * same sequence, or of enable_pins(false) at shutdown. On the bus the order
 * is the same: the command ends (a page program or an erase starts) right
 * before the next command begins.
 */
static bool cs_start_pending;

int ch341a_cs(bool low)
{
	if (low)
		cs_start_pending = true;	/* sent with the next stream */
	return 0;
}

static int ch341a_spi_send_stream(unsigned int writecnt, unsigned int readcnt, const unsigned char *writearr, unsigned char *readarr)
{
	int32_t ret = 0;

	if (handle == NULL)
		return -1;

	/* How many packets ... */
	const size_t packets = (writecnt + readcnt + CH341_PACKET_LENGTH - 2) / (CH341_PACKET_LENGTH - 1);

	/* We pluck CS/timeout handling into the first packet thus we need to allocate one extra package. */
	uint8_t wbuf[packets+1][CH341_PACKET_LENGTH];
	uint8_t rbuf[writecnt + readcnt];
	/* Initialize the write buffer to zero to prevent writing random stack contents to device. */
	memset(wbuf[0], 0, CH341_PACKET_LENGTH);
	if (cs_start_pending) {
		static const uint8_t cs_start[] = {
			CH341A_CMD_UIO_STREAM,
			CH341A_CMD_UIO_STM_OUT | 0x37,	/* CS high: the command before ends */
			CH341A_CMD_UIO_STM_OUT | 0x37,
			CH341A_CMD_UIO_STM_OUT | 0x37,
			CH341A_CMD_UIO_STM_OUT | 0x37,
			CH341A_CMD_UIO_STM_OUT | 0x37,
			CH341A_CMD_UIO_STM_OUT | 0x36,	/* CS low: this one begins */
			CH341A_CMD_UIO_STM_DIR | 0x3F,
			CH341A_CMD_UIO_STM_END,
		};
		memcpy(wbuf[0], cs_start, sizeof(cs_start));
	}

	uint8_t *ptr = wbuf[0];
	/* CS usage is optimized by doing both transitions in one packet.
	 * Final transition to deselected state is in the pin disable. */
	unsigned int write_left = writecnt;
	unsigned int read_left = readcnt;
	unsigned int p;
	for (p = 0; p < packets; p++) {
		unsigned int write_now = min(CH341_PACKET_LENGTH - 1, write_left);
		unsigned int read_now = min ((CH341_PACKET_LENGTH - 1) - write_now, read_left);
		ptr = wbuf[p + 1];
		*ptr++ = CH341A_CMD_SPI_STREAM;
		unsigned int i;
		for (i = 0; i < write_now; ++i)
			*ptr++ = swap_byte(*writearr++);
		if (read_now) {
			memset(ptr, 0xFF, read_now);
			read_left -= read_now;
		}
		write_left -= write_now;
	}

	ret = usb_transfer(__func__, CH341_PACKET_LENGTH + packets + writecnt + readcnt,
				    writecnt + readcnt, wbuf[0], rbuf);

	if (ret < 0)
		return -1;
	cs_start_pending = false;

	unsigned int i;
	for (i = 0; i < readcnt; i++) {
		*readarr++ = swap_byte(rbuf[writecnt + i]);
	}

	return 0;
}

/* ---- SPI by the pins (-S): a slow clock for a chip on its board ----
   The SPI stream above runs at the fixed clock of the CH341A (the speed bits
   of CH341A_CMD_I2C_STM_SET set only the I2C one, #108). Here the UIO stream
   moves the pins itself, SPI mode 0: per bit OUT with SCK low and the bit on
   MOSI, a delay, OUT with SCK high, a delay, IN (MISO is D7, sampled after
   the rising edge). The delay of each half period sets the clock: the clock
   given is an upper bound, the USB packets slow the real one further. */

#define	 PIN_CS_LOW	0x16	/* D1, D2, D4 high; CS (D0), SCK (D3), MOSI (D5) low */
#define	 PIN_SCK	0x08
#define	 PIN_MOSI	0x20
#define	 PIN_MISO	0x80
#define	 PIN_US_MAX	0x3F	/* the longest delay of one US command */
#define	 PIN_CMDS	(CH341_PACKET_LENGTH - 2)	/* a packet: 0xAB, the commands, END */
#define	 PIN_MAX_PACKETS	512	/* a USB transfer: 16 KB out */

static const struct { const char *name; unsigned int half_us; } ch341a_speeds[] = {
	{ "100K", 5 }, { "50K", 10 }, { "20K", 25 }, { "10K", 50 }, { "5K", 100 }, { "2K", 250 }, { "1K", 500 },
};
static unsigned int pin_half_us;	/* 0: the SPI stream at the fixed clock */

int ch341a_set_speed(const char *speed)
{
	for (unsigned i = 0; i < sizeof(ch341a_speeds) / sizeof(ch341a_speeds[0]); i++)
		if (!strcasecmp(speed, ch341a_speeds[i].name)) {
			pin_half_us = ch341a_speeds[i].half_us;
			return 0;
		}
	return -1;
}

const char *ch341a_speed_list(void) { return "100K, 50K, 20K, 10K, 5K, 2K, 1K"; }

/* the US commands of one half period */
static unsigned int pin_delays(void) { return (pin_half_us + PIN_US_MAX - 1) / PIN_US_MAX; }

static uint8_t *pin_delay(uint8_t *q)
{
	unsigned int left = pin_half_us, n = pin_delays();

	for (unsigned int i = 0; i < n; i++) {
		unsigned int now = (left + (n - i) - 1) / (n - i);	/* spread evenly, each 1..63 us */
		*q++ = CH341A_CMD_UIO_STM_US | now;
		left -= now;
	}
	return q;
}

/* the bits one packet carries */
static unsigned int pin_bits_per_packet(void) { return PIN_CMDS / (3 + 2 * pin_delays()); }

/* the bytes of one USB transfer */
static unsigned int pin_max_part(void) { return PIN_MAX_PACKETS * pin_bits_per_packet() / 8; }

static int ch341a_pins_send(unsigned int writecnt, unsigned int readcnt, const unsigned char *writearr, unsigned char *readarr)
{
	static uint8_t wbuf[(PIN_MAX_PACKETS + 2) * CH341_PACKET_LENGTH];
	static uint8_t in[PIN_MAX_PACKETS * PIN_CMDS];
	const unsigned int bits = (writecnt + readcnt) * 8, bpp = pin_bits_per_packet();
	const unsigned int packets = (bits + bpp - 1) / bpp;
	unsigned int bit = 0;

	if (handle == NULL)
		return -1;
	if (packets > PIN_MAX_PACKETS)
		return -1;
	memset(wbuf, 0, (packets + 2) * CH341_PACKET_LENGTH);
	if (cs_start_pending) {		/* as in the stream: CS high, then low */
		uint8_t *q = wbuf;
		*q++ = CH341A_CMD_UIO_STREAM;
		for (int i = 0; i < 5; i++)
			*q++ = CH341A_CMD_UIO_STM_OUT | PIN_CS_LOW | 0x01 | PIN_MOSI;
		*q++ = CH341A_CMD_UIO_STM_OUT | PIN_CS_LOW | PIN_MOSI;
		*q++ = CH341A_CMD_UIO_STM_DIR | 0x3F;
		*q++ = CH341A_CMD_UIO_STM_END;
	}
	for (unsigned int p = 0; p < packets; p++) {
		uint8_t *q = wbuf + (p + 1) * CH341_PACKET_LENGTH;

		*q++ = CH341A_CMD_UIO_STREAM;
		for (unsigned int k = 0; k < bpp && bit < bits; k++, bit++) {
			unsigned int byte = bit / 8;
			uint8_t v = byte < writecnt ? writearr[byte] : 0xFF;
			uint8_t o = PIN_CS_LOW | ((v >> (7 - bit % 8)) & 1 ? PIN_MOSI : 0);

			*q++ = CH341A_CMD_UIO_STM_OUT | o;
			q = pin_delay(q);
			*q++ = CH341A_CMD_UIO_STM_OUT | o | PIN_SCK;
			q = pin_delay(q);
			*q++ = CH341A_CMD_UIO_STM_IN;
		}
		*q = CH341A_CMD_UIO_STM_END;
	}
	{				/* SCK back low, CS stays low */
		uint8_t *q = wbuf + (packets + 1) * CH341_PACKET_LENGTH;
		*q++ = CH341A_CMD_UIO_STREAM;
		*q++ = CH341A_CMD_UIO_STM_OUT | PIN_CS_LOW | PIN_MOSI;
		*q = CH341A_CMD_UIO_STM_END;
	}
	/* a packet a millisecond at worst, and its delays, twice over */
	const unsigned int timeout = USB_TIMEOUT + 2 * (packets + 2) * (1 + bpp * 2 * pin_half_us / 1000 + 1);

	if (usb_xfer(__func__, (packets + 2) * CH341_PACKET_LENGTH, bits, wbuf, in, bpp, timeout) < 0)
		return -1;
	cs_start_pending = false;
	for (unsigned int i = 0; i < readcnt; i++) {
		uint8_t v = 0;
		for (unsigned int b = 0; b < 8; b++)
			v = v << 1 | !!(in[(writecnt + i) * 8 + b] & PIN_MISO);
		readarr[i] = v;
	}
	return 0;
}

/* Send writecnt bytes, then read readcnt bytes, in parts of at most
   one transfer: the write part first, then the reads. */
int ch341a_spi_send_command(unsigned int writecnt, unsigned int readcnt, const unsigned char *writearr, unsigned char *readarr)
{
	int (*send)(unsigned int, unsigned int, const unsigned char *, unsigned char *) =
		pin_half_us ? ch341a_pins_send : ch341a_spi_send_stream;
	const unsigned int max_part = pin_half_us ? pin_max_part() : CH341_MAX_STREAM;

	while (writecnt > max_part) {
		if (send(max_part, 0, writearr, NULL))
			return -1;
		writearr += max_part;
		writecnt -= max_part;
	}
	while (writecnt + readcnt > max_part) {
		unsigned int now = max_part - writecnt;

		if (send(writecnt, now, writearr, readarr))
			return -1;
		readarr += now;
		readcnt -= now;
		writecnt = 0;
	}
	return send(writecnt, readcnt, writearr, readarr);
}

int ch341a_spi_shutdown(void)
{
	if (handle == NULL)
		return -1;

	enable_pins(false);
	libusb_free_transfer(transfer_out);
	transfer_out = NULL;
	int i;
	for (i = 0; i < USB_IN_TRANSFERS; i++) {
		libusb_free_transfer(transfer_ins[i]);
		transfer_ins[i] = NULL;
	}
	libusb_release_interface(handle, 0);
	if (kernel_driver_detached) {
		libusb_attach_kernel_driver(handle, 0);	/* give the device back to the kernel driver */
		kernel_driver_detached = 0;
	}
	libusb_close(handle);
	libusb_exit(NULL);
	handle = NULL;
	return 0;
}

int ch341a_spi_init(void)
{
	if (handle != NULL) {
		printf("%s: handle already set!\n", __func__);
		return -1;
	}

	int32_t ret = ms_usb_init();
	if (ret < 0) {
		printf("Couldnt initialize libusb!\n");
		return -1;
	}
#if LIBUSB_API_VERSION >= 0x01000106
	libusb_set_option(NULL, LIBUSB_OPTION_LOG_LEVEL, 3);
#else
	libusb_set_debug(NULL, 3); // Enable information, warning and error messages (only).
#endif
	uint16_t vid = devs_ch341a_spi[0].vendor_id;
	uint16_t pid = devs_ch341a_spi[0].device_id;
	handle = ms_usb_open(vid, pid);
	if (handle == NULL) {
		printf("Couldn't open device %04x:%04x.\n", vid, pid);
		return -1;
	}
	printf("Found programmer device: %s - %s\n", devs_ch341a_spi[0].vendor_name, devs_ch341a_spi[0].device_name);

#ifdef __linux__
	/* libusb_detach_kernel_driver() and friends basically only work on Linux (__linux__: musl does not
	 * define __gnu_linux__, so the static release builds skipped this). We simply try to detach on Linux
	 * without a lot of passion here. If that works fine else we will fail on claiming the interface anyway. */
	ret = libusb_detach_kernel_driver(handle, 0);
	kernel_driver_detached = (ret == 0);
	if (ret == LIBUSB_ERROR_NOT_SUPPORTED) {
		printf("Detaching kernel drivers is not supported. Further accesses may fail.\n");
	} else if (ret != 0 && ret != LIBUSB_ERROR_NOT_FOUND) {
		printf("Failed to detach kernel driver: '%s'. Further accesses will probably fail.\n",
			  libusb_error_name(ret));
	}
#endif

	ret = libusb_claim_interface(handle, 0);
	if (ret != 0) {
		printf("Failed to claim interface 0: '%s'\n", libusb_error_name(ret));
		goto close_handle;
	}

	struct libusb_device *dev;
	if (!(dev = libusb_get_device(handle))) {
		printf("Failed to get device from device handle.\n");
		goto release_interface;
	}

	struct libusb_device_descriptor desc;
	ret = libusb_get_device_descriptor(dev, &desc);
	if (ret < 0) {
		printf("Failed to get device descriptor: '%s'\n", libusb_error_name(ret));
		goto release_interface;
	}

	printf("Device revision is %d.%01d.%01d\n",
		(desc.bcdDevice >> 8) & 0x00FF,
		(desc.bcdDevice >> 4) & 0x000F,
		(desc.bcdDevice >> 0) & 0x000F);

	/* Allocate and pre-fill transfer structures. */
	transfer_out = libusb_alloc_transfer(0);
	if (!transfer_out) {
		printf("Failed to alloc libusb OUT transfer\n");
		goto release_interface;
	}
	int i;
	for (i = 0; i < USB_IN_TRANSFERS; i++) {
		transfer_ins[i] = libusb_alloc_transfer(0);
		if (transfer_ins[i] == NULL) {
			printf("Failed to alloc libusb IN transfer %d\n", i);
			goto dealloc_transfers;
		}
	}
	/* We use these helpers but dont fill the actual buffer yet. */
	libusb_fill_bulk_transfer(transfer_out, handle, WRITE_EP, NULL, 0, cb_out, NULL, USB_TIMEOUT);
	for (i = 0; i < USB_IN_TRANSFERS; i++)
		libusb_fill_bulk_transfer(transfer_ins[i], handle, READ_EP, NULL, 0, cb_in, NULL, USB_TIMEOUT);

	if ((config_stream(CH341A_STM_I2C_750K) < 0) || (enable_pins(true) < 0))
		goto dealloc_transfers;

	return 0;

dealloc_transfers:
	for (i = 0; i < USB_IN_TRANSFERS; i++) {
		if (transfer_ins[i] == NULL)
			break;
		libusb_free_transfer(transfer_ins[i]);
		transfer_ins[i] = NULL;
	}
	libusb_free_transfer(transfer_out);
	transfer_out = NULL;
release_interface:
	libusb_release_interface(handle, 0);
close_handle:
	if (kernel_driver_detached) {
		libusb_attach_kernel_driver(handle, 0);
		kernel_driver_detached = 0;
	}
	libusb_close(handle);
	handle = NULL;
	return -1;
}
/* End of [ch341a_spi.c] package */

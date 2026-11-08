/*
 * ch347_spi.c — SPI over the WCH CH347T (mode 1, interface 2) and CH347F
 * (interface 4) with libusb, Linux only.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Protocol: every command is a bulk OUT packet "cmd, length (u16 LE), data"
 * on endpoint 0x06; the chip answers on 0x86. SPI_OUT and SPI_SET_CFG are
 * answered with a 4-byte status, SPI_IN with packets "cmd, length, data",
 * SPI_OUT_IN with one packet "cmd, length, the bytes clocked in".
 *
 * CS in the frames: firmware 0341 and newer of the CH347T, and the CH347F,
 * take the chip select in the top bits of the length of a SPI_OUT_IN frame:
 * bit 7 sets CS low before the frame, bit 6 sets it high after it. A write
 * then goes as SPI_OUT_IN frames, its last frame held until CS high and sent
 * with it, so a write enable or a page program is one round trip instead of
 * a round trip and a CS packet. INFO_RD gives the firmware version.
 */
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include "usb.h"
#include "ch347_spi.h"

#define CH347_VID		0x1a86
#define CH347T_PID		0x55db
#define CH347F_PID		0x55de
#define CH347T_IFACE		2	/* mode 1: UART + SPI + I2C */
#define CH347F_IFACE		4

#define CH347_CMD_SPI_SET_CFG	0xc0
#define CH347_CMD_SPI_CS_CTRL	0xc1
#define CH347_CMD_SPI_OUT_IN	0xc2	/* full duplex: a byte read for every byte sent */
#define CH347_CMD_SPI_IN	0xc3
#define CH347_CMD_SPI_OUT	0xc4
#define CH347_CMD_INFO_RD	0xca	/* chip information: the firmware version at bytes 3 and 4 */

#define CH347_C2_CS_START	0x80	/* in the length high byte of SPI_OUT_IN: CS low before it */
#define CH347_C2_CS_STOP	0x40	/* CS high after it */
#define CH347_C2_LEN_HI		0x0f	/* the length bits of that byte */
#define CH347T_CS_FLAGS_FW	0x0341	/* the first CH347T firmware that takes them */
#define CH347_INFO_TIMEOUT	300	/* ms: firmware that does not know INFO_RD does not answer */

#define CH347_CS_ASSERT		0x00
#define CH347_CS_DEASSERT	0x40
#define CH347_CS_CHANGE		0x80

#define CH347_WRITE_EP		0x06
#define CH347_READ_EP		0x86
#define CH347_PACKET_SIZE	510
#define CH347_MAX_DATA		(CH347_PACKET_SIZE - 3)
#define CH347_CS_LEN		13
#define CH347_TIMEOUT		2000	/* ms */

static libusb_device_handle *ch347;
static int ch347_iface = -1;
static int ch347_detached;
/* CS low is not sent on its own: it goes in front of the next command, in
   the same USB transfer (one round trip less for every SPI transaction) */
static int cs_pending;
static int ch347_divisor = 1;		/* 30 MHz: within the 50 MHz of READ (0x03) of most chips */
static int cs_flags;			/* the firmware takes CS in the SPI_OUT_IN frames */
/* with cs_flags: the last frame of a write, held until CS high goes with it,
   or until the next command */
static unsigned char wpend[CH347_MAX_DATA];
static unsigned int wpend_n;
static int flush_write(int stop);

static const struct { const char *name; int divisor; } ch347_speeds[] = {
	{ "60M", 0 }, { "30M", 1 }, { "15M", 2 }, { "7.5M", 3 }, { "3.75M", 4 }, { "1.875M", 5 },
	{ "937.5K", 6 }, { "468.75K", 7 },
};

int ch347_set_speed(const char *speed)
{
	for (unsigned i = 0; i < sizeof(ch347_speeds) / sizeof(ch347_speeds[0]); i++)
		if (!strcasecmp(speed, ch347_speeds[i].name)) {
			ch347_divisor = ch347_speeds[i].divisor;
			return 0;
		}
	return -1;
}

const char *ch347_speed_list(void) { return "60M, 30M, 15M, 7.5M, 3.75M, 1.875M, 937.5K, 468.75K"; }

static int bulk_t(unsigned char ep, unsigned char *buf, int len, int *done, unsigned int timeout)
{
	int n = 0;
	int ret = libusb_bulk_transfer(ch347, ep, buf, len, &n, timeout);

	if (done)
		*done = n;
	if (ret < 0) {
		printf("CH347: USB transfer on 0x%02x failed: %s\n", ep, libusb_error_name(ret));
		return -1;
	}
	if (!done && n != len) {
		printf("CH347: short USB transfer on 0x%02x: %d of %d bytes\n", ep, n, len);
		return -1;
	}
	return 0;
}

static int bulk(unsigned char ep, unsigned char *buf, int len, int *done)
{
	return bulk_t(ep, buf, len, done, CH347_TIMEOUT);
}

/* The firmware version from INFO_RD, -1 when it does not come; quiet, as old
   firmware may not answer at all */
static int ch347_firmware(void)
{
	unsigned char cmd[4] = { CH347_CMD_INFO_RD, 1, 0, 0 }, r[CH347_PACKET_SIZE];
	int n = 0;

	if (bulk(CH347_WRITE_EP, cmd, sizeof(cmd), NULL) ||
	    libusb_bulk_transfer(ch347, CH347_READ_EP, r, sizeof(r), &n, CH347_INFO_TIMEOUT) < 0)
		return -1;
	if (n < 5 || r[0] != CH347_CMD_INFO_RD)
		return -1;
	return r[3] | (r[4] << 8);
}

/* SPI mode 0, MSB first, 8 bits, CS active low, clock 60 MHz >> divisor */
static int ch347_configure(void)
{
	unsigned char cfg[29] = {
		[0] = CH347_CMD_SPI_SET_CFG,
		[1] = sizeof(cfg) - 3,
		/* bytes 5, 6, 14 and 19: the values the vendor driver always sends;
		   clock polarity (9), phase (11), bit order (17) and CS polarity (24)
		   stay 0: mode 0, MSB first, CS active low */
		[5] = 4,
		[6] = 1,
		[14] = 2,
		[19] = 7,
	};
	/* the answer is 4 bytes (command, length, status); some firmware sends
	   the whole configuration back: room for it, or the rest would come
	   with the next read */
	unsigned char resp[sizeof(cfg)];
	int n;

	cfg[15] = (ch347_divisor & 7) << 3;
	if (bulk(CH347_WRITE_EP, cfg, sizeof(cfg), NULL) || bulk(CH347_READ_EP, resp, sizeof(resp), &n))
		return -1;
	if (n < 4 || resp[0] != CH347_CMD_SPI_SET_CFG || resp[3] != 0) {
		printf("CH347: SPI configuration refused\n");
		return -1;
	}
	return 0;
}

int ch347_spi_init(void)
{
	static const unsigned short pids[] = { CH347T_PID, CH347F_PID };
	unsigned short pid = 0;
	int ret;

	if (ch347)
		return -1;
	if (ms_usb_init() < 0) {
		printf("Could not initialize libusb!\n");
		return -1;
	}
	for (unsigned i = 0; i < sizeof(pids) / sizeof(pids[0]) && !ch347; i++)
		if ((ch347 = ms_usb_open(CH347_VID, pids[i])))
			pid = pids[i];
	if (!ch347) {
		printf("Couldn't open a CH347 (%04x:%04x or %04x:%04x).\n", CH347_VID, CH347T_PID, CH347_VID, CH347F_PID);
		goto exit;
	}
	ch347_iface = pid == CH347F_PID ? CH347F_IFACE : CH347T_IFACE;
	printf("Found programmer device: WinChipHead (WCH) - %s, SPI interface %d, %s\n",
	       pid == CH347F_PID ? "CH347F" : "CH347T", ch347_iface, ch347_speeds[ch347_divisor].name);

	ret = libusb_detach_kernel_driver(ch347, ch347_iface);
	ch347_detached = ret == 0;
	if (ret != 0 && ret != LIBUSB_ERROR_NOT_FOUND && ret != LIBUSB_ERROR_NOT_SUPPORTED)
		printf("Failed to detach kernel driver: '%s'. Further accesses will probably fail.\n", libusb_error_name(ret));
	ret = libusb_claim_interface(ch347, ch347_iface);
	if (ret != 0) {
		printf("Failed to claim interface %d: '%s'%s\n", ch347_iface, libusb_error_name(ret),
		       pid == CH347T_PID ? " (is the CH347T in mode 1?)" : "");
		goto close;
	}
	if (ch347_configure() < 0)
		goto release;
	cs_pending = 0;
	wpend_n = 0;
	{
		int fw = ch347_firmware();

		cs_flags = pid == CH347F_PID || fw >= CH347T_CS_FLAGS_FW;
		if (fw >= 0)
			printf("CH347 firmware %04x%s\n", fw, cs_flags ? ": CS goes in the data frames" : "");
	}
	return 0;

release:
	libusb_release_interface(ch347, ch347_iface);
close:
	if (ch347_detached)
		libusb_attach_kernel_driver(ch347, ch347_iface);
	ch347_detached = 0;
	libusb_close(ch347);
	ch347 = NULL;
exit:
	libusb_exit(NULL);
	return -1;
}

int ch347_spi_shutdown(void)
{
	if (!ch347)
		return -1;
	ch347_cs(false);
	libusb_release_interface(ch347, ch347_iface);
	if (ch347_detached)
		libusb_attach_kernel_driver(ch347, ch347_iface);	/* give it back to the kernel */
	ch347_detached = 0;
	libusb_close(ch347);
	ch347 = NULL;
	libusb_exit(NULL);
	return 0;
}

static int cs_packet(unsigned char *cmd, bool low)
{
	memset(cmd, 0, CH347_CS_LEN);
	cmd[0] = CH347_CMD_SPI_CS_CTRL;
	cmd[1] = CH347_CS_LEN - 3;
	cmd[3] = (low ? CH347_CS_ASSERT : CH347_CS_DEASSERT) | CH347_CS_CHANGE;	/* CS1; CS2 is left as it is */
	return CH347_CS_LEN;
}

int ch347_cs(bool low)
{
	unsigned char cmd[CH347_CS_LEN];

	if (!ch347)
		return -1;
	if (low) {
		if (flush_write(1))	/* not expected: a CS low with no CS high before it */
			return -1;
		cs_pending = 1;
		return 0;
	}
	if (wpend_n)			/* CS high in the last frame of the write */
		return flush_write(1);
	if (cs_pending) {		/* nothing went out between: CS never went low */
		cs_pending = 0;
		return 0;
	}
	return bulk(CH347_WRITE_EP, cmd, cs_packet(cmd, false), NULL);
}

/* the commands of pkt in one USB transfer, a CS low still to go in front */
static int send_out(const unsigned char *pkt, int len)
{
	unsigned char buf[CH347_PACKET_SIZE];
	int k = 0;

	if (cs_pending) {
		cs_pending = 0;
		k = cs_packet(buf, true);
		if (k + len > CH347_PACKET_SIZE) {
			if (bulk(CH347_WRITE_EP, buf, k, NULL))
				return -1;
			k = 0;
		}
	}
	if (!k)
		return bulk(CH347_WRITE_EP, (unsigned char *)pkt, len, NULL);
	memcpy(buf + k, pkt, len);
	return bulk(CH347_WRITE_EP, buf, k + len, NULL);
}

/* With cs_flags: one SPI_OUT_IN frame of a write, with a CS low that is still
   to go and, when stop, CS high after it; the bytes clocked in are dropped */
static int c2_write(const unsigned char *data, unsigned int n, int stop)
{
	unsigned char pkt[CH347_PACKET_SIZE];
	int got;

	pkt[0] = CH347_CMD_SPI_OUT_IN;
	pkt[1] = n & 0xff;
	pkt[2] = (n >> 8) | (cs_pending ? CH347_C2_CS_START : 0) | (stop ? CH347_C2_CS_STOP : 0);
	memcpy(pkt + 3, data, n);
	cs_pending = 0;
	if (bulk(CH347_WRITE_EP, pkt, n + 3, NULL) || bulk(CH347_READ_EP, pkt, sizeof(pkt), &got))
		return -1;
	if (got != (int)n + 3 || pkt[0] != CH347_CMD_SPI_OUT_IN || (pkt[1] | ((pkt[2] & CH347_C2_LEN_HI) << 8)) != n) {
		printf("CH347: bad answer to a write (%d bytes, command %02x)\n", got, got ? pkt[0] : 0);
		return -1;
	}
	return 0;
}

/* the held frame of a write goes out: with CS high after it (stop) or not */
static int flush_write(int stop)
{
	unsigned int n = wpend_n;

	if (!n)
		return 0;
	wpend_n = 0;
	return c2_write(wpend, n, stop);
}

static int ch347_write(unsigned int len, const unsigned char *data)
{
	unsigned char pkt[CH347_PACKET_SIZE], resp[4];

	if (cs_flags) {
		if (flush_write(0))
			return -1;
		for (; len > CH347_MAX_DATA; data += CH347_MAX_DATA, len -= CH347_MAX_DATA)
			if (c2_write(data, CH347_MAX_DATA, 0))
				return -1;
		memcpy(wpend, data, len);	/* the last frame: CS high may go with it */
		wpend_n = len;
		return 0;
	}

	while (len) {
		unsigned int n = len < CH347_MAX_DATA ? len : CH347_MAX_DATA;
		int got;

		pkt[0] = CH347_CMD_SPI_OUT;
		pkt[1] = n & 0xff;
		pkt[2] = n >> 8;
		memcpy(pkt + 3, data, n);
		if (send_out(pkt, n + 3) || bulk(CH347_READ_EP, resp, sizeof(resp), &got))
			return -1;
		/* the answer: SPI_OUT, length 1, status 0 */
		if (got != sizeof(resp) || resp[0] != CH347_CMD_SPI_OUT || resp[3] != 0) {
			printf("CH347: write refused (answer %d bytes: %02x %02x %02x %02x)\n", got, resp[0], resp[1], resp[2], resp[3]);
			return -1;
		}
		data += n;
		len -= n;
	}
	return 0;
}

static int ch347_read(unsigned int len, unsigned char *data)
{
	unsigned char cmd[7] = { CH347_CMD_SPI_IN, 4, 0, len & 0xff, (len >> 8) & 0xff, (len >> 16) & 0xff, len >> 24 };
	unsigned char pkt[CH347_PACKET_SIZE];
	unsigned int done = 0;

	if (flush_write(0) || send_out(cmd, sizeof(cmd)))
		return -1;
	while (done < len) {
		int got;
		unsigned int n;

		if (bulk(CH347_READ_EP, pkt, sizeof(pkt), &got))
			return -1;
		if (got < 3 || pkt[0] != CH347_CMD_SPI_IN) {
			printf("CH347: bad answer to a read (%d bytes, command %02x)\n", got, got ? pkt[0] : 0);
			return -1;
		}
		n = pkt[1] | (pkt[2] << 8);
		if (n == 0 || n != (unsigned int)got - 3 || n > len - done) {
			printf("CH347: read answer of %u bytes does not fit (%d received, %u left)\n", n, got - 3, len - done);
			return -1;
		}
		memcpy(data + done, pkt + 3, n);
		done += n;
	}
	return 0;
}

/* A short command and its answer in one round trip: the command bytes, then
   0xff while the answer is clocked in; the answer is the tail of what comes
   back. */
static int ch347_out_in(unsigned int writecnt, unsigned int readcnt, const unsigned char *writearr, unsigned char *readarr)
{
	unsigned char pkt[CH347_PACKET_SIZE];
	unsigned int n = writecnt + readcnt;
	int got;

	if (flush_write(0))
		return -1;
	pkt[0] = CH347_CMD_SPI_OUT_IN;
	pkt[1] = n & 0xff;
	pkt[2] = n >> 8;
	memcpy(pkt + 3, writearr, writecnt);
	memset(pkt + 3 + writecnt, 0xff, readcnt);
	if (send_out(pkt, n + 3) || bulk(CH347_READ_EP, pkt, sizeof(pkt), &got))
		return -1;
	if (got != (int)n + 3 || pkt[0] != CH347_CMD_SPI_OUT_IN || (pkt[1] | (pkt[2] << 8)) != n) {
		printf("CH347: bad answer to a transfer (%d bytes, command %02x)\n", got, got ? pkt[0] : 0);
		return -1;
	}
	memcpy(readarr, pkt + 3 + writecnt, readcnt);
	return 0;
}

int ch347_spi_send_command(unsigned int writecnt, unsigned int readcnt, const unsigned char *writearr, unsigned char *readarr)
{
	if (!ch347)
		return -1;
	if (writecnt && readcnt && writecnt + readcnt <= CH347_MAX_DATA)
		return ch347_out_in(writecnt, readcnt, writearr, readarr);
	if (writecnt && ch347_write(writecnt, writearr))
		return -1;
	if (readcnt && ch347_read(readcnt, readarr))
		return -1;
	return 0;
}

/* ---- I2C, for the SigmaStar ISP ----
   The stream commands of the CH341 (0xAA ...), on the same interface as the
   SPI. Every byte sent with STM_OUT answers 0x01 for ACK; a read answers its
   address ACK and then the data. The CH347F first switches its pins to I2C. */

#define CH347_I2C_STREAM	0xAA
#define CH347_I2C_STM_SET	0x60
#define CH347_I2C_STM_START	0x74
#define CH347_I2C_STM_STOP	0x75
#define CH347_I2C_STM_OUT	0x80
#define CH347_I2C_STM_IN	0xC0
#define CH347_I2C_STM_END	0x00

int ch347_i2c_speed(unsigned int speed)
{
	unsigned char pkt[16] = { CH347_I2C_STREAM, CH347_I2C_STM_SET | (speed & 3), CH347_I2C_STM_END };

	if (!ch347)
		return -1;
	if (ch347_iface == CH347F_IFACE) {
		static const unsigned char to_i2c[11] = { 0xe2, 0x08, 0x00, 0x00, 0x00, 0x81, 0x81, 0x00, 0x00, 0x00, 0x00 };
		unsigned char cmd[sizeof(to_i2c)], ans[4];
		int n = 0;

		memcpy(cmd, to_i2c, sizeof(cmd));
		if (bulk(CH347_WRITE_EP, cmd, sizeof(cmd), NULL) || bulk(CH347_READ_EP, ans, sizeof(ans), &n) || n != 4) {
			printf("CH347F: the pins do not switch to I2C.\n");
			return -1;
		}
	}
	return bulk(CH347_WRITE_EP, pkt, 3, NULL) ? -1 : 0;
}

int ch347_i2c_write(unsigned char addr, const unsigned char *data, unsigned int len)
{
	unsigned char pkt[CH347_I2C_MAX + 8], ans[CH347_PACKET_SIZE];
	unsigned int n = 0;
	int got = 0;

	if (!ch347 || len > CH347_I2C_MAX)
		return -1;
	pkt[n++] = CH347_I2C_STREAM;
	pkt[n++] = CH347_I2C_STM_START;
	pkt[n++] = CH347_I2C_STM_OUT | (len + 1);
	pkt[n++] = addr << 1;
	memcpy(pkt + n, data, len);
	n += len;
	pkt[n++] = CH347_I2C_STM_STOP;
	pkt[n++] = CH347_I2C_STM_END;
	if (bulk(CH347_WRITE_EP, pkt, n, NULL) || bulk(CH347_READ_EP, ans, sizeof(ans), &got) || got != (int)len + 1)
		return -1;
	for (int i = 0; i < got; i++)
		if (ans[i] != 0x01)			/* NACK */
			return -1;
	return 0;
}

int ch347_i2c_read(unsigned char addr, unsigned char *data, unsigned int len)
{
	unsigned char pkt[16], ans[CH347_PACKET_SIZE];
	unsigned int n = 0;
	int got = 0;

	if (!ch347 || len > CH347_I2C_MAX)
		return -1;
	pkt[n++] = CH347_I2C_STREAM;
	pkt[n++] = CH347_I2C_STM_START;
	pkt[n++] = CH347_I2C_STM_OUT | 1;
	pkt[n++] = addr << 1 | 1;
	if (len > 1)
		pkt[n++] = CH347_I2C_STM_IN | (len - 1);
	if (len)
		pkt[n++] = CH347_I2C_STM_IN;
	pkt[n++] = CH347_I2C_STM_STOP;
	pkt[n++] = CH347_I2C_STM_END;
	if (bulk(CH347_WRITE_EP, pkt, n, NULL) || bulk(CH347_READ_EP, ans, sizeof(ans), &got) ||
	    got != (int)len + 1 || ans[0] != 0x01)
		return -1;
	if (len)
		memcpy(data, ans + 1, len);
	return 0;
}

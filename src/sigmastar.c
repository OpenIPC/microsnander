/*
 * sigmastar.c — the SigmaStar (MStar) ISP: the SPI flash of a board on its
 * SoC, reached through the SoC's ISP bridge over I2C, without desoldering
 * (-m sigmastar). The I2C master is the programmer's (CH341A, CH347): the
 * UART of the SoC goes to it, TX to SDA, RX to SCL, and GND.
 *
 * The bridge answers at 0x49: 0x10 and bytes — they go out on the SPI bus;
 * 0x11 — the next I2C read clocks bytes in from it; 0x12 — the end of the
 * SPI command (CS high). Before that the SoC's debug port, 0x59, is told to
 * let the pins of its UART be the bridge, and "MSTAR" to 0x49 starts it.
 * The flash drivers do not know: the programmer's send and cs go here.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include "programmer.h"
#include "sigmastar.h"

#define ISP_ADDR	0x49
#define ISP_DEBUG	0x59
#define ISP_WRITE	0x10
#define ISP_READ	0x11
#define ISP_END		0x12

static bool on;				/* -m sigmastar */
static int speed = 1;			/* 100 kHz */
static const struct programmer *bus;	/* the programmer whose I2C it is */
static struct programmer isp;		/* bus with send and cs of the bridge */

static const struct { const char *name; int code; } speeds[] = {
	{ "20K", 0 }, { "100K", 1 }, { "400K", 2 }, { "750K", 3 },
};

int sigmastar_mode(const char *name)
{
	if (strcasecmp(name, "sigmastar"))
		return -1;
	on = true;
	return 0;
}

bool sigmastar_on(void) { return on; }

const char *sigmastar_speed_list(void) { return "20K, 100K (default), 400K, 750K"; }

int sigmastar_set_speed(const char *name)
{
	for (unsigned i = 0; i < sizeof(speeds) / sizeof(speeds[0]); i++)
		if (!strcasecmp(name, speeds[i].name)) {
			speed = speeds[i].code;
			return 0;
		}
	return -1;
}

/* the SPI command: bytes out, then bytes in, in parts that fit the I2C of
   the programmer; CS stays low until isp_cs(false) */
static int isp_send(unsigned int writecnt, unsigned int readcnt, const unsigned char *writearr, unsigned char *readarr)
{
	unsigned char buf[64];
	unsigned int part = bus->i2c_max - 1;	/* the command byte first */

	for (unsigned int done = 0; done < writecnt; ) {
		unsigned int n = writecnt - done < part ? writecnt - done : part;

		buf[0] = ISP_WRITE;
		memcpy(buf + 1, writearr + done, n);
		if (bus->i2c_write(ISP_ADDR, buf, n + 1))
			return -1;
		done += n;
	}
	for (unsigned int done = 0; done < readcnt; ) {
		unsigned int n = readcnt - done < bus->i2c_max ? readcnt - done : bus->i2c_max;

		buf[0] = ISP_READ;
		if (bus->i2c_write(ISP_ADDR, buf, 1) || bus->i2c_read(ISP_ADDR, readarr + done, n))
			return -1;
		done += n;
	}
	return 0;
}

static int isp_cs(bool low)
{
	unsigned char end = ISP_END;

	return low ? 0 : bus->i2c_write(ISP_ADDR, &end, 1);
}

/* the debug port lets the UART pins of the SoC go to the ISP bridge */
static int debug_port_to_isp(void)
{
	static const unsigned char serdb[] = { 'S', 'E', 'R', 'D', 'B' };
	static const unsigned char init[] = { 0x81, 0x83, 0x84, 0x53, 0x7F, 0x35, 0x71 };
	static const unsigned char uart[] = { 0x10, 0x00, 0x00, 0x0E, 0x12, 0x00, 0x00 };
	static const unsigned char leave[] = { 0x34, 0x45 };
	unsigned char reg;

	if (bus->i2c_write(ISP_DEBUG, serdb, sizeof(serdb)))
		return -1;
	for (unsigned i = 0; i < sizeof(init); i++)
		if (bus->i2c_write(ISP_DEBUG, init + i, 1))
			return -1;
	if (bus->i2c_write(ISP_DEBUG, uart, sizeof(uart)) || bus->i2c_write(ISP_DEBUG, uart, 6) ||
	    bus->i2c_read(ISP_DEBUG, &reg, 1) || reg)
		return -1;
	for (unsigned i = 0; i < sizeof(leave); i++)
		if (bus->i2c_write(ISP_DEBUG, leave + i, 1))
			return -1;
	return 0;
}

int i2c_scan(const struct programmer *p)
{
	if (!p->i2c_read) {
		printf("The %s has no I2C: -q works with the CH341A and the CH347.\n", p->name);
		return -1;
	}
	if (p->i2c_speed(speed))
		return -1;
	printf("I2C devices on the %s:\n     0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f", p->name);
	for (int a = 0; a < 0x80; a++) {
		if (a % 16 == 0)
			printf("\n%02x: ", a);
		if (p->i2c_read(a, NULL, 0))
			printf("-- ");
		else
			printf("%02x ", a);
	}
	printf("\n\n");
	return 0;
}

const struct programmer *sigmastar_attach(const struct programmer *p)
{
	static const unsigned char start[] = { 'M', 'S', 'T', 'A', 'R' };

	if (!p->i2c_write) {
		printf("The %s has no I2C: -m sigmastar works with the CH341A and the CH347.\n", p->name);
		return NULL;
	}
	bus = p;
	if (p->i2c_speed(speed))
		return NULL;
	if (p->i2c_read(ISP_ADDR, NULL, 0)) {
		printf("SigmaStar ISP: nothing answers at I2C 0x%02x. Check the wires (TX to SDA, RX to SCL, GND)\n"
		       "and that the board is powered; a power cycle of the board can help.\n", ISP_ADDR);
		return NULL;
	}
	if (p->i2c_read(ISP_DEBUG, NULL, 0) && debug_port_to_isp())
		printf("SigmaStar ISP: the debug port 0x%02x did not take the UART pins; going on.\n", ISP_DEBUG);
	if (p->i2c_write(ISP_ADDR, start, sizeof(start)) && isp_cs(false))
		return NULL;
	printf("SigmaStar ISP through the I2C of the %s at 0x%02x.\n", p->name, ISP_ADDR);
	isp = *p;
	isp.send = isp_send;
	isp.cs = isp_cs;
	isp.clock = false;
	isp.flash_init = NULL;
	return &isp;
}

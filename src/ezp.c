/*
 * ezp.c — EZP2019, EZP2019+ and EZP2023 programmers (USB 1fc8:310b, 310c,
 * 310d) with libusb, Linux only. The USB ID does not tell the model: an
 * EZP2019+ reports 310b.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * These programmers run the SPI bus in their own firmware: the host asks for
 * whole operations, not SPI commands. What they do: tell the JEDEC ID, read
 * or program a stream from an address, erase the whole chip, tell busy or
 * ready. What they do not: erase a sector, read or write a status register,
 * SPI NAND. So they are not an SPI bridge for spi_controller.c: the NOR
 * table gives the chip, and the read, write and erase here work on it
 * directly. Erasing a part of the chip is refused, and so is programming a
 * range that is not erased (programming only clears bits).
 *
 * USB: interface 0. A command is a 64-byte packet on bulk OUT 0x02:
 *   [1] operation, [2..3] bus (LE, 0: SPI), [4..5] page size (BE),
 *   [6..7] timeout (BE), [8..11] chip size or start address (BE),
 *   [12..15] chip ID (BE).
 * Answers are 64-byte packets on bulk IN 0x82; read data comes on 0x82 too,
 * program data goes to bulk OUT 0x01.
 */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <libusb-1.0/libusb.h>
#include "ezp.h"
#include "flashcmd_api.h"
#include "snorcmd_api.h"
#include "timer.h"

#define EZP_VID			0x1fc8

#define EZP_EP_DATA_OUT		0x01
#define EZP_EP_CMD		0x02
#define EZP_EP_IN		0x82

#define EZP_PKT			64
#define EZP_STREAM_UNIT		256	/* read data comes in whole 256-byte units */
#define EZP_READ_PART		(64 * 1024)
#define EZP_USB_TIMEOUT		5000	/* ms */

#define EZP_OP_CHIP_ERASE	0x02
#define EZP_OP_START		0x05	/* start the stream at an address */
#define EZP_OP_SETUP		0x07	/* the chip geometry for a read or a program */
#define EZP_OP_RESET		0x08
#define EZP_OP_DETECT		0x09	/* answers the JEDEC ID in [1..3] */
#define EZP_OP_STATUS		0x0a	/* answer [0] bit 0: busy */

#define EZP_PAGE		256
#define EZP_CHIP_TIMEOUT	1000

extern unsigned int bsize;

static libusb_device_handle *ezp;
static int ezp_detached;
static unsigned int ezp_id;			/* JEDEC ID, 3 bytes */
static unsigned int ezp_chip_id;		/* the ID the chip was set up with */
static unsigned long ezp_size;

static const unsigned short ezp_pids[] = { 0x310b, 0x310c, 0x310d };

static void put_be(unsigned char *p, unsigned int v, int n)
{
	while (n--)
		*p++ = v >> (8 * n);
}

/* A command packet with the chip geometry, as the setup and detect take it */
static void ezp_packet(unsigned char *pkt, unsigned char op, unsigned int size_or_addr)
{
	memset(pkt, 0, EZP_PKT);
	pkt[1] = op;
	put_be(pkt + 4, EZP_PAGE, 2);
	put_be(pkt + 6, EZP_CHIP_TIMEOUT, 2);
	put_be(pkt + 8, size_or_addr, 4);
	put_be(pkt + 12, ezp_id, 4);
}

static int ezp_bulk(unsigned char ep, unsigned char *buf, int len, int *done)
{
	int n = 0, ret = libusb_bulk_transfer(ezp, ep, buf, len, &n, EZP_USB_TIMEOUT);

	if (done)
		*done = n;
	if (ret < 0) {
		printf("EZP: USB transfer on 0x%02x failed: %s\n", ep, libusb_error_name(ret));
		if (ret == LIBUSB_ERROR_PIPE)
			libusb_clear_halt(ezp, ep);
		return -1;
	}
	if (!done && n != len) {
		printf("EZP: short USB transfer on 0x%02x: %d of %d bytes\n", ep, n, len);
		return -1;
	}
	return 0;
}

/* Send a command; with answer, wait for its 64-byte answer */
static int ezp_command(unsigned char *pkt, unsigned char *answer)
{
	if (ezp_bulk(EZP_EP_CMD, pkt, EZP_PKT, NULL))
		return -1;
	if (answer && ezp_bulk(EZP_EP_IN, answer, EZP_PKT, NULL))
		return -1;
	return 0;
}

static long ezp_ms_since(const struct timespec *t0)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (t.tv_sec - t0->tv_sec) * 1000L + (t.tv_nsec - t0->tv_nsec) / 1000000L;
}

/* Until the programmer is done with the chip. The first answer can still be
   the state from before the operation, so ready counts from the second. */
static int ezp_wait(long timeout_ms, unsigned int poll_us)
{
	unsigned char pkt[EZP_PKT], answer[EZP_PKT];
	struct timespec t0;
	int polls = 0;

	clock_gettime(CLOCK_MONOTONIC, &t0);
	memset(pkt, 0, sizeof(pkt));
	pkt[1] = EZP_OP_STATUS;
	for (;;) {
		usleep(poll_us);
		if (ezp_command(pkt, answer))
			return -1;
		if (++polls > 1 && !(answer[0] & 1))
			return 0;
		if (ezp_ms_since(&t0) > timeout_ms) {
			printf("EZP: the chip is still busy after %ld s\n", timeout_ms / 1000);
			return -1;
		}
	}
}

static int ezp_detect(void)
{
	unsigned char pkt[EZP_PKT], answer[EZP_PKT];

	ezp_packet(pkt, EZP_OP_DETECT, ezp_size);
	if (ezp_command(pkt, answer))
		return -1;
	ezp_id = (answer[1] << 16) | (answer[2] << 8) | answer[3];
	/* every operation detects again: it must still be the chip set up */
	if (ezp_chip_id && ezp_id != ezp_chip_id) {
		printf("EZP: the chip changed (ID %06x, was %06x): run again.\n", ezp_id, ezp_chip_id);
		ezp_id = ezp_chip_id;
		return -1;
	}
	return 0;
}

static int ezp_in_chip(unsigned long addr, unsigned long len)
{
	if (addr >= ezp_size || len > ezp_size - addr) {
		printf("The range 0x%lx + 0x%lx is past the chip end (0x%lx).\n", addr, len, ezp_size);
		return 0;
	}
	return 1;
}

/* Detect, set the geometry, start a stream at addr */
static int ezp_start(unsigned char start_op_answer, unsigned long addr)
{
	unsigned char pkt[EZP_PKT], answer[EZP_PKT];

	if (ezp_detect())
		return -1;
	ezp_packet(pkt, EZP_OP_SETUP, ezp_size);
	if (ezp_command(pkt, answer))
		return -1;
	memset(pkt, 0, sizeof(pkt));
	pkt[1] = EZP_OP_START;
	put_be(pkt + 8, addr, 4);
	return ezp_command(pkt, start_op_answer ? answer : NULL);
}

/* A stream starts right only at a multiple of 256: start there and drop the
   bytes before from. */
static int ezp_read_range(unsigned char *buf, unsigned long from, unsigned long len, int progress)
{
	unsigned char tail[EZP_STREAM_UNIT];
	unsigned long done = 0, skip = from % EZP_STREAM_UNIT;

	if (ezp_start(1, from - skip))
		return -1;
	if (skip) {
		int got;
		unsigned long n = len < EZP_STREAM_UNIT - skip ? len : EZP_STREAM_UNIT - skip;

		if (ezp_bulk(EZP_EP_IN, tail, sizeof(tail), &got) || got != (int)sizeof(tail))
			goto short_read;
		memcpy(buf, tail + skip, n);
		done = n;
	}
	while (done < len) {
		unsigned long left = len - done;
		int n, got;

		if (left >= EZP_STREAM_UNIT) {
			n = left < EZP_READ_PART ? left - left % EZP_STREAM_UNIT : EZP_READ_PART;
			if (ezp_bulk(EZP_EP_IN, buf + done, n, &got) || got != n)
				goto short_read;
		} else {
			/* the last part of a unit: take the whole unit */
			n = left;
			if (ezp_bulk(EZP_EP_IN, tail, sizeof(tail), &got) || got < n)
				goto short_read;
			memcpy(buf + done, tail, n);
		}
		done += n;
		if (progress && timer_progress()) {
			printf("\bRead %ld%% [%lu] of [%lu] bytes      ", 100 * done / len, done, len);
			printf("\r");
			fflush(stdout);
		}
	}
	return 0;

short_read:
	printf("EZP: read stopped at 0x%lx\n", from + done);
	return -1;
}

static int ezp_read(unsigned char *buf, unsigned long from, unsigned long len)
{
	int ret;

	if (!len)
		return 0;
	if (!ezp_in_chip(from, len))
		return -1;
	timer_start();
	ret = ezp_read_range(buf, from, len, 1);
	if (!ret)
		printf("Read 100%% [%lu] of [%lu] bytes      \n", len, len);
	timer_end();
	return ret ? -1 : (int)len;
}

static int ezp_erase(unsigned long offs, unsigned long len)
{
	unsigned char pkt[EZP_PKT], answer[EZP_PKT];
	int ret;

	if (offs || len != ezp_size) {
		printf("The EZP programmers erase only the whole chip: use -e without -a and -l.\n");
		return -1;
	}
	printf("Please Wait......\n");
	timer_start();
	if (ezp_detect())
		goto fail;
	ezp_packet(pkt, EZP_OP_SETUP, ezp_size);
	if (ezp_command(pkt, answer))
		goto fail;
	memset(pkt, 0, sizeof(pkt));
	pkt[0] = 1;
	pkt[1] = EZP_OP_CHIP_ERASE;
	pkt[0x1a] = 0x80;
	if (ezp_command(pkt, NULL))
		goto fail;
	ret = ezp_wait(400 * 1000, 20000);	/* a 16 MB chip: about half a minute */
	timer_end();
	return ret;

fail:
	timer_end();
	return -1;
}

/* Programming clears bits only: the range must be erased. The ends are
   filled with 0xff to whole pages, which leaves those bytes as they are. */
static int ezp_write(unsigned char *buf, unsigned long to, unsigned long len)
{
	static unsigned char chunk[EZP_READ_PART];
	unsigned long first = to - to % EZP_PAGE, end, done;

	if (!len)
		return 0;
	if (!ezp_in_chip(to, len))
		return -1;
	end = to + len;
	end += (EZP_PAGE - end % EZP_PAGE) % EZP_PAGE;

	timer_start();
	printf("Checking that the range is erased...\n");
	for (done = first; done < end; done += sizeof(chunk)) {
		unsigned long n = end - done < sizeof(chunk) ? end - done : sizeof(chunk);

		if (ezp_read_range(chunk, done, n, 0))
			goto fail;
		for (unsigned long i = 0; i < n; i++) {
			unsigned long a = done + i;

			if (a >= to && a < to + len && chunk[i] != 0xff) {
				printf("The range is not erased (0x%02x at 0x%lx): erase the chip first (-e).\n", chunk[i], a);
				goto fail;
			}
		}
	}

	if (ezp_start(0, first))
		goto fail;
	for (done = first; done < end; done += sizeof(chunk)) {
		unsigned long n = end - done < sizeof(chunk) ? end - done : sizeof(chunk);

		memset(chunk, 0xff, n);
		for (unsigned long i = 0; i < n; i++) {
			unsigned long a = done + i;

			if (a >= to && a < to + len)
				chunk[i] = buf[a - to];
		}
		if (ezp_bulk(EZP_EP_DATA_OUT, chunk, n, NULL))
			goto fail;
		if (timer_progress()) {
			printf("\bWritten %ld%% [%lu] of [%lu] bytes      \r", 100 * (done + n - first) / (end - first),
			       done + n - first, end - first);
			fflush(stdout);
		}
	}
	if (ezp_wait(10 * 1000, 2000))
		goto fail;
	printf("Written 100%% [%lu] of [%lu] bytes      \n", len, len);
	timer_end();
	return len;

fail:
	timer_end();
	return -1;
}

long ezp_flash_init(struct flash_cmd *cmd)
{
	unsigned char id[3];
	unsigned long sector;
	int addr4b;
	const char *name;

	if (!ezp || ezp_detect())
		return -1;
	id[0] = ezp_id >> 16;
	id[1] = ezp_id >> 8;
	id[2] = ezp_id;
	printf("spi device id: %x %x %x\n", id[0], id[1], id[2]);
	name = snor_lookup(id, &ezp_size, &sector, &addr4b);
	if (!name) {
		printf("SPI NOR Flash Not Detected!\n");
		return -1;
	}
	if (addr4b) {
		printf("%s: chips over 16 MB are not supported with the EZP programmers yet.\n", name);
		return -1;
	}
	printf("Detected SPI NOR Flash: %s, Flash Size: %lu MB (status registers are not readable through EZP)\n",
	       name, ezp_size >> 20);
	if (ezp_detect())			/* again, with the chip size */
		return -1;
	ezp_chip_id = ezp_id;
	cmd->flash_read = ezp_read;
	cmd->flash_write = ezp_write;
	cmd->flash_erase = ezp_erase;
	cmd->flash_unprotect = NULL;
	bsize = sector;
	flash_chip_name = name;
	return ezp_size;
}

int ezp_init(void)
{
	unsigned char strbuf[256];
	unsigned int which = 0;
	int ret;

	if (ezp)
		return -1;
	if (libusb_init(NULL) < 0) {
		printf("Could not initialize libusb!\n");
		return -1;
	}
	for (which = 0; which < sizeof(ezp_pids) / sizeof(ezp_pids[0]) && !ezp; which++)
		ezp = libusb_open_device_with_vid_pid(NULL, EZP_VID, ezp_pids[which]);
	if (!ezp) {
		printf("Couldn't open an EZP programmer (%04x:%04x, %04x, %04x).\n", EZP_VID, ezp_pids[0], ezp_pids[1], ezp_pids[2]);
		libusb_exit(NULL);
		return -1;
	}
	printf("Found programmer device: EZP2019/EZP2019+/EZP2023 (%04x:%04x)\n", EZP_VID, ezp_pids[which - 1]);

	ret = libusb_detach_kernel_driver(ezp, 0);
	ezp_detached = ret == 0;
	/* some of them take commands only after their strings were read */
	libusb_get_string_descriptor(ezp, 1, 0x0409, strbuf, sizeof(strbuf));
	libusb_get_string_descriptor(ezp, 2, 0x0409, strbuf, sizeof(strbuf));
	ret = libusb_claim_interface(ezp, 0);
	if (ret != 0) {
		printf("Failed to claim interface 0: '%s'\n", libusb_error_name(ret));
		if (ezp_detached)
			libusb_attach_kernel_driver(ezp, 0);
		libusb_close(ezp);
		ezp = NULL;
		libusb_exit(NULL);
		return -1;
	}
	ezp_id = ezp_chip_id = 0;
	ezp_size = 0;
	return 0;
}

int ezp_shutdown(void)
{
	unsigned char pkt[EZP_PKT];

	if (!ezp)
		return -1;
	memset(pkt, 0, sizeof(pkt));
	pkt[0] = 1;
	pkt[1] = EZP_OP_RESET;
	ezp_command(pkt, NULL);
	libusb_release_interface(ezp, 0);
	if (ezp_detached)
		libusb_attach_kernel_driver(ezp, 0);
	ezp_detached = 0;
	libusb_close(ezp);
	ezp = NULL;
	libusb_exit(NULL);
	return 0;
}

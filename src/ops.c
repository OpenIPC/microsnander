/*
 * ops.c — the operations of the web mode and the Android app, on the one
 * programmer that is connected (chosen as without -W). Each opens the
 * programmer and the chip, works, and closes them again.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "ops.h"
#include "flashcmd_api.h"
#include "programmer.h"
#include "voltage.h"
#include "selftest.h"

extern unsigned int bsize;		/* the erase block of the chip found */

#define OPS_PART	(64 * 1024)		/* read, write and verify in parts: the progress */

static struct flash_cmd prog;
static long long flen;

static void say(char *msg, size_t n, const char *fmt, ...)
{
	va_list ap;

	if (!msg || !n)
		return;
	va_start(ap, fmt);
	vsnprintf(msg, n, fmt, ap);
	va_end(ap);
}

static void progress(ms_progress cb, void *ctx, const char *phase, unsigned long long done, unsigned long long total)
{
	if (cb)
		cb(ctx, phase, done, total);
}

/* "16 MB", "512 KB" or "300 bytes" */
static const char *size_text(char *buf, size_t n, long long size)
{
	if (size >= 1 << 20 && !(size & ((1 << 20) - 1)))
		snprintf(buf, n, "%lld MB", size >> 20);
	else if (size >= 1024 && !(size & 1023))
		snprintf(buf, n, "%lld KB", size >> 10);
	else
		snprintf(buf, n, "%lld bytes", size);
	return buf;
}

/* the chip the user confirmed for the next operation: Erase and Write go
   only to it; "" takes any */
static char expect_chip[64];
static long long expect_size;

void ms_expect(const char *chip, long long size)
{
	snprintf(expect_chip, sizeof(expect_chip), "%s", chip ? chip : "");
	expect_size = size;
}

/* confirmed: an Erase or a Write, which goes only to the chip of ms_expect();
   any other operation drops that expectation */
static int chip_open(struct ms_chip *c, char *msg, size_t n, int confirmed)
{
	char want[sizeof(expect_chip)];
	long long want_size = expect_size;

	memcpy(want, expect_chip, sizeof(want));	/* for this operation only */
	expect_chip[0] = 0;
	if (!confirmed)
		want[0] = 0;

	int ret = programmer_init(false);

	memset(c, 0, sizeof(*c));
	if (ret == -2) {
		say(msg, n, "Several programmers, or two of one kind, are connected: connect one.");
		return -1;
	}
	if (ret < 0) {
		say(msg, n, "No programmer found.");
		return -1;
	}
	memset(&prog, 0, sizeof(prog));
	flash_chip_name = NULL;
	flen = flash_cmd_init(&prog);
	snprintf(c->programmer, sizeof(c->programmer), "%s", programmer->name);
	if (flen <= 0) {
		say(msg, n, "No chip found.");
		programmer_shutdown();
		return -1;
	}
	snprintf(c->chip, sizeof(c->chip), "%s", flash_chip_name ? flash_chip_name : "");
	c->size = flen;
	c->voltage = ms_voltage(c->chip);
	ms_voltage_warning(c->programmer, c->chip, c->warning, sizeof(c->warning));
	if (want[0] && (strcmp(c->chip, want) || c->size != want_size)) {
		char now[24], was[24];

		say(msg, n, "The chip is %s (%s) now, not the %s (%s) confirmed: nothing done.",
		    c->chip, size_text(now, sizeof(now), c->size), want, size_text(was, sizeof(was), want_size));
		programmer_shutdown();
		return -1;
	}
	return 0;
}

static void chip_close(void)
{
	programmer_shutdown();
}

/* the status registers: SPI NOR through an SPI bridge only */
static int read_registers(struct ms_chip *c)
{
	c->sr_valid = prog.flash_unprotect && programmer->send && !snor_read_status(c->sr);
	return c->sr_valid ? 0 : -1;
}

void ms_dump_name(char *name, size_t n, const char *chip, int range, unsigned long long a, unsigned long long l)
{
	char stamp[16], up[64];
	time_t now = time(NULL);
	struct tm tm;
	size_t i;

	localtime_r(&now, &tm);
	strftime(stamp, sizeof(stamp), "%Y%m%d%H%M", &tm);
	for (i = 0; chip[i] && i < sizeof(up) - 1; i++) {
		char ch = chip[i];

		up[i] = (ch >= 'a' && ch <= 'z') ? ch - 32 : (ch == '/' || ch == ' ' ? '_' : ch);
	}
	up[i] = 0;
	if (range)
		snprintf(name, n, "flash-%s-%s-0x%llx-0x%llx.bin", stamp, up, a, l);
	else
		snprintf(name, n, "flash-%s-%s.bin", stamp, up);
}

int ms_detect(struct ms_chip *c, char *msg, size_t n)
{
	if (chip_open(c, msg, n, 0))
		return -1;
	read_registers(c);
	say(msg, n, "Found %s with %s, %lld MB.", c->programmer, c->chip, flen >> 20);
	chip_close();
	return 0;
}

int ms_read(struct ms_chip *c, FILE *out, unsigned long long a, unsigned long long len,
	    ms_progress cb, void *ctx, char *msg, size_t n)
{
	unsigned char *part;
	unsigned long long l;
	int ret = -1;

	if (chip_open(c, msg, n, 0))
		return -1;
	l = len ? len : (a < (unsigned long long)flen ? (unsigned long long)flen - a : 0);
	if (a >= (unsigned long long)flen || l > (unsigned long long)flen - a) {
		say(msg, n, "The range 0x%llx + 0x%llx is past the chip end (0x%llx).", a, l, flen);
		goto out;
	}
	if (!(part = malloc(OPS_PART))) {
		say(msg, n, "Out of memory.");
		goto out;
	}
	for (unsigned long long done = 0; done < l; ) {
		unsigned long k = l - done < OPS_PART ? l - done : OPS_PART;

		progress(cb, ctx, "read", done, l);
		if (prog.flash_read(part, a + done, k) != (int)k) {
			say(msg, n, "Read failed at 0x%llx.", a + done);
			goto free;
		}
		if (fwrite(part, 1, k, out) != k) {
			say(msg, n, "Cannot write the dump file.");
			goto free;
		}
		done += k;
	}
	progress(cb, ctx, "read", l, l);
	say(msg, n, "Read 0x%llx bytes.", l);
	ret = 0;
free:
	free(part);
out:
	chip_close();
	return ret;
}

int ms_erase(struct ms_chip *c, ms_progress cb, void *ctx, char *msg, size_t n)
{
	int ret = -1;

	if (chip_open(c, msg, n, 1))
		return -1;
	progress(cb, ctx, "erase", 0, 0);
	if (prog.flash_erase(0, flen))
		say(msg, n, "Erase failed.");
	else {
		say(msg, n, "The whole chip is erased.");
		ret = 0;
	}
	chip_close();
	return ret;
}

/* k bytes of the file from off into buf; 0, or -1. fseek, not fseeko: the
   Android API 21 has no 64-bit fseeko, and a chip is far below 2 GB */
static int file_part(FILE *f, unsigned long long off, unsigned char *buf, unsigned long k)
{
	return fseek(f, (long)off, SEEK_SET) || fread(buf, 1, k, f) != k ? -1 : 0;
}

int ms_write_file(struct ms_chip *c, FILE *f, ms_progress cb, void *ctx, char *msg, size_t n)
{
	unsigned char *part = NULL, *chunk = NULL;
	unsigned long long l;
	long end;
	int ret = -1;

	if (fseek(f, 0, SEEK_END) || (end = ftell(f)) < 0) {
		ms_expect(NULL, 0);		/* this write ends before the chip is opened */
		say(msg, n, "Cannot read the file.");
		return -1;
	}
	l = end;
	if (!l) {				/* before the programmer is touched */
		ms_expect(NULL, 0);
		memset(c, 0, sizeof(*c));
		say(msg, n, "The file is empty.");
		return -1;
	}
	if (chip_open(c, msg, n, 1))
		return -1;
	if (l > (unsigned long long)flen) {
		say(msg, n, "The file (0x%llx bytes) is larger than the chip (0x%llx).", l, flen);
		goto out;
	}
	if (!(part = malloc(OPS_PART)) || !(chunk = malloc(OPS_PART))) {
		say(msg, n, "Out of memory.");
		goto out;
	}
	/* the file is read again for the verify: only two parts are in memory */
	for (unsigned long long done = 0; done < l; ) {
		unsigned long k = l - done < OPS_PART ? l - done : OPS_PART;

		progress(cb, ctx, "write", done, l);
		if (file_part(f, done, chunk, k)) {
			say(msg, n, "Cannot read the file at 0x%llx.", done);
			goto out;
		}
		if (prog.flash_write(chunk, done, k) != (int)k) {
			say(msg, n, "Write failed at 0x%llx.", done);
			goto out;
		}
		done += k;
	}
	for (unsigned long long done = 0; done < l; ) {
		unsigned long k = l - done < OPS_PART ? l - done : OPS_PART;

		progress(cb, ctx, "verify", done, l);
		if (prog.flash_read(part, done, k) != (int)k) {
			say(msg, n, "Read back failed at 0x%llx.", done);
			goto out;
		}
		if (file_part(f, done, chunk, k)) {
			say(msg, n, "Cannot read the file at 0x%llx.", done);
			goto out;
		}
		for (unsigned long i = 0; i < k; i++)
			if (part[i] != chunk[i]) {
				say(msg, n, "Verify failed: 0x%02x at 0x%llx, the file has 0x%02x.", part[i], done + i, chunk[i]);
				goto out;
			}
		done += k;
	}
	progress(cb, ctx, "verify", l, l);
	say(msg, n, "Wrote and verified 0x%llx bytes from address 0.", l);
	ret = 0;
out:
	free(part);
	free(chunk);
	chip_close();
	return ret;
}

int ms_selftest(struct ms_chip *c, const char *save_dir, char *msg, size_t n)
{
	struct selftest t = { 0 };
	int ret;

	if (chip_open(c, msg, n, 1))		/* it erases a block: only the chip confirmed */
		return -1;
	read_registers(c);
	t.cmd = &prog;
	t.chip = c->chip;
	t.programmer = c->programmer;
	t.size = flen;
	t.addr = 0;				/* the first block: where a locked loader usually is */
	t.block = bsize;
	t.kind = prog.kind;
	t.lock = prog.lock;
	if (prog.kind == FLASH_NOR && programmer->send) {
		t.sr_valid = c->sr_valid;
		memcpy(t.sr, c->sr, 3);
		t.block_locks = snor_block_locks();
	}
	t.save_dir = save_dir;
	t.out = msg;
	t.out_size = n;
	msg[0] = 0;
	ret = selftest(&t);
	/* the lines one after the other: keep the last (the verdict) and the protection */
	for (char *p = msg; *p; p++)
		if (*p == '\n' && p[1])
			*p = ' ';
	if (n && msg[0] && msg[strlen(msg) - 1] == '\n')
		msg[strlen(msg) - 1] = 0;
	chip_close();
	return ret;
}

int ms_registers(struct ms_chip *c, char *msg, size_t n)
{
	int ret;

	if (chip_open(c, msg, n, 0))
		return -1;
	ret = read_registers(c);
	if (ret)
		say(msg, n, "The status registers are readable for SPI NOR through the CH341A or the CH347 only.");
	else
		say(msg, n, "Status registers read.");
	chip_close();
	return ret;
}

int ms_unprotect(struct ms_chip *c, char *msg, size_t n)
{
	int ret = -1;

	if (chip_open(c, msg, n, 0))
		return -1;
	if (!prog.flash_unprotect)
		say(msg, n, "Clearing the protection is for SPI NOR through the CH341A or the CH347 only.");
	else if ((ret = prog.flash_unprotect())) {
		read_registers(c);
		if (ret == -2 && prog.flash_unprotect == snor_unprotect && snor_unprotect_why[0])
			say(msg, n, "The protection stays: %s.", snor_unprotect_why);
		else
			say(msg, n, "Clearing the protection failed: the programmer or the chip did not answer.");
		ret = -1;
	}
	else {
		read_registers(c);
		say(msg, n, "The protection is cleared.");
		ret = 0;
	}
	chip_close();
	return ret;
}

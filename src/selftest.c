/*
 * selftest.c — -t: is the chip protected, does a block erase, take a write
 * and read back? One block (the first by default, where a locked loader
 * usually is; -a for another), its data kept in memory and written back.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include "flashcmd_api.h"
#include "selftest.h"
#include "timer.h"
#include <stdarg.h>
#include <signal.h>

/* a line of the result: to the console, and kept in t->out for the web
   mode and the app (the lines one after the other) */
static void report(const struct selftest *t, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	if (t->out && t->out_size) {
		size_t k = strlen(t->out);

		if (k < t->out_size) {
			va_start(ap, fmt);
			vsnprintf(t->out + k, t->out_size - k, fmt, ap);
			va_end(ap);
		}
	}
}

static double now(void)
{
	struct timeval tv;

	gettimeofday(&tv, NULL);
	return tv.tv_sec + tv.tv_usec / 1e6;
}

/* a pattern that tells stuck bits and swapped addresses apart */
static unsigned char pattern(unsigned long long a)
{
	return (unsigned char)(a ^ (a >> 8) ^ (a >> 16) ^ 0xa5);
}

/* the first offset where got is not want, or -1 */
static long first_diff(const unsigned char *got, const unsigned char *want, unsigned long n)
{
	for (unsigned long i = 0; i < n; i++)
		if (got[i] != want[i])
			return (long)i;
	return -1;
}

/* the first offset that is not erased (0xff), or -1 */
static long first_not_ff(const unsigned char *got, unsigned long n)
{
	for (unsigned long i = 0; i < n; i++)
		if (got[i] != 0xff)
			return (long)i;
	return -1;
}

/* "2 ms" or "2.43 s" */
static const char *took(double s, char *buf, size_t n)
{
	if (s < 1)
		snprintf(buf, n, "%.0f ms", s * 1000);
	else
		snprintf(buf, n, "%.2f s", s);
	return buf;
}

/* the protection the registers show; 1 if some area is protected */
static int protection(const struct selftest *t, char *why, size_t n)
{
	why[0] = 0;
	if (t->kind == FLASH_NAND) {
		if (t->lock < 0)
			return 0;
		unsigned bp = (t->lock >> 3) & 7;
		if (bp)
			snprintf(why, n, "its blocks were locked at power-up (A0 = 0x%02x, BP = %u); "
				 "microsnander unlocks them for the run", t->lock, bp);
		else
			snprintf(why, n, "none, the blocks are unlocked (A0 = 0x%02x)", t->lock);
		return 0;		/* unlocked by the driver: not in the way of this run */
	}
	if (t->block_locks) {
		snprintf(why, n, "an SST26-type chip: its blocks are locked at power-up, and microsnander "
			 "unlocks them for the run (the lock comes back at the next power-up)");
		return 0;		/* volatile, unlocked by the driver: not in the way of this run */
	}
	if (!t->sr_valid)
		return 0;
	if (t->sr_macronix) {
		unsigned bp3 = (t->sr[0] >> 2) & 15;

		if (bp3)
			snprintf(why, n, "SR1 = 0x%02x: BP = %u%s", t->sr[0], bp3,
				 t->sr[0] & 0x80 ? "; the register itself is locked (SRWD)" : "");
		return bp3 != 0;
	}
	unsigned sr1 = t->sr[0], sr2 = t->sr[1] == 0xff ? 0 : t->sr[1];
	unsigned bp = (sr1 >> 2) & 7, tb = (sr1 >> 5) & 1, cmp = (sr2 >> 6) & 1;
	int on = cmp ? bp != 7 : bp != 0;	/* CMP turns the area round: BP 0 is all of it */

	if (on)
		snprintf(why, n, "SR1 = 0x%02x SR2 = 0x%02x: BP = %u, TB = %u (%s), CMP = %u%s", t->sr[0], t->sr[1],
			 bp, tb, tb ? "from the start" : "from the end", cmp,
			 (sr1 & 0x80) || (sr2 & 1) ? "; the registers themselves are locked (SRP)" : "");
	return on;
}

#define STOPPED "Self-test: stopped by Ctrl+C; the block gets its data back.\n"

int selftest(const struct selftest *t)
{
	unsigned long n = t->block;
	unsigned long long a = t->addr - t->addr % n;
	unsigned char *orig = malloc(n), *buf = malloc(n), *pat = malloc(n);
	char why[200];
	int prot = protection(t, why, sizeof(why)), ret = 1, changed = 0;
	double t0, t_erase = 0, t_write = 0;

	printf("SELF-TEST: %s, block 0x%llx + 0x%lx\n", t->chip, a, n);
	if (why[0])
		report(t, "Protection: %s.\n", why);
	else if (t->kind != FLASH_FIRMWARE)
		report(t, "Protection: none set in the registers.\n");
	if (!orig || !buf || !pat) {
		report(t, "Self-test: FAILED, out of memory.\n");
		goto out;
	}
	if (a + n > t->size) {
		report(t, "Self-test: FAILED, the block is past the chip end (0x%llx).\n", t->size);
		goto out;
	}
	if (t->kind == FLASH_NAND && a) {
		/* block 0 is the one every maker guarantees good; another one may
		   be a bad block, whose mark the erase would take away */
		report(t, "Self-test: SPI NAND is tested on block 0 only (-a 0).\n");
		goto out;
	}
	if (prot) {
		/* an erase would clear the protection bits, and they are the user's:
		   no test of the block until -u is given for it */
		report(t, "Self-test: PROTECTED, the registers protect part of the chip (exit 2). "
		       "Clear them with -u, then run -t again.\n");
		ret = 2;
		goto out;
	}
	if (t->cmd->flash_read(orig, a, n) != (int)n || t->cmd->flash_read(buf, a, n) != (int)n) {
		report(t, ms_stop ? "Self-test: stopped by Ctrl+C; nothing was changed.\n" :
			  "Self-test: FAILED, the block does not read.\n");
		goto out;
	}
	if (memcmp(orig, buf, n)) {
		report(t, "Self-test: FAILED, two reads of the block differ at 0x%llx (an unstable read: "
		       "the wires, the voltage, the clock).\n", a + first_diff(buf, orig, n));
		goto out;
	}
	if (t->kind == FLASH_FIRMWARE) {
		/* the EZP erases the whole chip only: no erase and write of one block */
		report(t, "Self-test: READ OK (two reads equal); the %s erases only the whole chip, so the "
		       "erase and the write of a block are not tried.\n", t->programmer);
		ret = 0;
		goto out;
	}

	if (ms_stop) {				/* stopped after the reads: nothing changed yet */
		report(t, "Self-test: stopped by Ctrl+C; nothing was changed.\n");
		goto out;
	}
	t0 = now();
	changed = 1;
	int e = t->cmd->flash_erase(a, n);
	t_erase = now() - t0;
	int got = t->cmd->flash_read(buf, a, n) == (int)n;
	long d = got ? first_not_ff(buf, n) : 0;

	if (ms_stop) {
		report(t, STOPPED);
		goto restore;
	}
	if (e || !got || d >= 0) {
		int same = got && !memcmp(buf, orig, n);	/* nothing happened to it */

		changed = !same;
		/* a block that takes the erase command and keeps its data is locked
		   in a way the registers do not show (the WP# pin, a block lock); an
		   erase that fails as a command is a fault of its own */
		if (same && !e) {
			report(t, "Self-test: PROTECTED, the block takes the erase and keeps its data, though no "
			       "register bit says so (the WP# pin, a block lock?).\n");
			ret = 2;
		} else if (e)
			report(t, "Self-test: FAILED, the erase command fails.\n");
		else if (!got)
			report(t, "Self-test: FAILED, the erased block does not read.\n");
		else
			report(t, "Self-test: FAILED, the erase does not take: 0x%02x at 0x%llx.\n", buf[d], a + d);
		goto restore;
	}
	for (unsigned long i = 0; i < n; i++)
		pat[i] = pattern(a + i);
	t0 = now();
	if (t->cmd->flash_write(pat, a, n) != (int)n) {
		report(t, ms_stop ? STOPPED : "Self-test: FAILED, the write command fails.\n");
		goto restore;
	}
	t_write = now() - t0;
	if (t->cmd->flash_read(buf, a, n) != (int)n) {
		report(t, ms_stop ? STOPPED : "Self-test: FAILED, the written block does not read back.\n");
		goto restore;
	}
	d = first_diff(buf, pat, n);
	if (d >= 0) {
		report(t, "Self-test: FAILED, the write does not take: 0x%02x at 0x%llx, written 0x%02x.\n", buf[d], a + d, pat[d]);
		goto restore;
	}
	ret = 0;

restore:
	if (!changed)
		goto out;
	{
		int blank = first_not_ff(orig, n) < 0;
		/* after a Ctrl+C the block still gets its data back: Ctrl+C waits
		   until it is back (the signals are held for that long) */
		sig_atomic_t stop = ms_stop;
		sigset_t hold, old;
		int bad;

		sigemptyset(&hold);
		sigaddset(&hold, SIGINT);
		sigaddset(&hold, SIGTERM);
		sigprocmask(SIG_BLOCK, &hold, &old);
		ms_stop = 0;
		bad = t->cmd->flash_erase(a, n) ||
		      (!blank && t->cmd->flash_write(orig, a, n) != (int)n) ||
		      t->cmd->flash_read(buf, a, n) != (int)n || memcmp(buf, orig, n);
		sigprocmask(SIG_SETMASK, &old, NULL);	/* a Ctrl+C held now comes */
		ms_stop = stop || ms_stop;		/* and counts, as the one before */
		if (bad) {
			char base[120], *name;
			FILE *f = NULL;

			snprintf(base, sizeof(base), "selftest-%s-0x%llx.bin", t->chip, a);
			for (char *p = base; *p; p++)
				if (*p == ' ' || *p == '/')
					*p = '_';
			/* the whole directory, never a shortened one */
			name = malloc((t->save_dir ? strlen(t->save_dir) + 1 : 0) + strlen(base) + 1);
			if (name) {
				sprintf(name, "%s%s%s", t->save_dir ? t->save_dir : "", t->save_dir ? "/" : "", base);
				f = fopen(name, "wb");
			}
			int kept = f && fwrite(orig, 1, n, f) == n;

			if (f && fclose(f))
				kept = 0;
			if (kept) {
				report(t, "Self-test: the block did not come back as it was: its old data is in %s "
				       "(write it with -a 0x%llx -w %s).\n", name, a, name);
				if (t->saved)
					snprintf(t->saved, t->saved_size, "%s", base);
			} else {
				report(t, "Self-test: the block did not come back as it was, and %s cannot be written.\n",
				       name ? name : base);
			}
			free(name);
			ret = 1;
			goto out;
		}
	}
	if (ret == 0 && ms_stop) {		/* stopped after the test passed: not a finished run */
		report(t, STOPPED);
		ret = 1;
	}
	if (ret == 0) {
		char te[16], tw[16];

		report(t, "Self-test: OK, the block erases in %s, takes a write in %s, reads back; "
		       "its data is back%s.\n", took(t_erase, te, sizeof(te)), took(t_write, tw, sizeof(tw)),
		       t->kind == FLASH_NAND ? " (its spare area besides the ECC is not kept)" : "");
	}
out:
	free(orig);
	free(buf);
	free(pat);
	return ret;
}

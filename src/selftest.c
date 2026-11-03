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

/* the protection the registers show; 1 if some area is protected */
/* "2 ms" or "2.43 s" */
static const char *took(double s, char *buf, size_t n)
{
	if (s < 1)
		snprintf(buf, n, "%.0f ms", s * 1000);
	else
		snprintf(buf, n, "%.2f s", s);
	return buf;
}

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
	if (!t->sr_valid)
		return 0;
	unsigned sr1 = t->sr[0], sr2 = t->sr[1] == 0xff ? 0 : t->sr[1];
	unsigned bp = (sr1 >> 2) & 7, tb = (sr1 >> 5) & 1, cmp = (sr2 >> 6) & 1;
	int on = cmp ? bp != 7 : bp != 0;	/* CMP turns the area round: BP 0 is all of it */

	if (on)
		snprintf(why, n, "SR1 = 0x%02x SR2 = 0x%02x: BP = %u, TB = %u (%s), CMP = %u%s", t->sr[0], t->sr[1],
			 bp, tb, tb ? "from the start" : "from the end", cmp,
			 (sr1 & 0x80) || (sr2 & 1) ? "; the registers themselves are locked (SRP)" : "");
	return on;
}

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
		printf("Protection: %s.\n", why);
	else if (t->kind != FLASH_FIRMWARE)
		printf("Protection: none set in the registers.\n");
	if (!orig || !buf || !pat) {
		printf("Self-test: FAILED, out of memory.\n");
		goto out;
	}
	if (a + n > t->size) {
		printf("Self-test: FAILED, the block is past the chip end (0x%llx).\n", t->size);
		goto out;
	}
	if (t->cmd->flash_read(orig, a, n) != (int)n || t->cmd->flash_read(buf, a, n) != (int)n) {
		printf("Self-test: FAILED, the block does not read.\n");
		goto out;
	}
	if (memcmp(orig, buf, n)) {
		printf("Self-test: FAILED, two reads of the block differ at 0x%llx (an unstable read: "
		       "the wires, the voltage, the clock).\n", a + first_diff(buf, orig, n));
		goto out;
	}
	if (t->kind == FLASH_FIRMWARE) {
		/* the EZP erases the whole chip only: no erase and write of one block */
		printf("Self-test: READ OK (two reads equal); the %s erases only the whole chip, so the "
		       "erase and the write of a block are not tried.\n", t->programmer);
		ret = 0;
		goto out;
	}

	t0 = now();
	changed = 1;
	int e = t->cmd->flash_erase(a, n);
	t_erase = now() - t0;
	int got = t->cmd->flash_read(buf, a, n) == (int)n;
	long d = got ? first_not_ff(buf, n) : 0;

	if (e || !got || d >= 0) {
		int same = got && !memcmp(buf, orig, n);	/* nothing happened to it */

		changed = !same;
		/* a protected block takes the erase command and keeps its data; an
		   erase that fails as a command is a fault of its own */
		if (prot || (same && !e)) {
			printf("Self-test: PROTECTED, the block does not erase%s.\n",
			       prot ? ": the registers protect it, -u clears them" : " (its data stays as it was)");
			ret = 2;
		} else {
			printf("Self-test: FAILED, the erase does not take (0x%02x at 0x%llx).\n", got ? buf[d] : 0, a + (d > 0 ? d : 0));
		}
		goto restore;
	}
	for (unsigned long i = 0; i < n; i++)
		pat[i] = pattern(a + i);
	t0 = now();
	if (t->cmd->flash_write(pat, a, n) != (int)n) {
		printf("Self-test: %s, the write does not go%s.\n", prot ? "PROTECTED" : "FAILED",
		       prot ? ": the registers protect the block, -u clears them" : "");
		ret = prot ? 2 : 1;
		goto restore;
	}
	t_write = now() - t0;
	if (t->cmd->flash_read(buf, a, n) != (int)n) {
		printf("Self-test: FAILED, the written block does not read back.\n");
		goto restore;
	}
	d = first_diff(buf, pat, n);
	if (d >= 0) {
		printf("Self-test: FAILED, the write does not take: 0x%02x at 0x%llx, written 0x%02x.\n", buf[d], a + d, pat[d]);
		goto restore;
	}
	ret = 0;

restore:
	if (!changed)
		goto out;
	{
		int blank = first_not_ff(orig, n) < 0;

		if (t->cmd->flash_erase(a, n) ||
		    (!blank && t->cmd->flash_write(orig, a, n) != (int)n) ||
		    t->cmd->flash_read(buf, a, n) != (int)n || memcmp(buf, orig, n)) {
			char name[160];
			FILE *f;

			snprintf(name, sizeof(name), "selftest-%s-0x%llx.bin", t->chip, a);
			for (char *p = name; *p; p++)
				if (*p == ' ' || *p == '/')
					*p = '_';
			if ((f = fopen(name, "wb")) && fwrite(orig, 1, n, f) == n && !fclose(f))
				printf("Self-test: the block did not come back as it was: its old data is in %s "
				       "(write it with -a 0x%llx -w %s).\n", name, a, name);
			else
				printf("Self-test: the block did not come back as it was, and %s cannot be written.\n", name);
			ret = 1;
			goto out;
		}
	}
	if (ret == 0) {
		char te[16], tw[16];

		printf("Self-test: OK, the block erases in %s, takes a write in %s, reads back; "
		       "its data is back.\n", took(t_erase, te, sizeof(te)), took(t_write, tw, sizeof(tw)));
	}
out:
	free(orig);
	free(buf);
	free(pat);
	return ret;
}

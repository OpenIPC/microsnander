/*
 * selftest.h — -t, the short self-test of the chip.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef __MS_SELFTEST_H__
#define __MS_SELFTEST_H__

struct flash_cmd;

struct selftest {
	struct flash_cmd *cmd;		/* the chip, open */
	const char *chip, *programmer;
	unsigned long long size, addr;	/* the block at addr (rounded down) */
	unsigned long block;		/* the erase block */
	int kind;			/* FLASH_NOR, FLASH_NAND, FLASH_FIRMWARE */
	int sr_valid;			/* SPI NOR: sr read */
	unsigned char sr[3];
	int lock;			/* SPI NAND: A0 when found, -1 */
	const char *save_dir;		/* where the block goes if it does not come back; NULL: here */
	char *out;			/* NULL, or the result lines are kept here too */
	size_t out_size;
	char *saved;			/* NULL, or the name (no directory) of the block saved there */
	size_t saved_size;
	int block_locks;		/* SPI NOR of the SST26 type: blocks locked at power-up, not in SR */
};

/* the verdict printed; 0 OK, 2 protected, 1 failed */
int selftest(const struct selftest *t);

#endif /* __MS_SELFTEST_H__ */

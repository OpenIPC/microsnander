/*
 * ops.h — the operations of the web mode and the Android app: Detect,
 * Read, Erase, Write (and verify), Registers and Clear protection, on the
 * one programmer that is connected.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef __MS_OPS_H__
#define __MS_OPS_H__

#include <stdio.h>

struct ms_chip {
	char programmer[32];		/* "CH347" */
	char chip[64];			/* "W25Q128BV" */
	long long size;
	int sr_valid;			/* status registers read (SPI NOR, SPI bridge) */
	unsigned char sr[3];
	int voltage;			/* MS_VOLTAGE_*, from the chip name */
	char warning[320];		/* a 1.8 V chip, the 5 V lines of the CH341A; "" */
};

/* progress of an operation: phase "read", "erase", "write" or "verify";
   total 0: not known */
typedef void (*ms_progress)(void *ctx, const char *phase, unsigned long long done, unsigned long long total);

/* Every operation opens the programmer and the chip, does its work and
   closes them; chip gets what was found. 0 or -1, and msg says what. */
int ms_detect(struct ms_chip *chip, char *msg, size_t n);
int ms_read(struct ms_chip *chip, FILE *out, unsigned long long addr, unsigned long long len,
	    ms_progress cb, void *ctx, char *msg, size_t n);	/* len 0: from addr to the end */
int ms_erase(struct ms_chip *chip, ms_progress cb, void *ctx, char *msg, size_t n);
int ms_write(struct ms_chip *chip, const unsigned char *data, unsigned long len,
	     ms_progress cb, void *ctx, char *msg, size_t n);	/* from address 0, then verified */
int ms_registers(struct ms_chip *chip, char *msg, size_t n);
int ms_unprotect(struct ms_chip *chip, char *msg, size_t n);

/* flash-YYYYMMDDHHMM-CHIP[-0xADDR-0xLEN].bin, local time */
void ms_dump_name(char *name, size_t n, const char *chip, int range, unsigned long long addr, unsigned long long len);

#endif /* __MS_OPS_H__ */

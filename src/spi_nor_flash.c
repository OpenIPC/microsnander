/*
 * Copyright (C) 2018-2021 McMCC <mcmcc@mail.ru>
 * spi_nor_flash.c
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdbool.h>
#include <time.h>
#include "spi_controller.h"
#include "snorcmd_api.h"
#include "types.h"
#include "timer.h"

#define min(a,b) (((a)<(b))?(a):(b))

/******************************************************************************
 * SPI FLASH elementray definition and function
 ******************************************************************************/

#define FLASH_PAGESIZE			256

/* Flash opcodes. */
#define OPCODE_WREN			6	/* Write enable */
#define OPCODE_WRDI			4	/* Write disable */
#define OPCODE_RDSR			5	/* Read status register */
#define OPCODE_WRSR			1	/* Write status register */
#define OPCODE_RDSR2		0x35/* Read status register 2*/
#define OPCODE_WRSR2		0x31/* Write status register 2*/
#define OPCODE_RDSR3		0x15/* Read status register 3 */
#define OPCODE_WRSR3		0x11/* Write status register 3*/
#define OPCODE_READ			3	/* Read data bytes */
#define OPCODE_PP			2	/* Page program */
#define OPCODE_SE			0xD8	/* Sector erase */
#define OPCODE_RES			0xAB	/* Read Electronic Signature */
#define OPCODE_RDID			0x9F	/* Read JEDEC ID */

#define OPCODE_FAST_READ		0x0B	/* Fast Read */
#define OPCODE_DOR			0x3B	/* Dual Output Read */
#define OPCODE_QOR			0x6B	/* Quad Output Read */
#define OPCODE_DIOR			0xBB	/* Dual IO High Performance Read */
#define OPCODE_QIOR			0xEB	/* Quad IO High Performance Read */
#define OPCODE_READ_ID			0x90	/* Read Manufacturer and Device ID */

#define OPCODE_P4E			0x20	/* 4KB Parameter Sectore Erase */
#define OPCODE_P8E			0x40	/* 8KB Parameter Sectore Erase */
#define OPCODE_BE			0x60	/* Bulk Erase */
#define OPCODE_BE1			0xC7	/* Bulk Erase */
#define OPCODE_QPP			0x32	/* Quad Page Programing */

#define OPCODE_CLSR			0x30
#define OPCODE_RCR			0x35	/* Read Configuration Register */

#define OPCODE_AAI			0xAD	/* SST25: auto address increment word program */
#define OPCODE_ULBPR			0x98	/* SST26: global block protection unlock */

#define OPCODE_BRRD			0x16
#define OPCODE_BRWR			0x17

/* Status Register bits. */
#define SR_WIP				1	/* Write in progress */
#define SR_WEL				2	/* Write enable latch */
#define SR_BP0				4	/* Block protect 0 */
#define SR_BP1				8	/* Block protect 1 */
#define SR_BP2				0x10	/* Block protect 2 */
#define SR_EPE				0x20	/* Erase/Program error */
#define SR_SRWD				0x80	/* SR write protect */

/* what unprotect clears and checks in SR1: BP0-2 and SRP0. Bit 5 is TB or
   BP3 on some chips but an erase/program error flag on others, and TB
   alone protects nothing: it is written 0 with the rest, never checked */
#define SR1_PROTECT			(SR_BP0 | SR_BP1 | SR_BP2 | SR_SRWD)
#define SR2_SRP1			1
#define SR2_QE				2
#define SR2_R				4
#define SR2_LB1				8
#define SR2_LB2				0x10 
#define SR2_LB3				0x20
#define SR2_CMP				0x40
#define SR2_SUS				0x80

#define SR3_WPS				4
#define SR3_DRV0			0x20
#define SR3_DRV1			0x40
#define SR3_HOLD			0x80

#define snor_dbg(args...)
/* #define snor_dbg(args...) do { if (1) printf(args); } while(0) */

#define udelay(x)			usleep(x)

struct chip_info {
	char		*name;
	u8		id;
	u32		jedec_id;
	unsigned long	sector_size;
	unsigned int	n_sectors;
	char		addr4b;
};

struct chip_info *spi_chip_info;

static int snor_wait_ready(int sleep_ms);
static int snor_wait_busy(void);
static int snor_read_sr(u8 opcode, u8 *val);
static int snor_write_sr(u8 opcode, u8 *val);

extern unsigned int bsize;

/* SST25VF-type chips (maker 0xbf, device 0x25xx) without a 256-byte page
   program: 0x02 programs one byte, longer data goes with AAI word program.
   SST25VF064C has a page program and is not one of them. */
static int snor_is_sst25_aai(void)
{
	u32 dev = spi_chip_info->jedec_id >> 16;

	return spi_chip_info->id == 0xbf && (dev >> 8) == 0x25 && dev != 0x254b;
}

/* SST26-type chips (maker 0xbf, device 0x26xx): every block is locked at
   power-up until ULBPR, and the blocks at the ends are 8 KB and 32 KB, so
   they are erased in 4 KB sectors. */
static int snor_is_sst26(void)
{
	return spi_chip_info->id == 0xbf && (spi_chip_info->jedec_id >> 24) == 0x26;
}

int snor_block_locks(void) { return snor_is_sst26(); }	/* see snorcmd_api.h */

static int snor_is_macronix(void)
{
	return spi_chip_info->id == 0xc2;
}

int snor_sr1_macronix(void) { return snor_is_macronix(); }	/* see snorcmd_api.h */

/*
 * Set write enable latch with Write Enable command.
 * Returns negative if error occurred.
 */
static inline void snor_write_enable(void)
{
	SPI_CONTROLLER_Chip_Select_Low();
	SPI_CONTROLLER_Write_One_Byte(OPCODE_WREN);
	SPI_CONTROLLER_Chip_Select_High();
}

static inline void snor_write_disable(void)
{
	SPI_CONTROLLER_Chip_Select_Low();
	SPI_CONTROLLER_Write_One_Byte(OPCODE_WRDI);
	SPI_CONTROLLER_Chip_Select_High();
}

/* One status register write of the unprotect. A chip that does not take it
   keeps the write enable latch set: clear it, the callers send their own
   write enable right before the next erase or program. */
static int snor_unprotect_write(u8 opcode, u8 *val)
{
	int ret;

	if (snor_wait_ready(1))
		return -1;
	snor_write_enable();
	snor_write_sr(opcode, val);
	ret = snor_wait_busy();	/* the status register write takes time */
	snor_write_disable();
	return ret;
}

/* The write of registers 1 and 2 with one command: the only way for the chips
   that do not know WRSR2, as the older Winbond parts. */
static int snor_write_sr12(u8 sr1, u8 sr2)
{
	u8 d[2] = { sr1, sr2 };
	int ret;

	if (snor_wait_ready(1))
		return -1;
	snor_write_enable();
	SPI_CONTROLLER_Chip_Select_Low();
	SPI_CONTROLLER_Write_One_Byte(OPCODE_WRSR);
	ret = SPI_CONTROLLER_Write_NByte(d, 2, SPI_CONTROLLER_SPEED_SINGLE);
	if (SPI_CONTROLLER_Chip_Select_High())
		ret = -1;
	if (snor_wait_busy())
		ret = -1;
	snor_write_disable();
	return ret ? -1 : 0;
}

/*
 * Set all sectors (global) unprotected if they are protected.
 * Returns negative if error occurred.
 */
char snor_unprotect_why[200];

/* all: the -u of the user, every protection bit with SRP0 and SRP1; else
   (before an erase or a program) only what protects the data: BP, CMP,
   WPS; a register lock (SRP) does not stop those */
static int unprotect(int all)
{
	u8 sr = 0;
	/* Macronix: BP3 in bit 5 */
	const u8 sr1_mask = (all ? SR1_PROTECT : (SR_BP0 | SR_BP1 | SR_BP2)) | (snor_is_macronix() ? 0x20 : 0);
	const u8 sr2_mask = all ? (SR2_SRP1 | SR2_CMP) : SR2_CMP;

	snor_unprotect_why[0] = 0;
	if (snor_is_sst26()) {
		/* the block protection register, not the status register */
		int err = 0;

		if (snor_wait_ready(1))
			return -1;
		snor_write_enable();
		err |= SPI_CONTROLLER_Chip_Select_Low();
		err |= SPI_CONTROLLER_Write_One_Byte(OPCODE_ULBPR);
		err |= SPI_CONTROLLER_Chip_Select_High();
		if (err || snor_wait_busy()) {	/* the unlock done before the next WREN */
			printf("%s: ULBPR failed\n", __func__);
			return -1;
		}
		return 0;
	}

	if (snor_read_sr(OPCODE_RDSR, &sr) < 0) {
		printf("%s: read_sr fail: %x\n", __func__, sr);
		return -1;
	}

	/* BP0-2 and SRP0 (TB with them, it means nothing alone). Bit 6 is kept:
	   SEC on Winbond-type chips (nothing without BP), QE on Macronix-type ones */
	if (snor_is_sst25_aai() ? (sr & 0xbc) : (sr & sr1_mask)) {
		u8 was = sr;

		sr = snor_is_sst25_aai() ? 0 : sr & 0x40;
		if (snor_unprotect_write(OPCODE_WRSR, &sr) || snor_read_sr(OPCODE_RDSR, &sr) < 0)
			return -1;
		if (sr & (snor_is_sst25_aai() ? 0xbc : sr1_mask)) {
			snprintf(snor_unprotect_why, sizeof(snor_unprotect_why),
				 "status register 1 stays 0x%02x (was 0x%02x): %s", sr, was,
				 sr & SR_SRWD ? "SRP0 is set, so the WP# pin of the chip is probably low and locks the register "
						"(in a socket it should be pulled up; on a board, the WP line)" :
						"the chip does not take the write");
			printf("Unprotect: %s.\n", snor_unprotect_why);
			return -2;
		}
	}

	/* SST25-type chips have one status register (BP3 and BPL in it). On Macronix
	   chips 0x35 and 0x15 read something else, or nothing: bits there are not CMP,
	   SRP1 or WPS (MX25L12805D reads ff and 07), and they take no such write. */
	if (snor_is_sst25_aai() || snor_is_macronix())
		return 0;

	/* Status registers 2 and 3: a chip without them leaves MISO high and
	   reads 0xff, and ignores the write, which would leave the write enable
	   latch set. A real register 2 or 3 has reserved bits that read 0. */
	if (snor_read_sr(OPCODE_RDSR2, &sr) < 0) {
		printf("%s: read_sr2 fail: %x\n", __func__, sr);
		return -1;
	}

	/* only the protection bits: QE (quad enable) protects nothing, and a
	   chip set up for quad reads, or one with QE fixed at 1, needs it kept */
	if (sr != 0xff && (sr & sr2_mask)) {
		u8 was = sr;

		sr &= ~sr2_mask;
		if (snor_unprotect_write(OPCODE_WRSR2, &sr) || snor_read_sr(OPCODE_RDSR2, &sr) < 0)
			return -1;
		if (sr & sr2_mask) {		/* no WRSR2 on this chip, perhaps */
			u8 sr1 = 0;

			if (snor_read_sr(OPCODE_RDSR, &sr1) < 0 || snor_write_sr12(sr1, sr & ~sr2_mask) ||
			    snor_read_sr(OPCODE_RDSR2, &sr) < 0)
				return -1;
		}
		if (sr & sr2_mask) {
			snprintf(snor_unprotect_why, sizeof(snor_unprotect_why),
				 "status register 2 stays 0x%02x (was 0x%02x): %s", sr, was,
				 sr & SR2_SRP1 ? "SRP1 locks the registers until the chip is powered off and on" :
						 "the chip does not take the write");
			printf("Unprotect: %s.\n", snor_unprotect_why);
			return -2;
		}
	}

	if (snor_read_sr(OPCODE_RDSR3, &sr) < 0) {
		printf("%s: read_sr3 fail: %x\n", __func__, sr);
		return -1;
	}

	if (sr != 0xff && (sr & SR3_WPS)) {
		sr ^= SR3_WPS;
		if (snor_unprotect_write(OPCODE_WRSR3, &sr) || snor_read_sr(OPCODE_RDSR3, &sr) < 0)
			return -1;
		if (sr & SR3_WPS) {
			snprintf(snor_unprotect_why, sizeof(snor_unprotect_why),
				 "status register 3 stays 0x%02x: the chip does not take the write", sr);
			printf("Unprotect: %s.\n", snor_unprotect_why);
			return -2;
		}
	}

	return 0;
}

int snor_unprotect(void) { return unprotect(1); }

/* the write of a status register that clears QE, and the read back; 1: it stays */
static int snor_clear_qe_bit(u8 wr, u8 rd, u8 *sr, u8 qe)
{
	*sr &= ~qe;
	if (snor_unprotect_write(wr, sr) || snor_read_sr(rd, sr) < 0)
		return -1;
	return (*sr & qe) != 0;
}

/*
 * -uu: the protection as -u, then QE. It protects nothing, but with it set the
 * WP# and HOLD# pins are data lines. QE is bit 6 of status register 1 on
 * Macronix chips and bit 1 of status register 2 on the others; a chip without
 * that register or bit has nothing to clear. Returns as snor_unprotect().
 */
int snor_clear_qe(void)
{
	const int macronix = snor_is_macronix();
	const u8 rd = macronix ? OPCODE_RDSR : OPCODE_RDSR2;
	const u8 wr = macronix ? OPCODE_WRSR : OPCODE_WRSR2;
	const u8 qe = macronix ? 0x40 : SR2_QE;
	u8 sr = 0;
	int ret = unprotect(1);

	if (ret)
		return ret;
	if (snor_is_sst25_aai() || snor_is_sst26()) {
		printf("QE: this chip has no QE bit in its status registers.\n");
		return 0;
	}
	if (snor_read_sr(rd, &sr) < 0) {
		printf("%s: read_sr fail: %x\n", __func__, sr);
		return -1;
	}
	if (!macronix && sr == 0xff) {		/* no status register 2, as in unprotect() */
		printf("QE: this chip has no status register 2.\n");
		return 0;
	}
	if (!(sr & qe)) {
		printf("QE: already clear.\n");
		return 0;
	}
	ret = snor_clear_qe_bit(wr, rd, &sr, qe);
	if (ret < 0)
		return -1;
	if (ret && !macronix) {
		u8 sr1 = 0;

		if (snor_read_sr(OPCODE_RDSR, &sr1) < 0 || snor_write_sr12(sr1, sr & ~qe) ||
		    snor_read_sr(rd, &sr) < 0)
			return -1;
		ret = (sr & qe) != 0;
	}
	if (ret) {
		snprintf(snor_unprotect_why, sizeof(snor_unprotect_why),
			 "QE stays set (status register 0x%02x): the chip does not take the write, "
			 "or its QE is fixed at the factory", sr);
		printf("Unprotect: %s.\n", snor_unprotect_why);
		return -2;
	}
	printf("QE: cleared. A board that reads this chip in quad mode may not boot until QE is set again.\n");
	return 0;
}

/*
 * Read the status register until the write in progress bit is clear: the
 * chip is ready. The other bits (block protection, write enable latch, quad
 * enable) say nothing about it. Gives up after (sleep_ms + 1) / 2 seconds of
 * wall-clock time, whatever a status read costs over USB.
 * Returns non-zero if error.
 */
static long snor_ms_since(const struct timespec *t0)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (t.tv_sec - t0->tv_sec) * 1000L + (t.tv_nsec - t0->tv_nsec) / 1000000L;
}

static int snor_wait_ready(int sleep_ms)
{
	struct timespec t0;
	u8 sr = 0;

	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (;;) {
		if (snor_read_sr(OPCODE_RDSR, &sr) < 0) {
			printf("%s: status read failed\n", __func__);
			return -1;
		}
		if (!(sr & SR_WIP))
			return 0;
		if (snor_ms_since(&t0) > (sleep_ms + 1) * 500L)
			break;
		udelay(500);
	}
	printf("%s: the chip is still busy (status %02x)\n", __func__, sr);
	return -1;
}

/*
 * read status register
 */
static int snor_read_rg(u8 code, u8 *val)
{
	int retval;

	SPI_CONTROLLER_Chip_Select_Low();
	SPI_CONTROLLER_Write_One_Byte(code);
	retval = SPI_CONTROLLER_Read_NByte(val, 1, SPI_CONTROLLER_SPEED_SINGLE);
	SPI_CONTROLLER_Chip_Select_High();
	if (retval) {
		printf("%s: ret: %x\n", __func__, retval);
		return -1;
	}

	return 0;
}

/*
 * write status register
 */
static int snor_write_rg(u8 code, u8 *val)
{
	int retval;

	SPI_CONTROLLER_Chip_Select_Low();
	SPI_CONTROLLER_Write_One_Byte(code);
	retval = SPI_CONTROLLER_Write_NByte(val, 1, SPI_CONTROLLER_SPEED_SINGLE);
	SPI_CONTROLLER_Chip_Select_High();
	if (retval) {
		printf("%s: ret: %x\n", __func__, retval);
		return -1;
	}

	return 0;
}

static int snor_4byte_mode(int enable)
{
	int retval;

	if (snor_wait_ready(1))
		return -1;

	if (spi_chip_info->id == 0x1) /* Spansion */
	{
		u8 br_cfn;
		u8 br = enable ? 0x81 : 0;

		snor_write_rg(OPCODE_BRWR, &br);
		snor_read_rg(OPCODE_BRRD, &br_cfn);
		if (br_cfn != br) {
			printf("4B mode switch failed %s, 0x%02x, 0x%02x\n", enable ? "enable" : "disable" , br_cfn, br);
			return -1;
		}
	} else {
		u8 code = enable ? 0xb7 : 0xe9; /* B7: enter 4B, E9: exit 4B */

		SPI_CONTROLLER_Chip_Select_Low();
		retval = SPI_CONTROLLER_Write_One_Byte(code);
		SPI_CONTROLLER_Chip_Select_High();
		if (retval) {
			printf("%s: ret: %x\n", __func__, retval);
			return -1;
		}
		if ((!enable) && (spi_chip_info->id == 0xef)) /* Winbond */
		{
			code = 0;
			snor_write_enable();
			snor_write_rg(0xc5, &code);
		}
	}
	return 0;
}

/*
 * Erase one sector of flash memory at offset ``offset'' which is any
 * address within the sector which should be erased.
 *
 * Returns 0 if successful, non-zero otherwise.
 */
static int snor_erase_sector(unsigned long offset)
{
	int ret;

	snor_dbg("%s: offset:%x\n", __func__, offset);

	/* Wait until finished previous write command. */
	if (snor_wait_ready(950))
		return -1;

	if (spi_chip_info->addr4b) {
		snor_4byte_mode(1);
	}

	/* Send write enable, then erase commands. */
	snor_write_enable();

	SPI_CONTROLLER_Chip_Select_Low();

	SPI_CONTROLLER_Write_One_Byte(snor_is_sst26() ? OPCODE_P4E : OPCODE_SE);
	if (spi_chip_info->addr4b)
		SPI_CONTROLLER_Write_One_Byte((offset >> 24) & 0xff);
	SPI_CONTROLLER_Write_One_Byte((offset >> 16) & 0xff);
	SPI_CONTROLLER_Write_One_Byte((offset >> 8) & 0xff);
	SPI_CONTROLLER_Write_One_Byte(offset & 0xff);

	SPI_CONTROLLER_Chip_Select_High();

	ret = snor_wait_ready(950);

	if (spi_chip_info->addr4b)
		snor_4byte_mode(0);

	return ret;
}

static int full_erase_chip(void)
{
	int ret;

	timer_start();
	/* Wait until finished previous write command. */
	if (snor_wait_ready(3))
		return -1;

	/* Unprotect first: clearing the protection ends with the write enable
	   latch cleared, so the write enable goes right before the erase. */
	if (unprotect(0) < 0) {
		timer_end();
		return -1;
	}
	snor_write_enable();

	SPI_CONTROLLER_Chip_Select_Low();
	SPI_CONTROLLER_Write_One_Byte(OPCODE_BE1);
	SPI_CONTROLLER_Chip_Select_High();

	/* up to some minutes on the big chips: give it about 17 */
	ret = snor_wait_ready(2000);
	snor_write_disable();
	timer_end();

	return ret;
}

static struct chip_info chips_data [] = {
	/* REVISIT: fill in JEDEC ids, for parts that have them */
	{ "AT25DF321",		0x1f, 0x47000000, 64 * 1024, 64,  0 },
	{ "AT26DF161",		0x1f, 0x46000000, 64 * 1024, 32,  0 },

	{ "F25L016",		0x8c, 0x21150000, 64 * 1024, 32,  0 }, //ESMT
	{ "F25L16QA",		0x8c, 0x41158c41, 64 * 1024, 32,  0 },
	{ "F25L032",		0x8c, 0x21160000, 64 * 1024, 64,  0 },
	{ "F25L32QA",		0x8c, 0x41168c41, 64 * 1024, 64,  0 },
	{ "F25L064",		0x8c, 0x21170000, 64 * 1024, 128, 0 },
	{ "F25L64QA",		0x8c, 0x41170000, 64 * 1024, 128, 0 },

	{ "GD25Q16",		0xc8, 0x40150000, 64 * 1024, 32,  0 },
	{ "GD25Q32",		0xc8, 0x40160000, 64 * 1024, 64,  0 },
	{ "GD25Q64CSIG",	0xc8, 0x4017c840, 64 * 1024, 128, 0 },
	{ "GD25Q128CSIG",	0xc8, 0x4018c840, 64 * 1024, 256, 0 },
	{ "YC25Q128",		0xd8, 0x4018d840, 64 * 1024, 256, 0 },
	{ "GD25Q256CSIG",	0xc8, 0x4019c840, 64 * 1024, 512, 1 },
	{ "GD25Q20C",		0xc8, 0x40120000, 64 * 1024, 4,   0 },
	{ "GD25Q40C",		0xc8, 0x40130000, 64 * 1024, 8,   0 },
	{ "GD25Q80C",		0xc8, 0x40140000, 64 * 1024, 16,  0 },
	{ "GD25LQ80C",		0xc8, 0x60140000, 64 * 1024, 16,  0 },
	{ "GD25WD80C",		0xc8, 0x64140000, 64 * 1024, 16,  0 },
	{ "GD25WQ80E",		0xc8, 0x65140000, 64 * 1024, 16,  0 },
	{ "GD25LQ16C",		0xc8, 0x60150000, 64 * 1024, 32,  0 },
	{ "GD25WQ16E",		0xc8, 0x65150000, 64 * 1024, 32,  0 },
	{ "GD25LQ32E",		0xc8, 0x60160000, 64 * 1024, 64,  0 },
	{ "GD25WQ32E",		0xc8, 0x65160000, 64 * 1024, 64,  0 },
	{ "GD25LQ64E",		0xc8, 0x60170000, 64 * 1024, 128, 0 },
	{ "GD25LQ128D",		0xc8, 0x60180000, 64 * 1024, 256, 0 },
	{ "GD25F256F",		0xc8, 0x43190000, 64 * 1024, 512, 1 },
	{ "GD25LB256E",		0xc8, 0x6719ff00, 64 * 1024, 512, 1 },

	{ "MX25L1605D",		0xc2, 0x2015c220, 64 * 1024, 32,  0 },
	{ "MX25L3205D",		0xc2, 0x2016c220, 64 * 1024, 64,  0 },
	{ "MX25L6405D",		0xc2, 0x2017c220, 64 * 1024, 128, 0 },
	{ "MX25L12805D",	0xc2, 0x2018c220, 64 * 1024, 256, 0 },
	{ "MX25L25635E",	0xc2, 0x2019c220, 64 * 1024, 512, 1 },
	{ "MX25L51245G",	0xc2, 0x201ac220, 64 * 1024, 1024, 1 },
	{ "MX25L4005A",		0xc2, 0x2013c220, 64 * 1024, 8,   0 },
	{ "MX25L8005M",		0xc2, 0x2014c220, 64 * 1024, 16,  0 },
	{ "MX25U1635F",		0xc2, 0x2535c220, 64 * 1024, 32,  0 },
	{ "MX25U3235F",		0xc2, 0x2536c220, 64 * 1024, 64,  0 },
	{ "MX25U6435F",		0xc2, 0x2537c220, 64 * 1024, 128, 0 },
	{ "MX25U12835F",		0xc2, 0x2538c220, 64 * 1024, 256, 0 },
	{ "MX25U25643G",		0xc2, 0x2539c220, 64 * 1024, 512, 1 },
	{ "MX25U51245G",		0xc2, 0x253ac220, 64 * 1024, 1024, 1 },

	{ "FL016AIF",		0x01, 0x02140000, 64 * 1024, 32,  0 },
	{ "FL064AIF",		0x01, 0x02160000, 64 * 1024, 128, 0 },
	{ "S25FL032P",		0x01, 0x02154D00, 64 * 1024, 64,  0 },
	{ "S25FL064P",		0x01, 0x02164D00, 64 * 1024, 128, 0 },
	{ "S25FL128P",		0x01, 0x20180301, 64 * 1024, 256, 0 },
	{ "S25FL129P",		0x01, 0x20184D01, 64 * 1024, 256, 0 },
	{ "S25FL256S",		0x01, 0x02194D01, 64 * 1024, 512, 1 },
	{ "S25FL116K",		0x01, 0x40150140, 64 * 1024, 32,  0 },
	{ "S25FL132K",		0x01, 0x40160140, 64 * 1024, 64,  0 },
	{ "S25FL164K",		0x01, 0x40170140, 64 * 1024, 128, 0 },
	{ "S25FL512S",		0x01, 0x02204d00, 256 * 1024, 256, 1 },

	{ "EN25F16",		0x1c, 0x31151c31, 64 * 1024, 32,  0 },
	{ "EN25Q16",		0x1c, 0x30151c30, 64 * 1024, 32,  0 },
	{ "EN25QH16",		0x1c, 0x70151c70, 64 * 1024, 32,  0 },
	{ "EN25Q32B",		0x1c, 0x30161c30, 64 * 1024, 64,  0 },
	{ "EN25F32",		0x1c, 0x31161c31, 64 * 1024, 64,  0 },
	{ "EN25F64",		0x1c, 0x20171c20, 64 * 1024, 128, 0 },
	{ "EN25Q64",		0x1c, 0x30171c30, 64 * 1024, 128, 0 },
	{ "EN25QA64A",		0x1c, 0x60170000, 64 * 1024, 128, 0 },
	{ "EN25QH64A",		0x1c, 0x70171c70, 64 * 1024, 128, 0 },
	{ "EN25Q128",		0x1c, 0x30181c30, 64 * 1024, 256, 0 },
	{ "EN25QA128A",		0x1c, 0x60180000, 64 * 1024, 256, 0 },
	{ "EN25QH128A",		0x1c, 0x70181c70, 64 * 1024, 256, 0 },
	{ "GM25Q64A",		0x1c, 0x40171c40, 64 * 1024, 128, 0 },
	{ "GM25Q128A",		0x1c, 0x40181c40, 64 * 1024, 256, 0 },
	{ "EN25XQ128A",		0x1c, 0x71181c71, 64 * 1024, 256, 0 },
	{ "EN25Q256",		0x1c, 0x70191c70, 64 * 1024, 512, 1 },

	{ "W25X05",		0xef, 0x30100000, 64 * 1024, 1,   0 },
	{ "W25X10",		0xef, 0x30110000, 64 * 1024, 2,   0 },
	{ "W25X20",		0xef, 0x30120000, 64 * 1024, 4,   0 },
	{ "W25X40",		0xef, 0x30130000, 64 * 1024, 8,   0 },
	{ "W25X80",		0xef, 0x30140000, 64 * 1024, 16,  0 },
	{ "W25X16",		0xef, 0x30150000, 64 * 1024, 32,  0 },
	{ "W25X32VS",		0xef, 0x30160000, 64 * 1024, 64,  0 },
	{ "W25X64",		0xef, 0x30170000, 64 * 1024, 128, 0 },
	{ "W25Q20CL",		0xef, 0x40120000, 64 * 1024, 4,   0 },
	{ "W25Q20BW",		0xef, 0x50120000, 64 * 1024, 4,   0 },
	{ "W25Q20EW",		0xef, 0x60120000, 64 * 1024, 4,   0 },
	{ "W25Q80BW",		0xef, 0x50140000, 64 * 1024, 16,  0 },
	{ "W25Q80BL",		0xef, 0x40140000, 64 * 1024, 16,  0 },
	{ "W25Q16JQ",		0xef, 0x40150000, 64 * 1024, 32,  0 },
	{ "W25Q16JM",		0xef, 0x70150000, 64 * 1024, 32,  0 },
	{ "W25Q32BV",		0xef, 0x40160000, 64 * 1024, 64,  0 },
	{ "W25Q32DW",		0xef, 0x60160000, 64 * 1024, 64,  0 },
	{ "W25Q64BV",		0xef, 0x40170000, 64 * 1024, 128, 0 },
	{ "W25Q64DW",		0xef, 0x60170000, 64 * 1024, 128, 0 },
	{ "W25Q128BV",		0xef, 0x40180000, 64 * 1024, 256, 0 },
	{ "W25Q128FW",		0xef, 0x60180000, 64 * 1024, 256, 0 },
	{ "W25Q256FV",		0xef, 0x40190000, 64 * 1024, 512, 1 },
	{ "W25Q512JV",		0xef, 0x71190000, 64 * 1024, 1024, 1 },
	{ "W25Q32JWIM",		0xef, 0x80160000, 64 * 1024, 64,  0 },
	{ "W25Q64JVIM",		0xef, 0x70170000, 64 * 1024, 128, 0 },
	{ "W25Q64JWIM",		0xef, 0x80170000, 64 * 1024, 128, 0 },
	{ "W25Q256JW",		0xef, 0x60190000, 64 * 1024, 512, 1 },
	{ "W25Q256JWIM",		0xef, 0x80190000, 64 * 1024, 512, 1 },
	{ "W25Q512JV",		0xef, 0x40200000, 64 * 1024, 1024, 1 },
	{ "W25Q512JVIM",		0xef, 0x70200000, 64 * 1024, 1024, 1 },
	{ "W25Q512NW",		0xef, 0x60200000, 64 * 1024, 1024, 1 },
	{ "W25Q512NWIM",		0xef, 0x80200000, 64 * 1024, 1024, 1 },

	{ "M25P016",		0x20, 0x20150000, 64 * 1024, 32,  0 },
	{ "N25Q032A13",		0x20, 0xba161000, 64 * 1024, 64,  0 },
	{ "N25Q064A13",		0x20, 0xba171000, 64 * 1024, 128, 0 },
	{ "M25P128",		0x20, 0x20180000, 64 * 1024, 256, 0 },
	{ "N25Q128A13",		0x20, 0xba181000, 64 * 1024, 256, 0 },
	{ "XM25QH32B",		0x20, 0x40160000, 64 * 1024, 64,  0 },
	{ "XM25QH32A",		0x20, 0x70160000, 64 * 1024, 64,  0 },
	{ "XM25QH64A",		0x20, 0x70170000, 64 * 1024, 128, 0 },
	{ "XM25QH128A",		0x20, 0x70182070, 64 * 1024, 256, 0 },
	{ "XM25QH64C",		0x20, 0x40170000, 64 * 1024, 128, 0 },
	{ "XM25QH128C",		0x20, 0x40182070, 64 * 1024, 256, 0 },
	{ "N25Q256A13",		0x20, 0xba191000, 64 * 1024, 512, 1 },
	{ "MT25QL512AB",	0x20, 0xba201044, 64 * 1024, 1024, 1 },
	{ "M25P05",		0x20, 0x20100000, 64 * 1024, 1,   0 },
	{ "M25P10",		0x20, 0x20110000, 64 * 1024, 2,   0 },
	{ "M25P20",		0x20, 0x20120000, 64 * 1024, 4,   0 },
	{ "M25P40",		0x20, 0x20130000, 64 * 1024, 8,   0 },
	{ "M25P80",		0x20, 0x20140000, 64 * 1024, 16,  0 },
	{ "M25P32",		0x20, 0x20160000, 64 * 1024, 64,  0 },
	{ "M25P64",		0x20, 0x20170000, 64 * 1024, 128, 0 },
	{ "N25Q016A11",		0x20, 0xbb151000, 64 * 1024, 32,  0 },
	{ "N25Q032A11",		0x20, 0xbb161000, 64 * 1024, 64,  0 },
	{ "N25Q064A11",		0x20, 0xbb171000, 64 * 1024, 128, 0 },
	{ "N25Q128A11",		0x20, 0xbb181000, 64 * 1024, 256, 0 },
	{ "MT25QU256AB",		0x20, 0xbb191000, 64 * 1024, 512, 1 },
	{ "MT25QU512AB",		0x20, 0xbb201044, 64 * 1024, 1024, 1 },
	{ "XM25QH10B",		0x20, 0x40110000, 64 * 1024, 2,   0 },
	{ "XM25QH20B",		0x20, 0x40120000, 64 * 1024, 4,   0 },
	{ "XM25QU41B",		0x20, 0x50130000, 64 * 1024, 8,   0 },
	{ "XM25QH40B",		0x20, 0x40130000, 64 * 1024, 8,   0 },
	{ "XM25QU80B",		0x20, 0x50140000, 64 * 1024, 16,  0 },
	{ "XM25QH80B",		0x20, 0x40140000, 64 * 1024, 16,  0 },
	{ "XM25QU16B",		0x20, 0x50150000, 64 * 1024, 32,  0 },
	{ "XM25QH16C",		0x20, 0x40150000, 64 * 1024, 32,  0 },
	{ "XM25QW16C",		0x20, 0x42150000, 64 * 1024, 32,  0 },
	{ "XM25QW32C",		0x20, 0x42160000, 64 * 1024, 64,  0 },
	{ "XM25LU32C",		0x20, 0x50160000, 64 * 1024, 64,  0 },
	{ "XM25LU64C",		0x20, 0x41170000, 64 * 1024, 128, 0 },
	{ "XM25QW64C",		0x20, 0x42170000, 64 * 1024, 128, 0 },
	{ "XM25LU128C",		0x20, 0x41180000, 64 * 1024, 256, 0 },
	{ "XM25QW128C",		0x20, 0x42180000, 64 * 1024, 256, 0 },
	{ "XM25QH256C",		0x20, 0x40190000, 64 * 1024, 512, 1 },
	{ "XM25QU256C",		0x20, 0x41190000, 64 * 1024, 512, 1 },
	{ "XM25QW256C",		0x20, 0x42190000, 64 * 1024, 512, 1 },
	{ "XM25QH512C",		0x20, 0x40200000, 64 * 1024, 1024, 1 },
	{ "XM25QU512C",		0x20, 0x41200000, 64 * 1024, 1024, 1 },
	{ "XM25QW512C",		0x20, 0x42200000, 64 * 1024, 1024, 1 },

	{ "ZB25VQ16",		0x5e, 0x40150000, 64 * 1024, 32,  0 },
	{ "ZB25VQ32",		0x5e, 0x40160000, 64 * 1024, 64,  0 },
	{ "ZB25VQ64",		0x5e, 0x40170000, 64 * 1024, 128, 0 },
	{ "ZB25VQ128",		0x5e, 0x40180000, 64 * 1024, 256, 0 },
	{ "ZB25LQ16",		0x5e, 0x50150000, 64 * 1024, 32,  0 },
	{ "ZB25LQ32",		0x5e, 0x50160000, 64 * 1024, 64,  0 },
	{ "ZB25LQ64",		0x5e, 0x50170000, 64 * 1024, 128, 0 },
	{ "ZB25LQ128",		0x5e, 0x50180000, 64 * 1024, 256, 0 },

	{ "BY25Q16BS",		0x68, 0x40150000, 64 * 1024, 32,  0 },
	{ "BY25Q32BS",		0x68, 0x40160000, 64 * 1024, 64,  0 },
	{ "BY25Q64AS",		0x68, 0x40170000, 64 * 1024, 128, 0 },
	{ "BY25Q128AS",		0x68, 0x40180000, 64 * 1024, 256, 0 },
	{ "BY25D05AS",		0x68, 0x40100000, 64 * 1024, 1,   0 },
	{ "BY25D10AS",		0x68, 0x40110000, 64 * 1024, 2,   0 },
	{ "BY25D20AS",		0x68, 0x40120000, 64 * 1024, 4,   0 },
	{ "BY25D40AS",		0x68, 0x40130000, 64 * 1024, 8,   0 },
	{ "BY25Q40BL",		0x68, 0x10130000, 64 * 1024, 8,   0 },
	{ "BY25Q40BL",		0x68, 0x60130000, 64 * 1024, 8,   0 },
	{ "BY25Q80BS",		0x68, 0x40140000, 64 * 1024, 16,  0 },
	{ "BY25Q16BL",		0x68, 0x10150000, 64 * 1024, 32,  0 },
	{ "BY25Q32AL",		0x68, 0x60160000, 64 * 1024, 64,  0 },
	{ "BY25Q64AL",		0x68, 0x60170000, 64 * 1024, 128, 0 },
	{ "BY25Q128EL",		0x68, 0x60180000, 64 * 1024, 256, 0 },
	{ "BY25Q256ES",		0x68, 0x40190000, 64 * 1024, 512, 1 },

	{ "XT25F32B",		0x0b, 0x40150000, 64 * 1024, 32,  0 },
	{ "XT25F32B",		0x0b, 0x40160000, 64 * 1024, 64,  0 },
	{ "XT25F64B",		0x0b, 0x40170000, 64 * 1024, 128, 0 },
	{ "XT25F128B",		0x0b, 0x40180000, 64 * 1024, 256, 0 },
	{ "XT25F04D",		0x0b, 0x40130000, 64 * 1024, 8,   0 },
	{ "XT25F08B",		0x0b, 0x40140000, 64 * 1024, 16,  0 },
	{ "XT25F08D",		0x0b, 0x60140000, 64 * 1024, 16,  0 },
	{ "XT25Q16D",		0x0b, 0x60150000, 64 * 1024, 32,  0 },
	{ "XT25Q64D",		0x0b, 0x60170000, 64 * 1024, 128, 0 },
	{ "XT25F128D",		0x0b, 0x60180000, 64 * 1024, 256, 0 },

	{ "PM25LQ016",		0x7f, 0x9d450000, 64 * 1024, 32,  0 },
	{ "PM25LQ032",		0x7f, 0x9d460000, 64 * 1024, 64,  0 },
	{ "PM25LQ064",		0x7f, 0x9d470000, 64 * 1024, 128, 0 },
	{ "PM25LQ128",		0x7f, 0x9d480000, 64 * 1024, 256, 0 },

	{ "IC25LP016",		0x9d, 0x60150000, 64 * 1024, 32,  0 },
	{ "IC25LP032",		0x9d, 0x60160000, 64 * 1024, 64,  0 },
	{ "IC25LP064",		0x9d, 0x60170000, 64 * 1024, 128, 0 },
	{ "IC25LP128",		0x9d, 0x60180000, 64 * 1024, 256, 0 },
	{ "IS25LQ010",		0x9d, 0x40110000, 64 * 1024, 2,   0 },
	{ "IS25LQ020",		0x9d, 0x40120000, 64 * 1024, 4,   0 },
	{ "IS25WP040D",		0x9d, 0x70130000, 64 * 1024, 8,   0 },
	{ "IS25LP080D",		0x9d, 0x60140000, 64 * 1024, 16,  0 },
	{ "IS25WP080D",		0x9d, 0x70140000, 64 * 1024, 16,  0 },
	{ "IS25WP016D",		0x9d, 0x70150000, 64 * 1024, 32,  0 },
	{ "IS25WP032D",		0x9d, 0x70160000, 64 * 1024, 64,  0 },
	{ "IS25WP064D",		0x9d, 0x70170000, 64 * 1024, 128, 0 },
	{ "IS25WP128F",		0x9d, 0x70180000, 64 * 1024, 256, 0 },
	{ "IS25LP256D",		0x9d, 0x60190000, 64 * 1024, 512, 1 },
	{ "IS25WP256D",		0x9d, 0x70190000, 64 * 1024, 512, 1 },
	{ "IS25LP512M",		0x9d, 0x601a0000, 64 * 1024, 1024, 1 },
	{ "IS25WP512M",		0x9d, 0x701a0000, 64 * 1024, 1024, 1 },

	{ "FM25Q16",		0xa1, 0x40150000, 64 * 1024, 32,  0 },
	{ "FM25Q32",		0xa1, 0x40160000, 64 * 1024, 64,  0 },
	{ "FM25Q64",		0xa1, 0x40170000, 64 * 1024, 128, 0 },
	{ "FM25Q128",		0xa1, 0x40180000, 64 * 1024, 256, 0 },
	{ "FM25W16",		0xa1, 0x28150000, 64 * 1024, 32,  0 },
	{ "FM25W32",		0xa1, 0x28160000, 64 * 1024, 64,  0 },
	{ "FM25W64",		0xa1, 0x28170000, 64 * 1024, 128, 0 },
	{ "FM25W128",		0xa1, 0x28180000, 64 * 1024, 256, 0 },
	{ "FM25W04",		0xa1, 0x28130000, 64 * 1024, 8,   0 },
	{ "FM25Q04",		0xa1, 0x40130000, 64 * 1024, 8,   0 },
	{ "FM25Q08",		0xa1, 0x40140000, 64 * 1024, 16,  0 },

	{ "FM25Q16A",		0xf8, 0x32150000, 64 * 1024, 32,  0 },
	{ "FM25Q32A",		0xf8, 0x32160000, 64 * 1024, 64,  0 },
	{ "FM25Q64A",		0xf8, 0x32170000, 64 * 1024, 128, 0 },
	{ "FM25Q128A",		0xf8, 0x32180000, 64 * 1024, 256, 0 },
	{ "FM25Q04A",		0xf8, 0x32130000, 64 * 1024, 8,   0 },
	{ "FM25M04A",		0xf8, 0x42130000, 64 * 1024, 8,   0 },
	{ "FM25Q08A",		0xf8, 0x32140000, 64 * 1024, 16,  0 },
	{ "FM25M08A",		0xf8, 0x42140000, 64 * 1024, 16,  0 },
	{ "FM25M16A",		0xf8, 0x42150000, 64 * 1024, 32,  0 },
	{ "FM25M32B",		0xf8, 0x42160000, 64 * 1024, 64,  0 },
	{ "FM25M64A",		0xf8, 0x42170000, 64 * 1024, 128, 0 },

	{ "PN25F16",		0xe0, 0x40150000, 64 * 1024, 32,  0 },
	{ "PN25F32",		0xe0, 0x40160000, 64 * 1024, 64,  0 },
	{ "PN25F64",		0xe0, 0x40170000, 64 * 1024, 128, 0 },
	{ "PN25F128",		0xe0, 0x40180000, 64 * 1024, 256, 0 },

	{ "P25Q16H",		0x85, 0x60150000, 64 * 1024, 32,  0 },
	{ "P25Q32H",		0x85, 0x60160000, 64 * 1024, 64,  0 },
	{ "P25Q64H",		0x85, 0x60170000, 64 * 1024, 128, 0 },
	{ "P25Q128H",		0x85, 0x60180000, 64 * 1024, 256, 0 },
	{ "PY25Q128HA",		0x85, 0x20180000, 64 * 1024, 256, 0 },
	{ "P25D05H",		0x85, 0x60100000, 64 * 1024, 1,   0 },
	{ "P25D10H",		0x85, 0x60110000, 64 * 1024, 2,   0 },
	{ "P25D20H",		0x85, 0x60120000, 64 * 1024, 4,   0 },
	{ "P25D40H",		0x85, 0x60130000, 64 * 1024, 8,   0 },
	{ "P25D80H",		0x85, 0x60140000, 64 * 1024, 16,  0 },
	{ "PY25Q64HA",		0x85, 0x20170000, 64 * 1024, 128, 0 },
	{ "PY25Q32HB",		0x85, 0x20160000, 64 * 1024, 64,  0 },

	{ "SK25P32",		0x25, 0x60162560, 64 * 1024, 64,  0 },
	{ "SK25P64",		0x25, 0x60172560, 64 * 1024, 128, 0 },
	{ "SK25P128",		0x25, 0x60182560, 64 * 1024, 256, 0 },

	{ "NM25Q64E",		0x52, 0x22170000, 64 * 1024, 128, 0 },
	{ "NM25Q128E",		0x52, 0x21180000, 64 * 1024, 256, 0 },
	{ "NM25L256F",		0x52, 0x10190000, 64 * 1024, 512, 1 },
	{ "A25L10PU",		0x37, 0x20110000, 64 * 1024, 2,   0 },
	{ "A25L20PU",		0x37, 0x20120000, 64 * 1024, 4,   0 },
	{ "A25L040",		0x37, 0x30130000, 64 * 1024, 8,   0 },
	{ "A25LQ080",		0x37, 0x40140000, 64 * 1024, 16,  0 },
	{ "A25L080",		0x37, 0x30140000, 64 * 1024, 16,  0 },
	{ "A25LQ16",		0x37, 0x40150000, 64 * 1024, 32,  0 },
	{ "A25LQ32",		0x37, 0x40160000, 64 * 1024, 64,  0 },
	{ "A25L032",		0x37, 0x30160000, 64 * 1024, 64,  0 },
	{ "A25LQ64",		0x37, 0x40170000, 64 * 1024, 128, 0 },
	{ "ES25P10",		0x4a, 0x20110000, 64 * 1024, 2,   0 },
	{ "ES25P20",		0x4a, 0x20120000, 64 * 1024, 4,   0 },
	{ "ES25P40",		0x4a, 0x20130000, 64 * 1024, 8,   0 },
	{ "ES25P80",		0x4a, 0x20140000, 64 * 1024, 16,  0 },
	{ "ES25P16",		0x4a, 0x20150000, 64 * 1024, 32,  0 },
	{ "ES25P32",		0x4a, 0x20160000, 64 * 1024, 64,  0 },
	{ "ES25M40A",		0x4a, 0x32130000, 64 * 1024, 8,   0 },
	{ "ES25M80A",		0x4a, 0x32140000, 64 * 1024, 16,  0 },
	{ "ES25M16A",		0x4a, 0x32150000, 64 * 1024, 32,  0 },
	{ "DQ25Q64AS",		0x54, 0x40170000, 64 * 1024, 128, 0 },
	{ "DQ25Q128AL",		0x54, 0x60180000, 64 * 1024, 256, 0 },
	{ "MD25D20",		0x51, 0x40120000, 64 * 1024, 4,   0 },
	{ "MD25D40",		0x51, 0x40130000, 64 * 1024, 8,   0 },
	{ "LE25U20AMB",		0x62, 0x06120000, 64 * 1024, 4,   0 },
	{ "LE25U40CMC",		0x62, 0x06130000, 64 * 1024, 8,   0 },
	{ "ZD25Q16A",		0xba, 0x40150000, 64 * 1024, 32,  0 },
	{ "ZD25Q32A",		0xba, 0x40160000, 64 * 1024, 64,  0 },
	{ "ZD25Q64A",		0xba, 0x40170000, 64 * 1024, 128, 0 },
	{ "ZD25Q128A",		0xba, 0x40180000, 64 * 1024, 256, 0 },
	{ "ZD25Q16B",		0xba, 0x32150000, 64 * 1024, 32,  0 },
	{ "ZD25Q32B",		0xba, 0x32160000, 64 * 1024, 64,  0 },
	{ "ZD25Q64B",		0xba, 0x32170000, 64 * 1024, 128, 0 },
	{ "ZD25Q128B",		0xba, 0x32180000, 64 * 1024, 256, 0 },
	{ "PCT25VF020B",		0xbf, 0x258c0000, 64 * 1024, 4,   0 },
	{ "PCT25VF040B",		0xbf, 0x258d0000, 64 * 1024, 8,   0 },
	{ "PCT25VF080B",		0xbf, 0x258e0000, 64 * 1024, 16,  0 },
	{ "PCT25VF016B",		0xbf, 0x25410000, 64 * 1024, 32,  0 },
	{ "PCT25VF032B",		0xbf, 0x254a0000, 64 * 1024, 64,  0 },
	{ "PCT25VF064C",		0xbf, 0x254b0000, 64 * 1024, 128, 0 },
	{ "PCT26VF016",		0xbf, 0x26010000, 4 * 1024, 512,  0 },
	{ "PCT26VF032",		0xbf, 0x26020000, 4 * 1024, 1024, 0 },
	{ "ZD25Q16C",		0xba, 0x60150000, 64 * 1024, 32,  0 },
	{ "ZD25Q32C",		0xba, 0x60160000, 64 * 1024, 64,  0 },
	{ "ZD25Q64C",		0xba, 0x60170000, 64 * 1024, 128, 0 },
	{ "ZD25Q128C",		0xba, 0x60180000, 64 * 1024, 256, 0 },

};

/*
 * read SPI flash device ID
 */
static int snor_read_devid(u8 *rxbuf, int n_rx)
{
	int retval = 0;

	SPI_CONTROLLER_Chip_Select_Low();
	SPI_CONTROLLER_Write_One_Byte(OPCODE_RDID);

	retval = SPI_CONTROLLER_Read_NByte(rxbuf, n_rx, SPI_CONTROLLER_SPEED_SINGLE);
	SPI_CONTROLLER_Chip_Select_High();
	if (retval) {
		printf("%s: ret: %x\n", __func__, retval);
		return retval;
	}

	return 0;
}

/*
 * read status register
 */
static int snor_read_sr(u8 opcode, u8 *val)
{
	int retval = 0;

	SPI_CONTROLLER_Chip_Select_Low();
	SPI_CONTROLLER_Write_One_Byte(opcode);

	retval = SPI_CONTROLLER_Read_NByte(val, 1, SPI_CONTROLLER_SPEED_SINGLE);
	SPI_CONTROLLER_Chip_Select_High();
	if (retval) {
		printf("%s: ret: %x\n", __func__, retval);
		return retval;
	}

	return 0;
}

/*
 * write status register
 */
static int snor_write_sr(u8 opcode, u8 *val)
{
	int retval = 0;

	SPI_CONTROLLER_Chip_Select_Low();
	SPI_CONTROLLER_Write_One_Byte(opcode);

	retval = SPI_CONTROLLER_Write_NByte(val, 1, SPI_CONTROLLER_SPEED_SINGLE);
	SPI_CONTROLLER_Chip_Select_High();
	if (retval) {
		printf("%s: ret: %x\n", __func__, retval);
		return retval;
	}
	return 0;
}

struct chip_info *chip_prob(void)
{
	struct chip_info *info = NULL, *match = NULL;
	u8 buf[5];
	u32 jedec, jedec_strip, weight;
	int i;

	snor_read_devid(buf, 5);
	jedec = (u32)((u32)(buf[1] << 24) | ((u32)buf[2] << 16) | ((u32)buf[3] <<8) | (u32)buf[4]);
	jedec_strip = jedec & 0xffff0000;

	printf("spi device id: %x %x %x %x %x (%x)\n", buf[0], buf[1], buf[2], buf[3], buf[4], jedec);

	// FIXME, assign default as AT25D
	weight = 0xffffffff;
	match = &chips_data[0];
	for (i = 0; i < sizeof(chips_data)/sizeof(chips_data[0]); i++) {
		info = &chips_data[i];
		if (info->id == buf[0]) {
			if ((info->jedec_id == jedec) || ((info->jedec_id & 0xffff0000) == jedec_strip)) {
				u8 sr,sr2,sr3=0;
				snor_read_sr(OPCODE_RDSR, &sr);

				if (info->id == 0xc2) {	/* Macronix: one status register */
					printf("Detected SPI NOR Flash: %s, Flash Size: %ld MB, SR: [%02x]\n", info->name, (info->sector_size * info->n_sectors) >> 20, sr);
					return info;
				}
				snor_read_sr(OPCODE_RDSR2, &sr2);
				snor_read_sr(OPCODE_RDSR3, &sr3);

				printf("Detected SPI NOR Flash: %s, Flash Size: %ld MB, SR: [%02x] [%02x] [%02x]\n", info->name, (info->sector_size * info->n_sectors) >> 20, sr, sr2, sr3);
				return info;
			}

			if (weight > (info->jedec_id ^ jedec)) {
				weight = info->jedec_id ^ jedec;
				match = info;
			}
		}
	}
	printf("SPI NOR Flash Not Detected!\n");
	match = NULL; /* Not support JEDEC calculate info */

	return match;
}

/* The table entry for a JEDEC ID that a programmer read itself (one that
   does the SPI in its own firmware): the name, or NULL. */
const char *snor_lookup(const unsigned char id[3], unsigned long *size, unsigned long *sector, int *addr4b)
{
	u32 dev = ((u32)id[1] << 24) | ((u32)id[2] << 16);

	for (unsigned i = 0; i < sizeof(chips_data) / sizeof(chips_data[0]); i++) {
		const struct chip_info *info = &chips_data[i];

		if (info->id == id[0] && (info->jedec_id & 0xffff0000) == dev) {
			*size = info->sector_size * info->n_sectors;
			*sector = info->sector_size;
			*addr4b = info->addr4b;
			return info->name;
		}
	}
	return NULL;
}

/* Status registers 1, 2 and 3 of the chip found by snor_init() */
int snor_read_status(unsigned char sr[3])
{
	if (snor_read_sr(OPCODE_RDSR, &sr[0]) < 0 || snor_read_sr(OPCODE_RDSR2, &sr[1]) < 0 ||
	    snor_read_sr(OPCODE_RDSR3, &sr[2]) < 0)
		return -1;
	return 0;
}

long snor_init(void)
{
	spi_chip_info = chip_prob();

	if(spi_chip_info == NULL)
		return -1;

	bsize = spi_chip_info->sector_size;
	flash_chip_name = spi_chip_info->name;

	return spi_chip_info->sector_size * spi_chip_info->n_sectors;
}

int snor_erase(unsigned long offs, unsigned long len)
{
	unsigned long plen = len;
	snor_dbg("%s: offs:%x len:%x\n", __func__, offs, len);

	/* sanity checks */
	if (len == 0)
		return -1;

	if(!offs && len == (spi_chip_info->sector_size * spi_chip_info->n_sectors))
	{
		printf("Please Wait......\n");
		return full_erase_chip();
	}

	timer_start();

	if (unprotect(0) < 0) {
		timer_end();
		return -1;
	}

	/* now erase those sectors */
	while (len > 0) {
		if (snor_erase_sector(offs)) {
			printf("\nErase failed at 0x%lx\n", offs);
			timer_end();
			return -1;
		}

		offs += spi_chip_info->sector_size;
		len -= spi_chip_info->sector_size;
		if( timer_progress() )
		{
			printf("\bErase %ld%% [%lu] of [%lu] bytes      ", 100 * (plen - len) / plen, plen - len, plen);
			printf("\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b");
			fflush(stdout);
		}
	}
	printf("Erase 100%% [%lu] of [%lu] bytes      \n", plen - len, plen);
	timer_end();

	return 0;
}

int snor_read(unsigned char *buf, unsigned long from, unsigned long len)
{
	u32 read_addr, physical_read_addr, remain_len, data_offset;
	int failed;

	snor_dbg("%s: from:%x len:%x \n", __func__, from, len);

	/* sanity checks */
	if (len == 0)
		return 0;

	timer_start();
	/* Wait till previous write/erase is done. */
	if (snor_wait_ready(1)) {
		/* REVISIT status return?? */
		return -1;
	}

	read_addr = from;
	remain_len = len;
	failed = 0;

	while(remain_len > 0) {

		physical_read_addr = read_addr;
		data_offset = (physical_read_addr % (spi_chip_info->sector_size));

		if (spi_chip_info->addr4b)
			snor_4byte_mode(1);

		SPI_CONTROLLER_Chip_Select_Low();

		/* Set up the write data buffer. */
		SPI_CONTROLLER_Write_One_Byte(OPCODE_READ);

		if (spi_chip_info->addr4b)
			SPI_CONTROLLER_Write_One_Byte((physical_read_addr >> 24) & 0xff);
		SPI_CONTROLLER_Write_One_Byte((physical_read_addr >> 16) & 0xff);
		SPI_CONTROLLER_Write_One_Byte((physical_read_addr >> 8) & 0xff);
		SPI_CONTROLLER_Write_One_Byte(physical_read_addr & 0xff);

		if( (data_offset + remain_len) < spi_chip_info->sector_size )
		{
			if(SPI_CONTROLLER_Read_NByte(&buf[len - remain_len], remain_len, SPI_CONTROLLER_SPEED_SINGLE)) {
				SPI_CONTROLLER_Chip_Select_High();
				if (spi_chip_info->addr4b)
					snor_4byte_mode(0);
				failed = 1;
				break;
			}
			remain_len = 0;
		} else {
			if(SPI_CONTROLLER_Read_NByte(&buf[len - remain_len], spi_chip_info->sector_size - data_offset, SPI_CONTROLLER_SPEED_SINGLE)) {
				SPI_CONTROLLER_Chip_Select_High();
				if (spi_chip_info->addr4b)
					snor_4byte_mode(0);
				failed = 1;
				break;
			}
			remain_len -= spi_chip_info->sector_size - data_offset;
			read_addr += spi_chip_info->sector_size - data_offset;
			if( timer_progress() ) {
				printf("\bRead %ld%% [%lu] of [%lu] bytes      ", 100 * (len - remain_len) / len, len - remain_len, len);
				printf("\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b");
				fflush(stdout);
			}
		}

		SPI_CONTROLLER_Chip_Select_High();

		if (spi_chip_info->addr4b)
			snor_4byte_mode(0);
	}
	if (failed) {
		printf("\nRead failed at 0x%08x after [%lu] of [%lu] bytes\n", read_addr, len - remain_len, len);
		timer_end();
		return -1;
	}
	printf("Read 100%% [%lu] of [%lu] bytes      \n", len - remain_len, len);
	timer_end();

	return len;
}

/* Wait for the end of a byte or word program: the busy bit only (in AAI
   mode bit 6 of the status register is set, so snor_wait_ready() cannot be used). */
static int snor_wait_busy(void)
{
	u8 sr = 0;
	int count;

	for (count = 0; count < 1000; count++) {
		if (snor_read_sr(OPCODE_RDSR, &sr) < 0)
			return -1;
		if (!(sr & SR_WIP))
			return 0;
	}
	return -1;
}

static int snor_send_addr(unsigned long addr)
{
	int err = 0;

	err |= SPI_CONTROLLER_Write_One_Byte((addr >> 16) & 0xff);
	err |= SPI_CONTROLLER_Write_One_Byte((addr >> 8) & 0xff);
	err |= SPI_CONTROLLER_Write_One_Byte(addr & 0xff);
	return err;
}

static int snor_program_byte(unsigned long addr, u8 data)
{
	int err = 0;

	snor_write_enable();
	err |= SPI_CONTROLLER_Chip_Select_Low();
	err |= SPI_CONTROLLER_Write_One_Byte(OPCODE_PP);
	err |= snor_send_addr(addr);
	err |= SPI_CONTROLLER_Write_One_Byte(data);
	err |= SPI_CONTROLLER_Chip_Select_High();
	if (err)
		return -1;
	return snor_wait_busy();
}

/* SST25VF-type write: a byte program for an odd first or last byte, AAI word
   program (two bytes per command) for the rest, write disable at the end. */
static int snor_write_aai(unsigned char *buf, unsigned long to, unsigned long len)
{
	unsigned long plen = len;
	int aai = 0;

	if (unprotect(0) < 0)
		goto fail;

	if ((to & 1) && len) {
		if (snor_program_byte(to, *buf))
			goto fail;
		to++; buf++; len--;
	}
	while (len >= 2) {
		int err = 0;

		if (!aai)
			snor_write_enable();
		err |= SPI_CONTROLLER_Chip_Select_Low();
		err |= SPI_CONTROLLER_Write_One_Byte(OPCODE_AAI);
		if (!aai)
			err |= snor_send_addr(to);
		err |= SPI_CONTROLLER_Write_One_Byte(buf[0]);
		err |= SPI_CONTROLLER_Write_One_Byte(buf[1]);
		err |= SPI_CONTROLLER_Chip_Select_High();
		aai = 1;
		/* the word counts only when its command went out and the chip is ready
		   again (BUSY, bit 0: bit 6 only says AAI mode is on) */
		if (err || snor_wait_busy())
			goto fail;
		to += 2; buf += 2; len -= 2;
		if( timer_progress() ) {
			printf("\bWritten %ld%% [%lu] of [%lu] bytes      ", 100 * (plen - len) / plen, plen - len, plen);
			printf("\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b");
			fflush(stdout);
		}
	}
	if (aai) {
		snor_write_disable();	/* ends AAI mode */
		aai = 0;
		if (snor_wait_busy())
			goto fail;
	}
	if (len) {
		if (snor_program_byte(to, *buf))
			goto fail;
		len--;
	}
	snor_write_disable();
	printf("Written 100%% [%lu] of [%lu] bytes      \n", plen - len, plen);
	timer_end();
	return plen;

fail:
	if (aai)
		snor_write_disable();
	printf("\nWrite failed at 0x%06lx\n", to);
	timer_end();
	return plen - len;
}

int snor_write(unsigned char *buf, unsigned long to, unsigned long len)
{
	u32 page_offset, page_size;
	int rc = 0, retlen = 0;
	unsigned long plen = len;

	snor_dbg("%s: to:%x len:%x \n", __func__, to, len);

	/* sanity checks */
	if (len == 0)
		return 0;

	if (to + len > spi_chip_info->sector_size * spi_chip_info->n_sectors)
		return -1;

	timer_start();
	/* Wait until finished previous write command. */
	if (snor_wait_ready(2)) {
		return -1;
	}

	if (snor_is_sst25_aai())
		return snor_write_aai(buf, to, len);

	/* the protection is cleared once, before the first page: it stays
	   cleared, and checking it again for every page is three status reads */
	if (unprotect(0) < 0) {
		timer_end();
		return -1;
	}

	/* what page do we start with? */
	page_offset = to % FLASH_PAGESIZE;

	if (spi_chip_info->addr4b)
		snor_4byte_mode(1);

	/* write everything in PAGESIZE chunks */
	while (len > 0) {
		page_size = min(len, FLASH_PAGESIZE - page_offset);
		page_offset = 0;
		/* write the next page to flash */

		/* the page before is not written until the chip is ready */
		if (snor_wait_ready(3)) {
			retlen -= rc;
			break;
		}
		snor_write_enable();

		SPI_CONTROLLER_Chip_Select_Low();
		/* Set up the opcode in the write buffer. */
		SPI_CONTROLLER_Write_One_Byte(OPCODE_PP);

		if (spi_chip_info->addr4b)
			SPI_CONTROLLER_Write_One_Byte((to >> 24) & 0xff);
		SPI_CONTROLLER_Write_One_Byte((to >> 16) & 0xff);
		SPI_CONTROLLER_Write_One_Byte((to >> 8) & 0xff);
		SPI_CONTROLLER_Write_One_Byte(to & 0xff);

		if(!SPI_CONTROLLER_Write_NByte(buf, page_size, SPI_CONTROLLER_SPEED_SINGLE))
			rc = page_size;
		else
			rc = 1;

		SPI_CONTROLLER_Chip_Select_High();

		snor_dbg("%s: to:%x page_size:%x ret:%x\n", __func__, to, page_size, rc);

		if( timer_progress() ) {
			printf("\bWritten %ld%% [%lu] of [%lu] bytes      ", 100 * (plen - len) / plen, plen - len, plen);
			printf("\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b");
			fflush(stdout);
		}

		if (rc > 0) {
			retlen += rc;
			if (rc < page_size) {
				printf("%s: rc:%x page_size:%x\n",
						__func__, rc, page_size);
				snor_write_disable();
				return retlen - rc;
			}
		}

		len -= page_size;
		to += page_size;
		buf += page_size;
	}

	if (len == 0 && snor_wait_ready(3))	/* the last page */
		retlen -= rc;

	if (spi_chip_info->addr4b)
		snor_4byte_mode(0);

	snor_write_disable();

	if (retlen == (int)plen)
		printf("Written 100%% [%lu] of [%lu] bytes      \n", plen, plen);
	else
		printf("Written [%d] of [%lu] bytes\n", retlen, plen);
	timer_end();

	return retlen;
}

void support_snor_list(void)
{
	int i;

	printf("SPI NOR Flash Support List:\n");
	for ( i = 0; i < (sizeof(chips_data)/sizeof(struct chip_info)); i++)
	{
		printf("%03d. %s\n", i + 1, chips_data[i].name);
	}
}
/* End of [spi_nor_flash.c] package */

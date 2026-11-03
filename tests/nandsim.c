/* tests/nandsim.c — SPI NAND chip simulated at the SPI command level, in place of
   spi_controller.c, and the checks that drive spi_nand_flash.c through it:
   detection by the 2- and 3-byte ID, write and read, and the -k (skip BAD pages) path.
   The row address wraps at the chip end like on a real chip, so a write past the end
   lands on page 0. READ ID (9F) without an address byte answers a dummy byte
   first, so the probe fallback that reads a dummy byte is exercised too. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/types.h"
#include "../src/spi_controller.h"
#include "../src/nandcmd_api.h"
/* the driver is built into this file, so the checks can walk its chip table */
#include "../src/spi_nand_flash.c"

int snand_read(unsigned char *buf, unsigned long from, unsigned long len);
int snand_write(unsigned char *buf, unsigned long to, unsigned long len);
long snand_init(void);
int snand_erase(unsigned long offs, unsigned long len);

#define PAGE 2048
#define OOB 64
#define PAGES 65536			/* 128 MiB, as W25N01G */
#define MAXBAD 8

static unsigned char *mem;		/* PAGES * (PAGE + OOB) */
static unsigned char cache[PAGE + OOB];
static unsigned char id[3];
static unsigned char feat[256];
static unsigned char cmd[16];
static unsigned int ncmd, nread;
static long ecc_bad[MAXBAD], prog_fail[MAXBAD], erase_fail[MAXBAD];	/* erase_fail: block numbers */
static unsigned int nprog;		/* program executes (0x10) */
static int col_hi_read = -1, col_hi_load = -1;	/* high column byte of the last read from cache / program load */
static int stuck_busy;			/* the status register always shows OIP */
static int busy_after = -1;		/* stay busy from this program execute on */

static int in(const long *set, long v) { for (int i = 0; i < MAXBAD; i++) if (set[i] == v) return 1; return 0; }
static unsigned long row(void) { return ((cmd[1] << 16) | (cmd[2] << 8) | cmd[3]) % PAGES; }
static unsigned int col(void) { return ((cmd[1] << 8) | cmd[2]) & 0x0fff; }

SPI_CONTROLLER_RTN_T SPI_CONTROLLER_Enable_Manual_Mode(void) { return SPI_CONTROLLER_RTN_NO_ERROR; }
SPI_CONTROLLER_RTN_T SPI_CONTROLLER_Chip_Select_Low(void) { ncmd = nread = 0; return SPI_CONTROLLER_RTN_NO_ERROR; }

SPI_CONTROLLER_RTN_T SPI_CONTROLLER_Write_NByte(u8 *d, u32 len, SPI_CONTROLLER_SPEED_T speed)
{
	(void)speed;
	for (u32 i = 0; i < len; i++) {
		if (ncmd < 3 || (cmd[0] != 0x02 && cmd[0] != 0x32 && cmd[0] != 0x84 && cmd[0] != 0x34)) {
			if (ncmd < sizeof(cmd)) cmd[ncmd] = d[i];
			ncmd++;
			if (ncmd == 3 && (cmd[0] == 0x02 || cmd[0] == 0x32))
				memset(cache, 0xff, sizeof(cache));	/* program load resets the cache */
			continue;
		}
		unsigned int c = col() + (ncmd++ - 3);
		if (c < sizeof(cache)) cache[c] = d[i];
	}
	return SPI_CONTROLLER_RTN_NO_ERROR;
}

SPI_CONTROLLER_RTN_T SPI_CONTROLLER_Write_One_Byte(u8 data) { return SPI_CONTROLLER_Write_NByte(&data, 1, 0); }

SPI_CONTROLLER_RTN_T SPI_CONTROLLER_Read_NByte(u8 *d, u32 len, SPI_CONTROLLER_SPEED_T speed)
{
	(void)speed;
	for (u32 i = 0; i < len; i++, nread++) {
		switch (cmd[0]) {
		case 0x9f: {		/* without an address byte a dummy byte comes first, as on Winbond */
			unsigned int k = ncmd > 1 ? nread : nread - 1;
			d[i] = (ncmd == 1 && nread == 0) || k >= 3 ? 0 : id[k];
			break;
		}
		case 0x0f: d[i] = feat[cmd[1]] | (stuck_busy && cmd[1] == 0xc0 ? 0x01 : 0); break;
		case 0x03: case 0x0b: case 0x3b: case 0x6b: {
			unsigned int c = col() + nread;
			d[i] = c < sizeof(cache) ? cache[c] : 0xff;
			break;
		}
		default: d[i] = 0xff;
		}
	}
	return SPI_CONTROLLER_RTN_NO_ERROR;
}

SPI_CONTROLLER_RTN_T SPI_CONTROLLER_Chip_Select_High(void)
{
	unsigned char *p;
	switch (cmd[0]) {
	case 0x03: case 0x0b: case 0x3b: case 0x6b: col_hi_read = cmd[1]; break;
	case 0x02: case 0x32: case 0x84: case 0x34: col_hi_load = cmd[1]; break;
	case 0x1f: feat[cmd[1]] = cmd[2]; break;
	case 0x06: feat[0xc0] |= 0x02; break;
	case 0x04: feat[0xc0] &= ~0x02; break;
	case 0x13:				/* page read: ECC result in status bits 5:4 */
		memcpy(cache, mem + row() * (PAGE + OOB), sizeof(cache));
		feat[0xc0] = (feat[0xc0] & ~0x30) | (in(ecc_bad, row()) ? 0x20 : 0);
		break;
	case 0x10:				/* program execute */
		if (busy_after >= 0 && (int)nprog >= busy_after) stuck_busy = 1;
		nprog++;
		feat[0xc0] &= ~0x08;
		if (in(prog_fail, row())) { feat[0xc0] |= 0x08; break; }
		p = mem + row() * (PAGE + OOB);
		for (unsigned int i = 0; i < sizeof(cache); i++) p[i] &= cache[i];
		break;
	case 0xd8:				/* block erase, 64 pages */
		feat[0xc0] &= ~0x04;
		if (in(erase_fail, row() / 64)) { feat[0xc0] |= 0x04; break; }
		memset(mem + (row() & ~63UL) * (PAGE + OOB), 0xff, 64 * (PAGE + OOB));
		break;
	}
	ncmd = nread = 0;
	return SPI_CONTROLLER_RTN_NO_ERROR;
}

SPI_CONTROLLER_RTN_T SPI_CONTROLLER_Xfer_NByte(u8 *in_, u32 len_in, u8 *out, u32 len_out, SPI_CONTROLLER_SPEED_T speed)
{
	SPI_CONTROLLER_Chip_Select_Low();
	SPI_CONTROLLER_Write_NByte(in_, len_in, speed);
	SPI_CONTROLLER_Read_NByte(out, len_out, speed);
	SPI_CONTROLLER_Chip_Select_High();
	return SPI_CONTROLLER_RTN_NO_ERROR;
}

static int fail;
static void check(int ok, const char *what) { printf("%s %s\n", ok ? "ok  " : "FAIL", what); if (!ok) fail = 1; }

static void chip(unsigned char a, unsigned char b, unsigned char c)
{
	id[0] = a; id[1] = b; id[2] = c;
	memset(mem, 0xff, (size_t)PAGES * (PAGE + OOB));
	memset(feat, 0, sizeof(feat));
	for (int i = 0; i < MAXBAD; i++) ecc_bad[i] = prog_fail[i] = erase_fail[i] = -1;
	Skip_BAD_page = 0;
	nprog = 0;
	stuck_busy = 0;
}

static unsigned char *page(long n) { return mem + n * (PAGE + OOB); }
static void fill(unsigned char *b, long pages, int seed) { for (long i = 0; i < pages * PAGE; i++) b[i] = (unsigned char)(i * 7 + seed + i / PAGE); }

int main(void)
{
	static unsigned char buf[8 * PAGE], got[8 * PAGE];
	static const struct { unsigned char id[3]; long size; const char *name; } ids[] = {
		{ { 0xef, 0xaa, 0x21 }, 128L << 20, "W25N01G by 3-byte ID" },
		{ { 0xef, 0xaa, 0x22 }, 256L << 20, "W25N02KV by 3-byte ID" },
		{ { 0xef, 0xab, 0x21 }, 256L << 20, "W25M02G by 3-byte ID" },
		{ { 0xc2, 0x26, 0x03 }, 256L << 20, "MX35LF2GE4AD by 3-byte ID" },
		{ { 0x0b, 0x11, 0x00 }, 128L << 20, "XT26G01C by 2-byte ID" },
		{ { 0xc8, 0xd1, 0x00 }, 128L << 20, "GD5F1GQ4UB by 2-byte ID (old entry)" },
	};
	mem = malloc((size_t)PAGES * (PAGE + OOB));
	if (!mem) return 2;

	for (unsigned i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
		chip(ids[i].id[0], ids[i].id[1], ids[i].id[2]);
		check(snand_init() == ids[i].size, ids[i].name);
	}
	chip(0xef, 0xaa, 0x99);
	check(snand_init() < 0, "EF AA with an unknown third byte is not detected");

	/* every chip of the table is detected by its own ID and gets its own entry;
	   only the three old entries below share an ID with an earlier one */
	{
		const int n = sizeof(spi_nand_flash_tables) / sizeof(spi_nand_flash_tables[0]);
		int bad = 0, shadowed = 0;
		for (int i = 0; i < n; i++) {
			const struct SPI_NAND_FLASH_INFO_T *e = &spi_nand_flash_tables[i];
			chip(e->mfr_id, e->dev_id, e->dev_id_2);
			long size = snand_init();
			const char *got = (_SPI_NAND_GET_DEVICE_INFO_PTR)->ptr_name;
			if (size != (long)e->device_size || !got || strcmp(got, e->ptr_name)) {
				int first = -1;
				for (int j = 0; j < i && first < 0; j++)
					if (spi_nand_compare(e, &spi_nand_flash_tables[j]) == SPI_NAND_FLASH_RTN_NO_ERROR)
						first = j;
				if (first >= 0 && (!strcmp(e->ptr_name, "ZENTEL A5U12A21ASC") || !strcmp(e->ptr_name, "ZENTEL A5U1GA21BWS") ||
						   !strcmp(e->ptr_name, "MIRA PSU1GS20BN"))) {
					shadowed++;
					continue;
				}
				printf("     %s: detected as %s, size %ld\n", e->ptr_name, got ? got : "nothing", size);
				bad++;
			}
		}
		printf("     %d SPI NAND chips in the table\n", n);
		check(!bad && shadowed == 3, "every SPI NAND chip of the table is detected by its ID");
	}

	chip(0xef, 0xaa, 0x21);
	snand_init();
	fill(buf, 3, 1);
	check(snand_write(buf, 10L * PAGE, 3 * PAGE) == 3 * PAGE, "write 3 pages");
	check(snand_read(got, 10L * PAGE, 3 * PAGE) == 3 * PAGE && !memcmp(got, buf, 3 * PAGE), "read them back");

	ecc_bad[0] = 11;
	check(snand_read(got, 10L * PAGE, 3 * PAGE) < 0, "read over an ECC-failed page fails without -k");
	fill(buf, 4, 2);
	memcpy(page(12), buf + PAGE, PAGE); memcpy(page(13), buf + 2 * PAGE, PAGE); memcpy(page(10), buf, PAGE);
	Skip_BAD_page = 1;
	check(snand_read(got, 10L * PAGE, 3 * PAGE) == 3 * PAGE && !memcmp(got, buf, 3 * PAGE),
	      "-k read skips the ECC-failed page: pages 10, 12, 13");

	chip(0xef, 0xaa, 0x21);
	snand_init();
	prog_fail[0] = 21;
	fill(buf, 3, 3);
	check(snand_write(buf, 20L * PAGE, 3 * PAGE) < 0, "write over a program-failed page fails without -k");
	Skip_BAD_page = 1;
	for (long p = 20; p < 24; p++) memset(page(p), 0xff, PAGE + OOB);
	check(snand_write(buf, 20L * PAGE, 3 * PAGE) == 3 * PAGE, "-k write skips the program-failed page");
	check(!memcmp(page(20), buf, PAGE) && !memcmp(page(22), buf + PAGE, PAGE) && !memcmp(page(23), buf + 2 * PAGE, PAGE),
	      "-k write puts the data to pages 20, 22, 23");

	chip(0xef, 0xaa, 0x21);
	snand_init();
	Skip_BAD_page = 1;
	prog_fail[0] = PAGES - 2;
	fill(buf, 2, 4);
	check(snand_write(buf, (PAGES - 2L) * PAGE, 2 * PAGE) < 0, "-k write that needs a page past the chip end fails");
	check(page(0)[0] == 0xff && page(0)[PAGE - 1] == 0xff, "-k write does not wrap to page 0");

	chip(0xef, 0xaa, 0x21);
	snand_init();
	Skip_BAD_page = 1;
	ecc_bad[0] = PAGES - 2;
	check(snand_read(got, (PAGES - 2L) * PAGE, 2 * PAGE) < 0, "-k read that needs a page past the chip end fails");

	chip(0xef, 0xaa, 0x21);
	snand_init();
	ecc_bad[0] = 30;
	fill(buf, 1, 5);
	check(snand_write(buf, 30L * PAGE, PAGE) == PAGE && !memcmp(page(30), buf, PAGE),
	      "without -k an ECC error in the read before a write does not fail the write");

	/* the last chunk is a part of a page: after a skip it goes to the next page */
	for (int kind = 0; kind < 2; kind++) {
		chip(0xef, 0xaa, 0x21);
		snand_init();
		Skip_BAD_page = 1;
		if (kind) prog_fail[0] = 41; else ecc_bad[0] = 41;
		fill(buf, 2, 6 + kind);
		check(snand_write(buf, 40L * PAGE, PAGE + PAGE / 2) == PAGE + PAGE / 2, kind ?
		      "-k write, partial last chunk on a program-failed page" : "-k write, partial last chunk on an ECC-failed page");
		check(!memcmp(page(40), buf, PAGE) && !memcmp(page(42), buf + PAGE, PAGE / 2) && page(42)[PAGE / 2] == 0xff,
		      "  the partial chunk lands at the start of page 42");
		check(snand_read(got, 40L * PAGE, PAGE + PAGE / 2) == PAGE + PAGE / 2 && !memcmp(got, buf, PAGE + PAGE / 2),
		      "  a -k read in the same run (verify) gives the data back");
	}

	/* a -k write stays inside -a/-l: the next partition is not touched */
	chip(0xef, 0xaa, 0x21);
	snand_init();
	Skip_BAD_page = 1;
	prog_fail[0] = 61;
	fill(buf, 4, 8);
	Skip_BAD_end = 64L * PAGE;		/* -a 60 pages -l 4 pages */
	check(snand_write(buf, 60L * PAGE, 4 * PAGE) < 0, "-k write that needs a page past -a/-l fails");
	check(page(64)[0] == 0xff && page(64)[PAGE - 1] == 0xff, "  the page after -a/-l is not written");
	Skip_BAD_end = 65L * PAGE;		/* -l one page longer than the data */
	for (long p = 60; p < 65; p++) memset(page(p), 0xff, PAGE + OOB);
	check(snand_write(buf, 60L * PAGE, 4 * PAGE) == 4 * PAGE && !memcmp(page(64), buf + 3 * PAGE, PAGE),
	      "-k write with room left in -a/-l uses it");
	Skip_BAD_end = 0;

	/* two planes: the plane bit is column bit 12 on a 2 KB page, bit 13 on a 4 KB page */
	chip(0xc2, 0x35, 0x03);			/* MX35LF4G24AD: 4 KB page, two planes */
	snand_init();
	snand_read(got, 0, 16);
	check(col_hi_read == 0x00, "4 KB page, plane 0: no plane bit in the column");
	snand_read(got, 64L * 4096, 16);
	check(col_hi_read == 0x20, "4 KB page, plane 1: plane bit is column bit 13");
	fill(buf, 1, 12);
	snand_write(buf, 64L * 4096, 16);
	check(col_hi_load == 0x20, "  and the same on program load");
	chip(0xc2, 0x22, 0x00);			/* MX35LF2GE4AB: 2 KB page, two planes */
	snand_init();
	snand_read(got, 64L * 2048, 16);
	check(col_hi_read == 0x10, "2 KB page, plane 1: plane bit is column bit 12");

	/* a page with an ECC error is not served from the driver cache on the next read */
	chip(0xef, 0xaa, 0x21);
	snand_init();
	ecc_bad[0] = 50;
	check(snand_read(got, 50L * PAGE, PAGE) < 0 && snand_read(got, 50L * PAGE, PAGE) < 0,
	      "an ECC-failed page fails again on the second read");

	/* pages of all 0xFF are not programmed: nothing would change on the chip */
	chip(0xef, 0xaa, 0x21);
	snand_init();
	memset(buf, 0xff, 4 * PAGE);
	buf[2 * PAGE + 7] = 0x12;
	check(snand_write(buf, 70L * PAGE, 4 * PAGE) == 4 * PAGE && nprog == 1, "a write of 4 pages, 3 of them 0xFF, programs one page");
	check(page(72)[7] == 0x12 && page(72)[8] == 0xff && page(70)[0] == 0xff, "  the one page with data is written");
	nprog = 0;
	memset(buf, 0xff, PAGE);
	check(snand_write(buf, 80L * PAGE + 100, 500) == 500 && nprog == 0, "a partial write of 0xFF bytes programs nothing");
	check(snand_read(got, 70L * PAGE, 4 * PAGE) == 4 * PAGE && got[2 * PAGE + 7] == 0x12, "  the pages read back");

	/* with -k a 0xFF page over an ECC-failed page is skipped like any other */
	chip(0xef, 0xaa, 0x21);
	snand_init();
	Skip_BAD_page = 1;
	ecc_bad[0] = 91;
	fill(buf, 3, 10);
	memset(buf + PAGE, 0xff, PAGE);
	check(snand_write(buf, 90L * PAGE, 3 * PAGE) == 3 * PAGE && !memcmp(page(93), buf + 2 * PAGE, PAGE),
	      "-k write: a 0xFF page over an ECC-failed page goes to the next page");
	check(snand_read(got, 90L * PAGE, 3 * PAGE) == 3 * PAGE && !memcmp(got, buf, 3 * PAGE), "  and reads back in place");

	/* a block that fails to erase fails the erase, wherever it is in the range */
	chip(0xef, 0xaa, 0x21);
	snand_init();
	check(snand_erase(0, 4 * 128 * 1024) == 0, "erase of 4 blocks");
	erase_fail[0] = 1;
	memset(page(3 * 64), 0x00, PAGE);
	check(snand_erase(0, 4 * 128 * 1024) != 0, "erase fails when block 1 of 4 does not erase");
	check(page(3 * 64)[0] == 0xff, "  the blocks after it are still erased");
	erase_fail[0] = 3;
	check(snand_erase(0, 4 * 128 * 1024) != 0, "erase fails when the last block does not erase");

	/* a chip that stays busy (OIP) makes the operation fail instead of hanging */
	chip(0xef, 0xaa, 0x21);
	snand_init();
	stuck_busy = 1;
	check(snand_read(got, 0, PAGE) < 0, "read fails when the chip stays busy");
	fill(buf, 1, 9);
	check(snand_write(buf, 0, PAGE) < 0, "write fails when the chip stays busy");
	{
		u32 written = 12345;
		stuck_busy = 0;
		memset(page(0), 0xff, 2 * (PAGE + OOB));
		/* the first page goes through, then the chip hangs on the second */
		nprog = 0;
		busy_after = 1;
		fill(buf, 2, 11);
		check(SPI_NAND_Flash_Write_Nbyte(0, 2 * PAGE, &written, buf, SPI_NAND_FLASH_WRITE_SPEED_MODE_SINGLE) == SPI_NAND_FLASH_RTN_TIMEOUT &&
		      written == PAGE, "  a write that times out reports only the bytes written");
		busy_after = -1;
		stuck_busy = 1;
	}
	check(snand_erase(0, 128 * 1024) != 0, "erase fails when the chip stays busy");
	check(!(feat[0xc0] & 0x02), "  the write enable latch is cleared after the timeouts");
	stuck_busy = 0;
	check(snand_read(got, 0, PAGE) == PAGE, "  works again when the chip is ready");

	if (!fail) printf("all nandsim checks passed\n");
	return fail;
}

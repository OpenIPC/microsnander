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
#include "../src/spi_nand_flash.h"
#include "../src/nandcmd_api.h"

int snand_read(unsigned char *buf, unsigned long from, unsigned long len);
int snand_write(unsigned char *buf, unsigned long to, unsigned long len);
long snand_init(void);

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
static long ecc_bad[MAXBAD], prog_fail[MAXBAD];

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
		case 0x0f: d[i] = feat[cmd[1]]; break;
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
	case 0x1f: feat[cmd[1]] = cmd[2]; break;
	case 0x06: feat[0xc0] |= 0x02; break;
	case 0x04: feat[0xc0] &= ~0x02; break;
	case 0x13:				/* page read: ECC result in status bits 5:4 */
		memcpy(cache, mem + row() * (PAGE + OOB), sizeof(cache));
		feat[0xc0] = (feat[0xc0] & ~0x30) | (in(ecc_bad, row()) ? 0x20 : 0);
		break;
	case 0x10:				/* program execute */
		feat[0xc0] &= ~0x08;
		if (in(prog_fail, row())) { feat[0xc0] |= 0x08; break; }
		p = mem + row() * (PAGE + OOB);
		for (unsigned int i = 0; i < sizeof(cache); i++) p[i] &= cache[i];
		break;
	case 0xd8:				/* block erase, 64 pages */
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
	for (int i = 0; i < MAXBAD; i++) ecc_bad[i] = prog_fail[i] = -1;
	Skip_BAD_page = 0;
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

	/* a page with an ECC error is not served from the driver cache on the next read */
	chip(0xef, 0xaa, 0x21);
	snand_init();
	ecc_bad[0] = 50;
	check(snand_read(got, 50L * PAGE, PAGE) < 0 && snand_read(got, 50L * PAGE, PAGE) < 0,
	      "an ECC-failed page fails again on the second read");

	if (!fail) printf("all nandsim checks passed\n");
	return fail;
}

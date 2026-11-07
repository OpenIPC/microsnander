/* tests/norsim.c — SPI NOR driver against a chip simulated at the SPI command
   level. First every chip of the table must be detected by its own JEDEC ID.
   Then three kinds of chips are written, read back and erased: an ordinary one
   (256-byte page program), an SST25VF-type one (0x02 programs a single byte,
   AAI word program, protected at power-up) and an SST26-type one (blocks
   locked until ULBPR, 8 KB blocks at the start). Two more: a chip without
   status registers 2 and 3 (they read 0xff, a write to them is ignored) and
   one with the quad enable bit set in its only status register, busy for a
   while after every erase and program. Chips over 16 MB: one that takes B7
   (4-byte address mode) only after a write enable, as Micron N25Q/MT25Q, one
   that does not need it, and a W25M512JV of two dies selected with C2; a
   chip left in 3-byte mode takes the first three address bytes, so data goes
   to the wrong place, as on a real one. The driver is built into this file,
   so the checks can walk its chip table. */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "../src/types.h"
#include "../src/spi_controller.h"
/* the driver's busy timeouts run on a clock that goes 100 ms ahead at every
   look, so a chip that never gets ready fails in a moment */
static int fast_clock(clockid_t id, struct timespec *t)
{
	static long long ms;
	(void)id;
	ms += 100;
	t->tv_sec = ms / 1000;
	t->tv_nsec = (ms % 1000) * 1000000L;
	return 0;
}
#define clock_gettime fast_clock
#include "../src/spi_nor_flash.c"
#undef clock_gettime
#include "../src/voltage.c"

unsigned int bsize;
const char *flash_chip_name;

enum { ORDINARY, SST25, SST26, NO_SR23, QE_SR1, QE_FIXED, NO_WRSR2, FOURB, FOURB_WREN, DIES };
static int kind;
static unsigned char id[5];
static unsigned char mem[64 << 20];
static const unsigned long die_size = 32 << 20;
static int four[2], die;		/* 4-byte address mode of each die; the selected die (DIES) */
static long stop_reads;			/* Ctrl+C after this many bytes read */
static long stop_pages;			/* Ctrl+C after this many pages programmed */
static int stop_chip_erase;		/* Ctrl+C during a chip erase */
static int die_reads = -1;
static int nprogs;			/* page programs (02) */
static unsigned char sfdp[256];		/* the SFDP table the chip answers 5A with */
static int nerase20;			/* 4 KB erases (20) */		/* the die of the last read of status register 3 */
static unsigned char cmd[300];
static int ncmd, nread;
static int wel, aai, locked;
static int busy;			/* status reads that still show WIP after WRSR or ULBPR */
static unsigned char sr;			/* BP bits */
static unsigned long aai_addr;
static int rdid_fail;			/* the read of the ID fails on USB */
static int spi_fail_at = -1;		/* the Nth AAI command fails on the wire */
static int naai;
static unsigned char sr2;		/* status register 2: CMP 0x40, QE 0x02, SRP1 0x01 */
static int sr2_writes, sr2_reads;
static int wp_low;			/* the WP# pin low: with SRP0 set, SR1 does not take a write */
static int busy_cmds;			/* commands other than RDSR sent while busy */

static int alen(void) { return four[die] ? 4 : 3; }
static unsigned long addr(void)
{
	unsigned long a = ((unsigned long)cmd[1] << 16) | (cmd[2] << 8) | cmd[3];

	return four[die] ? (a << 8) | cmd[4] : a;
}
/* where an address of the selected die is in mem */
static unsigned long at(unsigned long a) { return kind == DIES ? die * die_size + a % die_size : a % sizeof(mem); }
/* BP bits protect a part of the chip; with CMP set and no BP bits, all of it */
static int writable(void) { return wel && !(kind == SST26 ? locked : (sr & 0x3c) || (sr2 & 0x40)); }
static void program(unsigned long a, const unsigned char *d, int n) { for (int i = 0; i < n; i++) mem[at(a + i)] &= d[i]; }
static void erase(unsigned long a, unsigned long n) { a -= a % n; memset(mem + at(a), 0xff, n); }

SPI_CONTROLLER_RTN_T SPI_CONTROLLER_Enable_Manual_Mode(void) { return SPI_CONTROLLER_RTN_NO_ERROR; }
SPI_CONTROLLER_RTN_T SPI_CONTROLLER_Chip_Select_Low(void) { ncmd = nread = 0; return SPI_CONTROLLER_RTN_NO_ERROR; }
SPI_CONTROLLER_RTN_T SPI_CONTROLLER_Write_NByte(u8 *d, u32 len, SPI_CONTROLLER_SPEED_T speed)
{
	(void)speed;
	if (ncmd == 0 && len && d[0] == OPCODE_AAI && spi_fail_at >= 0 && naai++ == spi_fail_at) {
		ncmd = sizeof(cmd) + 1;		/* the command is lost */
		return SPI_CONTROLLER_RTN_DEF_NO;
	}
	for (u32 i = 0; i < len; i++)
		if (ncmd < (int)sizeof(cmd)) cmd[ncmd++] = d[i];
	return SPI_CONTROLLER_RTN_NO_ERROR;
}
SPI_CONTROLLER_RTN_T SPI_CONTROLLER_Write_One_Byte(u8 data) { return SPI_CONTROLLER_Write_NByte(&data, 1, 0); }
SPI_CONTROLLER_RTN_T SPI_CONTROLLER_Read_NByte(u8 *d, u32 len, SPI_CONTROLLER_SPEED_T speed)
{
	(void)speed;
	if (rdid_fail && cmd[0] == OPCODE_RDID) return SPI_CONTROLLER_RTN_DEF_NO;
	for (u32 i = 0; i < len; i++, nread++) {
		switch (cmd[0]) {
		case OPCODE_RDID: d[i] = nread < 5 ? id[nread] : 0; break;
		case OPCODE_RDSFDP: {		/* 3 address bytes, a dummy byte, then the table */
			unsigned long a = ((unsigned long)cmd[1] << 16) | (cmd[2] << 8) | cmd[3];
			d[i] = a + nread < sizeof(sfdp) ? sfdp[a + nread] : 0xff;
			break;
		}
		case OPCODE_RDSR: d[i] = sr | (wel ? SR_WEL : 0) | (aai ? 0x40 : 0) | (busy ? SR_WIP : 0); if (busy) busy--; break;
		case OPCODE_RDSR2: d[i] = kind == NO_SR23 || kind == QE_SR1 ? 0xff : sr2; sr2_reads++; break;
		case OPCODE_RDSR3: die_reads = die; d[i] = kind == NO_SR23 ? 0xff : kind == QE_SR1 ? 0x07 : 0; break;	/* another register on Macronix */
		case OPCODE_READ:
			d[i] = mem[at(addr() + nread)];
			if (stop_reads > 0 && !--stop_reads) ms_stop = 1;
			break;
		default: d[i] = 0;
		}
	}
	return SPI_CONTROLLER_RTN_NO_ERROR;
}
SPI_CONTROLLER_RTN_T SPI_CONTROLLER_Chip_Select_High(void)
{
	if (ncmd == 0 || ncmd > (int)sizeof(cmd)) {
		ncmd = 0;
		return SPI_CONTROLLER_RTN_NO_ERROR;
	}
	if (busy && cmd[0] != OPCODE_RDSR && cmd[0] != OPCODE_RDSR2 && cmd[0] != OPCODE_RDSR3)
		busy_cmds++;			/* only status reads are allowed while busy */
	switch (cmd[0]) {
	case OPCODE_WREN: if (!busy) wel = 1; break;		/* ignored while the chip is busy */
	case OPCODE_WRDI: wel = 0; aai = 0; break;
	case OPCODE_WRSR:
		if (wel && ncmd > 1 && !(wp_low && (sr & 0x80))) {
			sr = (cmd[1] & (kind == QE_SR1 ? 0xfc : 0xbc));
			if (ncmd > 2 && kind != NO_SR23)	/* the second byte goes to register 2 */
				sr2 = (cmd[2] & 0x43) | (kind == QE_FIXED ? 0x02 : 0);
			busy = 3;
		}
		wel = 0;
		break;
	case OPCODE_WRSR2:
		if (kind != NO_SR23 && kind != NO_WRSR2 && wel && ncmd > 1) {
			sr2 = (cmd[1] & 0x43) | (kind == QE_FIXED ? 0x02 : 0);	/* QE stays 1 on a QE-fixed part */
			sr2_writes++;
			busy = 3;
		}
		if (kind != NO_SR23 && kind != NO_WRSR2) wel = 0;	/* a chip without the register or the command ignores it */
		break;
	case OPCODE_ULBPR: if (wel && kind == SST26) { locked = 0; busy = 3; } wel = 0; break;
	case OPCODE_PP:
		if (writable() && ncmd > 1 + alen()) {
			program(addr(), cmd + 1 + alen(), kind == SST25 ? 1 : ncmd - 1 - alen());	/* SST25: one byte only */
			nprogs++;
			if (kind == QE_SR1) busy = 2;
			if (stop_pages > 0 && !--stop_pages) ms_stop = 1;
		}
		wel = 0;
		break;
	case OPCODE_AAI:
		if (kind != SST25) break;
		if (!aai) {
			if (!writable() || ncmd < 6) break;
			aai_addr = addr(); program(aai_addr, cmd + 4, 2); aai = 1;
		} else if (ncmd >= 3)
			program(aai_addr, cmd + 1, 2);
		aai_addr += 2;
		break;
	case OPCODE_P4E: if (writable()) { erase(addr(), 4096); nerase20++; } wel = 0; break;
	case OPCODE_SE:
		/* SST26: the first 32 KB are 8 KB blocks, so a 64 KB erase there takes one 8 KB block */
		if (writable()) {
			erase(addr(), kind == SST26 && addr() < 32768 ? 8192 : 65536);
			if (kind == QE_SR1) busy = 5;
		}
		wel = 0;
		break;
	case OPCODE_BE: case OPCODE_BE1:
		if (writable()) {
			if (kind == DIES) memset(mem + die * die_size, 0xff, die_size);	/* the selected die */
			else memset(mem, 0xff, sizeof(mem));
			if (kind == QE_SR1) busy = 20;
			if (stop_chip_erase) ms_stop = 1;
		}
		wel = 0;
		break;
	case 0xb7: case 0xe9:			/* enter, exit 4-byte address mode */
		if (kind >= FOURB && (kind != FOURB_WREN || wel))
			four[die] = cmd[0] == 0xb7;
		break;
	case 0xc5: wel = 0; break;		/* Winbond extended address register: 0 */
	case OPCODE_DIE_SELECT: if (kind == DIES && ncmd > 1) die = cmd[1] & 1; break;
	}
	ncmd = 0;
	return SPI_CONTROLLER_RTN_NO_ERROR;
}
SPI_CONTROLLER_RTN_T SPI_CONTROLLER_Xfer_NByte(u8 *in, u32 len_in, u8 *out, u32 len_out, SPI_CONTROLLER_SPEED_T speed)
{
	SPI_CONTROLLER_Chip_Select_Low();
	SPI_CONTROLLER_Write_NByte(in, len_in, speed);
	SPI_CONTROLLER_Read_NByte(out, len_out, speed);
	return SPI_CONTROLLER_Chip_Select_High();
}

static int fail;
static void check(int ok, const char *what) { printf("%s %s\n", ok ? "ok  " : "FAIL", what); if (!ok) fail = 1; }

/* an SFDP table: header, one parameter header (basic table, version 1.6, n words
   at 0x30), then the words: density, erase types (size 2^N and command) in words
   8 and 9, the 4-byte mode entry in the top byte of word 16 */
static int sfdp_leave = 1;		/* make_sfdp: the table lists E9 to leave the 4-byte mode */
static void make_sfdp(unsigned long size, int e1, int c1, int e2, int c2, int enter, int n)
{
	unsigned char *t = sfdp + 0x30;
	unsigned long bits = size * 8 - 1;

	memset(sfdp, 0xff, sizeof(sfdp));
	memcpy(sfdp, "SFDP", 4);
	sfdp[4] = 6; sfdp[5] = 1; sfdp[6] = 0; sfdp[7] = 0xff;
	sfdp[8] = 0x00; sfdp[9] = 6; sfdp[10] = 1; sfdp[11] = n; sfdp[12] = 0x30; sfdp[13] = 0; sfdp[14] = 0; sfdp[15] = 0xff;
	memset(t, 0, n * 4);
	t[0] = 0xe5; t[1] = 0x20; t[2] = size > 16 << 20 ? 0x02 : 0x00;	/* address bytes: 3 or 4 when large */
	t[4] = bits; t[5] = bits >> 8; t[6] = bits >> 16; t[7] = bits >> 24;
	t[28] = e1; t[29] = c1; t[30] = e2; t[31] = c2;
	if (n >= 16) { t[63] = enter; if (sfdp_leave) t[61] = 0x40; }	/* exit with E9: bit 14 of word 16 */
}

static const struct chip_info *find(const char *name)
{
	for (unsigned i = 0; i < sizeof(chips_data) / sizeof(chips_data[0]); i++)
		if (!strcmp(chips_data[i].name, name)) return &chips_data[i];
	return NULL;
}

static void chip(const struct chip_info *e, int k)
{
	id[0] = e->id;
	id[1] = e->jedec_id >> 24; id[2] = e->jedec_id >> 16; id[3] = e->jedec_id >> 8; id[4] = e->jedec_id;
	kind = k; wel = aai = busy = busy_cmds = sr2_writes = 0;
	four[0] = four[1] = die = 0; stop_reads = stop_pages = stop_chip_erase = 0; ms_stop = 0;
	sr2 = k == QE_FIXED ? 0x02 : 0;
	locked = k == SST26;
	sr = k == SST26 ? 0 : 0x1c;		/* BP bits set at power-up */
	if (k == QE_SR1) sr = 0x40;		/* quad enable, not protected */
}

static void write_read_erase(const char *name, int k)
{
	static unsigned char buf[5000], got[5000];
	const struct chip_info *e = find(name);
	char what[160];
	if (!e) { snprintf(what, sizeof(what), "%s is in the table", name); check(0, what); return; }
	chip(e, k);
	memset(mem, 0x00, e->sector_size * e->n_sectors);	/* not erased */
	long size = snor_init();
	snprintf(what, sizeof(what), "%s: detected, %ld KB", name, size >> 10);
	check(size == (long)(e->sector_size * e->n_sectors), what);

	snor_erase(0, size);				/* whole chip */
	snprintf(what, sizeof(what), "%s: chip erase", name);
	check(mem[0] == 0xff && mem[size - 1] == 0xff, what);

	memset(mem, 0x00, 65536);
	snor_erase(0, 65536);				/* sector erase of the first 64 KB */
	int erased = 1;
	for (int i = 0; i < 65536; i++) if (mem[i] != 0xff) { erased = 0; break; }
	snprintf(what, sizeof(what), "%s: the first 64 KB erase completely", name);
	check(erased, what);

	for (unsigned i = 0; i < sizeof(buf); i++) buf[i] = (unsigned char)(i * 7 + 3);
	sr2_reads = 0;
	int n = snor_write(buf, 0x1001, 4097);		/* odd start, odd length */
	int unprotects = sr2_reads;
	memset(got, 0, sizeof(got));
	snor_read(got, 0x1001, 4097);
	snprintf(what, sizeof(what), "%s: write at an odd address, odd length, read back", name);
	check(n == 4097 && !memcmp(got, buf, 4097) && mem[0x1000] == 0xff && mem[0x1001 + 4097] == 0xff, what);

	if (k != SST25 && k != SST26) {
		snprintf(what, sizeof(what), "%s: the write of 17 pages clears the protection once", name);
		if (kind != QE_SR1) check(unprotects == 1, what);	/* counted by the reads of register 2, Macronix has none */
	}
	snprintf(what, sizeof(what), "%s: nothing is sent while the chip is busy, the latch is clear at the end", name);
	check(!busy_cmds && !wel, what);
}

int main(void)
{
	const int n = sizeof(chips_data) / sizeof(chips_data[0]);
	int bad = 0, shadowed = 0;

	for (int i = 0; i < n; i++) {
		const struct chip_info *e = &chips_data[i];
		chip(e, ORDINARY);
		struct chip_info *got = chip_prob();
		if (got == e)
			continue;
		/* an older entry with the same maker and 2-byte device ID wins */
		if (got && got < e && got->id == e->id && (got->jedec_id >> 16) == (e->jedec_id >> 16)) {
			printf("     %s is detected as %s (same ID)\n", e->name, got->name);
			shadowed++;
			continue;
		}
		printf("     %s: detected as %s\n", e->name, got ? got->name : "nothing");
		bad++;
	}
	printf("     %d SPI NOR chips in the table\n", n);
	check(!bad && shadowed == 2, "every SPI NOR chip of the table is detected by its ID");

	/* the supply voltage by the chip name */
	{
		static const struct { const char *name; int v; } v[] = {
			{ "W25Q128FW", MS_VOLTAGE_18 }, { "W25Q128BV", MS_VOLTAGE_33 }, { "W25Q64JWIM", MS_VOLTAGE_18 },
			{ "W25Q64JVIM", MS_VOLTAGE_33 }, { "W25Q80BW", MS_VOLTAGE_18 }, { "W25Q80BL", MS_VOLTAGE_33 },
			{ "N25Q128A11", MS_VOLTAGE_18 }, { "N25Q128A13", MS_VOLTAGE_33 }, { "MT25QU256AB", MS_VOLTAGE_18 },
			{ "MT25QL512AB", MS_VOLTAGE_33 }, { "GD25LQ64E", MS_VOLTAGE_18 }, { "GD25WQ32E", MS_VOLTAGE_WIDE },
			{ "GD25Q64CSIG", MS_VOLTAGE_33 }, { "MX25U12835F", MS_VOLTAGE_18 }, { "MX25L12805D", MS_VOLTAGE_33 },
			{ "BY25Q64AL", MS_VOLTAGE_18 }, { "BY25Q64AS", MS_VOLTAGE_33 }, { "XM25QU256C", MS_VOLTAGE_18 },
			{ "XM25QW64C", MS_VOLTAGE_WIDE }, { "XM25QH128C", MS_VOLTAGE_33 }, { "IS25WP128F", MS_VOLTAGE_18 },
			{ "IS25LP256D", MS_VOLTAGE_33 }, { "FM25W128", MS_VOLTAGE_WIDE }, { "ZB25LQ128", MS_VOLTAGE_18 },
			{ "EN25S64", MS_VOLTAGE_18 }, { "EN25QH32", MS_VOLTAGE_33 }, { "IS25WQ040", MS_VOLTAGE_18 },
			{ "AT25DL161", MS_VOLTAGE_18 }, { "AT25DF021A", MS_VOLTAGE_WIDE }, { "AT25DF021", MS_VOLTAGE_33 },
			{ "SST25WF080", MS_VOLTAGE_18 }, { "MX25R6435F", MS_VOLTAGE_WIDE }, { "FM25M4AA", MS_VOLTAGE_18 },
			{ "DS25M4AB", MS_VOLTAGE_18 }, { "W25Q128JW-M", MS_VOLTAGE_18 }, { "W25Q128JV-M", MS_VOLTAGE_33 },
		};
		int wrong = 0, n18 = 0, nwide = 0;
		char w[320];

		for (unsigned i = 0; i < sizeof(v) / sizeof(v[0]); i++)
			if (!find(v[i].name) || ms_voltage(v[i].name) != v[i].v) {
				printf("     %s: %s\n", v[i].name, find(v[i].name) ? ms_voltage_name(ms_voltage(v[i].name)) : "not in the table");
				wrong++;
			}
		for (int i = 0; i < n; i++) {
			n18 += ms_voltage(chips_data[i].name) == MS_VOLTAGE_18;
			nwide += ms_voltage(chips_data[i].name) == MS_VOLTAGE_WIDE;
		}
		printf("     %d SPI NOR chips of 1.8 V, %d of 1.65-3.6 V\n", n18, nwide);
		check(!wrong, "the supply voltage of SPI NOR chips by their names");
		check(ms_voltage_warning("CH341A", "W25Q128FW", w, sizeof(w)) && strstr(w, "W25Q128FW is a 1.8 V chip") &&
		      strstr(w, "5 V"), "  a 1.8 V chip on the CH341A: both warnings");
		check(!ms_voltage_warning("CH347", "W25Q128BV", w, sizeof(w)) && !w[0], "  a 3.3 V chip on the CH347: none");
		check(ms_voltage_warning("EZP", "MX25U6435F", w, sizeof(w)) && !strstr(w, "5 V"), "  a 1.8 V chip on the EZP: no 5 V line");
	}

	write_read_erase("W25Q32BV", ORDINARY);
	write_read_erase("PCT25VF016B", SST25);
	write_read_erase("PCT26VF016", SST26);
	write_read_erase("XM25QH64A", NO_SR23);
	write_read_erase("MX25L3205D", QE_SR1);
	write_read_erase("W25Q128BV", QE_FIXED);
	check(sr2_writes == 0 && sr2 == 0x02, "W25Q128BV with QE fixed at 1: status register 2 is never written");

	/* CMP protects the whole chip: cleared, QE kept */
	{
		chip(find("W25Q32BV"), ORDINARY);
		snor_init();
		sr = 0; sr2 = 0x42;
		check(snor_unprotect() == 0 && sr2 == 0x02, "W25Q32BV: unprotect clears CMP and keeps QE");
		memset(mem, 0, 65536);
		check(snor_erase(0, 65536) == 0 && mem[0] == 0xff, "  and the chip erases");
	}

	/* -uu: QE too, where each kind of chip keeps it */
	{
		chip(find("W25Q32BV"), ORDINARY);
		snor_init();
		sr = 0x1c; sr2 = 0x42;
		check(snor_clear_qe() == 0 && sr == 0 && sr2 == 0 && !wel, "W25Q32BV: -uu clears the protection and QE");
		sr2_writes = 0;
		check(snor_clear_qe() == 0 && sr2_writes == 0, "  QE already clear: nothing is written");
		chip(find("W25Q128BV"), QE_FIXED);
		snor_init();
		sr = 0; sr2 = 0x02;
		check(snor_clear_qe() == -2 && sr2 == 0x02 && strstr(snor_unprotect_why, "QE stays") && !wel,
		      "W25Q128BV with QE fixed at 1: -uu says it stays");
		chip(find("W25Q32BV"), NO_WRSR2);
		snor_init();
		sr = 0; sr2 = 0x02;
		check(snor_clear_qe() == 0 && sr2 == 0 && sr == 0 && !wel, "a chip that does not know WRSR2: QE goes with the two-byte WRSR");
		sr = 0; sr2 = 0x41;
		check(snor_unprotect() == 0 && sr2 == 0 && !wel, "  CMP and SRP1 go with the two-byte WRSR too");
		sr = 0x1c; sr2 = 0x42;
		check(snor_unprotect() == 0 && sr == 0 && sr2 == 0x02 && !wel, "  and -u keeps QE that way");
		chip(find("W25Q128BV"), QE_FIXED);
		snor_init();
		sr = 0; sr2 = 0x42;
		check(snor_unprotect() == 0 && sr2 == 0x02, "  a chip that knows WRSR2 does not need it");
		chip(find("XM25QH64A"), NO_SR23);
		snor_init();
		sr = 0; sr2 = 0;
		check(snor_clear_qe() == 0 && !wel, "XM25QH64A without status register 2: nothing to clear, no write enable left");
		chip(find("MX25L3205D"), QE_SR1);
		snor_init();
		sr = 0x40; sr2 = 0x02;
		check(snor_clear_qe() == 0 && sr == 0 && sr2 == 0x02, "MX25L3205D: QE is in status register 1, register 2 is not touched");
		sr = 0x20;
		check(snor_unprotect() == 0 && sr == 0, "  BP3 in bit 5 is cleared");
		sr = 0x40;
		check(snor_unprotect() == 0 && sr == 0x40, "  -u keeps QE, and 0x07 from the 0x15 read is not WPS");
		sr = 0;
		chip(find("W25Q32BV"), ORDINARY);
		snor_init();
		sr = 0x80; sr2 = 0x02; wp_low = 1;
		check(snor_clear_qe() == -2 && sr2 == 0x02, "a protection that stays stops -uu before QE");
		wp_low = 0; sr = 0;
	}

	/* SRP0 alone, no BP: unprotect clears it too (it used to be left, and
	   "cleared" said); with WP# low the register stays, and that is told */
	{
		chip(find("W25Q32BV"), ORDINARY);
		snor_init();
		sr = 0x80; sr2 = 0x02;
		check(snor_unprotect() == 0 && sr == 0 && sr2 == 0x02, "W25Q32BV: unprotect clears SRP0 alone too, keeps QE");
		sr = 0x9c; wp_low = 1;
		check(snor_unprotect() != 0 && sr == 0x9c, "  with SRP0 and WP# low the register stays: unprotect fails, not 'cleared'");
		wp_low = 0;
		sr = 0x20;
		check(snor_unprotect() == 0, "  bit 5 alone (TB, or an error flag on some chips) does not fail the unprotect");
	}

	/* SRP0 with WP# low and no BP: the data is free, an erase is not
	   stopped by the register lock (only -u wants SRP0 cleared) */
	{
		chip(find("W25Q32BV"), ORDINARY);
		snor_init();
		sr = 0x80; sr2 = 0x02; wp_low = 1;
		memset(mem, 0, 65536);
		check(snor_erase(0, 65536) == 0 && mem[0] == 0xff && sr == 0x80, "  SRP0 with WP# low, no BP: an erase still goes, SRP0 left");
		check(snor_unprotect() == -2 && strstr(snor_unprotect_why, "WP#"), "  and -u says the register stays and why");
		wp_low = 0; sr = 0;
	}

	/* unprotect of a chip without status registers 2 and 3 */
	{
		chip(find("XM25QH64A"), NO_SR23);
		snor_init();
		time_t t0 = time(NULL);
		int ret = snor_unprotect();
		check(ret == 0 && !(sr & 0x1c) && !wel, "XM25QH64A: unprotect clears BP, leaves no write enable latch");
		memset(mem, 0, 65536);
		ret = snor_erase(0, 65536);
		check(ret == 0 && mem[0] == 0xff && time(NULL) - t0 < 5, "XM25QH64A: the next erase does not wait for a timeout");
	}

	/* a chip that stays busy: the erase reports a failure */
	{
		chip(find("W25Q32BV"), ORDINARY);
		snor_init();
		snor_unprotect();
		busy = 1 << 30;
		check(snor_erase(0, 65536) != 0, "W25Q32BV: a chip that never gets ready fails the erase");
		busy = 0;
	}

	/* an AAI command that fails on the wire stops the write with the bytes written so far */
	{
		static unsigned char buf[64];
		memset(buf, 0x11, sizeof(buf));
		chip(find("PCT25VF016B"), SST25);
		snor_init();
		snor_erase(0, 65536);
		naai = 0; spi_fail_at = 10;
		int n = snor_write(buf, 0, sizeof(buf));
		spi_fail_at = -1;
		check(n == 20, "PCT25VF016B: a failed AAI command stops the write after 10 words");
	}

	/* over 16 MB: the 4-byte address mode, with and without a write enable before B7 */
	{
		static const struct { const char *name; int k; } c4[] = { { "N25Q256A13", FOURB_WREN }, { "W25Q256FV", FOURB } };
		static unsigned char buf[5000], got[5000];
		char what[160];

		for (unsigned c = 0; c < sizeof(c4) / sizeof(c4[0]); c++) {
			const char *name = c4[c].name;

			chip(find(name), c4[c].k);
			check(snor_init() == 32 << 20, name);
			memset(mem, 0, 65536);
			memset(mem + (16 << 20), 0, 65536);
			snprintf(what, sizeof(what), "  %s: an erase at 16 MB erases there, not at 0", name);
			check(snor_erase(16 << 20, 65536) == 0 && mem[16 << 20] == 0xff && mem[(16 << 20) + 65535] == 0xff &&
			      mem[0] == 0, what);
			for (unsigned i = 0; i < sizeof(buf); i++) buf[i] = (unsigned char)(i * 5 + 1);
			int n = snor_write(buf, (16 << 20) + 0x1001, 4097);
			memset(got, 0, sizeof(got));
			snor_read(got, (16 << 20) + 0x1001, 4097);
			snprintf(what, sizeof(what), "  %s: written above 16 MB, read back, nothing at 0", name);
			check(n == 4097 && !memcmp(got, buf, 4097) && !memcmp(mem + (16 << 20) + 0x1001, buf, 4097) &&
			      mem[0x1001] == 0, what);
			snprintf(what, sizeof(what), "  %s: left in 3-byte mode, no write enable latch", name);
			check(!four[0] && !wel && !busy_cmds, what);
		}

		/* Ctrl+C: the operation stops between two steps, out of 4-byte mode */
		chip(find("N25Q256A13"), FOURB_WREN);
		snor_init();
		static unsigned char big[3 * 65536];
		stop_reads = 1000;
		check(snor_read(big, 16 << 20, sizeof(big)) < 0 && !four[0], "Ctrl+C stops a read, the chip is out of 4-byte mode");
		ms_stop = 1;
		check(snor_write(buf, 16 << 20, 4096) != 4096 && !four[0] && !wel, "  a write too, no write enable latch left");
		check(snor_erase(16 << 20, 65536) != 0 && !four[0], "  an erase too");
		ms_stop = 0;
		memset(mem + (16 << 20), 0xff, 4096);
		stop_pages = 3;
		check(snor_write(buf, 16 << 20, 4096) == 768 && !memcmp(mem + (16 << 20), buf, 768) &&
		      mem[(16 << 20) + 768] == 0xff && !four[0], "  a stopped write counts every page it sent");
		ms_stop = 0;
	}

	/* W25M512JV: two W25Q256JV dies, C2 selects one */
	{
		static unsigned char buf[0x1000], got[0x1000];
		const unsigned long b = 32 << 20;

		chip(find("W25M512JV"), DIES);
		check(snor_init() == 64 << 20, "W25M512JV: detected, 64 MB");
		memset(mem, 0, 0x1000);
		memset(mem + b - 0x1000, 0, 0x2000);
		check(snor_erase(b - 65536, 2 * 65536) == 0 && mem[b - 1] == 0xff && mem[b] == 0xff && mem[0] == 0,
		      "  an erase over the die boundary erases the end of die 0 and the start of die 1");
		for (unsigned i = 0; i < sizeof(buf); i++) buf[i] = (unsigned char)(i * 3 + 7);
		int n = snor_write(buf, b - 0x800, sizeof(buf));
		memset(got, 0, sizeof(got));
		int r = snor_read(got, b - 0x800, sizeof(got));
		check(n == (int)sizeof(buf) && r == (int)sizeof(got) && !memcmp(got, buf, sizeof(buf)) &&
		      !memcmp(mem + b - 0x800, buf, sizeof(buf)) && mem[0] == 0,
		      "  a write over the boundary goes to both dies, reads back, nothing at 0");
		check(die == 0 && !four[0] && !four[1] && !wel, "  die 0 selected at the end, both dies in 3-byte mode");
		mem[0] = mem[b] = 0;
		check(snor_erase(0, 64 << 20) == 0 && mem[0] == 0xff && mem[b] == 0xff && die == 0, "  a chip erase erases both dies");
		mem[0] = mem[b] = 0;
		stop_chip_erase = 1;
		check(snor_erase(0, 64 << 20) != 0 && mem[0] == 0xff && mem[b] == 0 && die == 0,
		      "  Ctrl+C during the erase of die 0: die 1 is not erased, the erase fails");
		stop_chip_erase = 0;
		ms_stop = 0;
		{
			unsigned char sr[3];
			int seen;

			die_reads = -1;
			check(snor_read_status_at(b + 4096, sr) == 0 && die == 0, "  the status of a block of die 1: read there, die 0 again after");
			seen = die_reads;
			check(seen == 1, "  the registers came from die 1");
		}
		check(snor_erase(64UL << 20, 0) != 0 && snor_erase(65536, (unsigned long)-65536) != 0 && die == 0,
		      "  an erase of nothing, or past the chip end with a wrapped length, fails");
		check(snor_unprotect() == 0 && die == 0, "  -u goes over both dies");
		ms_stop = 1;
		check(snor_unprotect() != 0 && die == 0, "  Ctrl+C: -u does not go on, die 0 again");
		ms_stop = 0;
		die = 1;
		check(snor_init() == 64 << 20 && die == 0, "  a die left selected by a stopped run: die 0 again at the start");
	}

	/* pages of all 0xFF are not programmed: nothing on the chip would change */
	{
		static unsigned char buf[4 * 256];

		chip(find("W25Q128BV"), ORDINARY);
		snor_init();
		memset(mem, 0xff, sizeof(buf) + 256);
		memset(buf, 0xff, sizeof(buf));
		buf[2 * 256 + 5] = 0x12;
		nprogs = 0;
		check(snor_write(buf, 0, sizeof(buf)) == (int)sizeof(buf) && nprogs == 1 && mem[2 * 256 + 5] == 0x12 &&
		      mem[2 * 256 + 4] == 0xff, "NOR: a write of 4 pages, 3 of them 0xFF, programs one page");
		nprogs = 0;
		memset(buf, 0xff, sizeof(buf));
		check(snor_write(buf, 100, 300) == 300 && nprogs == 0 && !wel, "  a write of 0xFF only programs nothing");
		buf[0] = 0x34;
		check(snor_write(buf, 1000, 2) == 2 && nprogs == 1 && mem[1000] == 0x34, "  a short write with data is programmed");
	}

	/* an ID read that fails on USB is no chip, and no ID is guessed from it */
	chip(find("W25Q128BV"), ORDINARY);
	rdid_fail = 1;
	check(snor_init() < 0, "an ID read that fails on USB: no chip detected");
	rdid_fail = 0;
	check(snor_init() == 16 << 20, "  the next read finds it");

	/* a chip that is not in the table: its SFDP table gives size, erase and 4-byte mode */
	{
		static const struct chip_info unknown = { "unknown", 0x9d, 0x60990000, 64 * 1024, 256, 0 };
		static unsigned char buf[4096], got[4096];

		chip(&unknown, ORDINARY);
		memset(sfdp, 0xff, sizeof(sfdp));
		check(snor_init() < 0, "SFDP: an unknown ID with no SFDP table is not detected");
		make_sfdp(16 << 20, 12, 0x20, 16, 0xd8, 0, 16);
		long size = snor_init();
		check(size == 16 << 20 && spi_chip_info->sector_size == 65536 && !spi_chip_info->addr4b &&
		      !strcmp(flash_chip_name, "SFDP-9D6099"), "SFDP: 16 MB, 64 KB sectors (D8), named by its ID");
		memset(mem, 0, 3 * 65536);
		check(snor_erase(65536, 65536) == 0 && mem[65536] == 0xff && mem[2 * 65536 - 1] == 0xff && mem[0] == 0 &&
		      mem[2 * 65536] == 0, "  a sector erase takes 64 KB with D8");
		make_sfdp(8 << 20, 12, 0x20, 0, 0, 0, 9);
		check(snor_init() == 8 << 20 && spi_chip_info->sector_size == 4096, "SFDP: only 4 KB erase (20): 4 KB sectors");
		memset(mem, 0, 3 * 4096);
		nerase20 = 0;
		check(snor_erase(4096, 4096) == 0 && mem[4096] == 0xff && mem[8191] == 0xff && mem[4095] == 0 && mem[8192] == 0 && nerase20 == 1,
		      "  erased with 20");
		chip(&unknown, FOURB_WREN);
		make_sfdp(32 << 20, 12, 0x20, 16, 0xd8, 0x02, 16);	/* write enable, then B7 */
		check(snor_init() == 32 << 20 && spi_chip_info->addr4b, "SFDP: 32 MB with B7 after a write enable: 4-byte addresses");
		for (unsigned i = 0; i < sizeof(buf); i++) buf[i] = (unsigned char)(i * 7 + 3);
		memset(mem, 0, 4096);
		snor_erase((16 << 20) + 65536, 65536);
		check(snor_write(buf, (16 << 20) + 65536, sizeof(buf)) == (int)sizeof(buf) && snor_read(got, (16 << 20) + 65536, sizeof(got)) == (int)sizeof(got) &&
		      !memcmp(got, buf, sizeof(buf)) && mem[0] == 0 && !four[0], "  written and read above 16 MB, nothing at 0, back in 3-byte mode");
		sfdp_leave = 0;
		make_sfdp(32 << 20, 12, 0x20, 16, 0xd8, 0x01, 16);	/* B7, but no E9 to leave */
		check(snor_init() == 16 << 20 && !spi_chip_info->addr4b, "SFDP: 32 MB with B7 but no E9: the first 16 MB only");
		sfdp_leave = 1;
		make_sfdp(32 << 20, 12, 0x20, 16, 0xd8, 0x01, 16);
		sfdp[0x30 + 2] = 0x04;					/* 4-byte addresses only */
		check(snor_init() < 0, "SFDP: a chip of 4-byte addresses only is not detected");
		make_sfdp(32 << 20, 12, 0x20, 16, 0xd8, 0, 9);		/* an old table: no word 16 */
		check(snor_init() == 16 << 20 && !spi_chip_info->addr4b, "SFDP: 32 MB without the 4-byte mode entry: the first 16 MB only");
		make_sfdp(16 << 20, 0, 0, 0, 0, 0, 9);
		check(snor_init() < 0, "SFDP: no erase type: not detected");
		make_sfdp(16 << 20, 12, 0x20, 16, 0xd8, 0, 16);
		sfdp[0] = 'X';
		check(snor_init() < 0, "SFDP: a wrong signature: not detected");
		make_sfdp(16 << 20, 12, 0x20, 16, 0xd8, 0, 16);
		check(snor_init() == 16 << 20 && snor_init() > 0, "  and a known chip next is found by the table again");
		chip(find("W25Q128BV"), ORDINARY);
		check(snor_init() == 16 << 20 && !strcmp(flash_chip_name, "W25Q128BV"), "  W25Q128BV from the table, not from SFDP");
		memset(sfdp, 0xff, sizeof(sfdp));
	}

	if (!fail) printf("all norsim checks passed\n");
	return fail;
}

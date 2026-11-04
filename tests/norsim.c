/* tests/norsim.c — SPI NOR driver against a chip simulated at the SPI command
   level. First every chip of the table must be detected by its own JEDEC ID.
   Then three kinds of chips are written, read back and erased: an ordinary one
   (256-byte page program), an SST25VF-type one (0x02 programs a single byte,
   AAI word program, protected at power-up) and an SST26-type one (blocks
   locked until ULBPR, 8 KB blocks at the start). Two more: a chip without
   status registers 2 and 3 (they read 0xff, a write to them is ignored) and
   one with the quad enable bit set in its only status register, busy for a
   while after every erase and program. The driver is built into
   this file, so the checks can walk its chip table. */
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

enum { ORDINARY, SST25, SST26, NO_SR23, QE_SR1, QE_FIXED, NO_WRSR2 };
static int kind;
static unsigned char id[5];
static unsigned char mem[16 << 20];
static unsigned char cmd[300];
static int ncmd, nread;
static int wel, aai, locked;
static int busy;			/* status reads that still show WIP after WRSR or ULBPR */
static unsigned char sr;			/* BP bits */
static unsigned long aai_addr;
static int spi_fail_at = -1;		/* the Nth AAI command fails on the wire */
static int naai;
static unsigned char sr2;		/* status register 2: CMP 0x40, QE 0x02, SRP1 0x01 */
static int sr2_writes, sr2_reads;
static int wp_low;			/* the WP# pin low: with SRP0 set, SR1 does not take a write */
static int busy_cmds;			/* commands other than RDSR sent while busy */

static unsigned long addr(void) { return ((unsigned long)cmd[1] << 16) | (cmd[2] << 8) | cmd[3]; }
/* BP bits protect a part of the chip; with CMP set and no BP bits, all of it */
static int writable(void) { return wel && !(kind == SST26 ? locked : (sr & 0x3c) || (sr2 & 0x40)); }
static void program(unsigned long a, const unsigned char *d, int n) { for (int i = 0; i < n; i++) mem[(a + i) % sizeof(mem)] &= d[i]; }
static void erase(unsigned long a, unsigned long n) { a -= a % n; memset(mem + a % sizeof(mem), 0xff, n); }

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
	for (u32 i = 0; i < len; i++, nread++) {
		switch (cmd[0]) {
		case OPCODE_RDID: d[i] = nread < 5 ? id[nread] : 0; break;
		case OPCODE_RDSR: d[i] = sr | (wel ? SR_WEL : 0) | (aai ? 0x40 : 0) | (busy ? SR_WIP : 0); if (busy) busy--; break;
		case OPCODE_RDSR2: d[i] = kind == NO_SR23 || kind == QE_SR1 ? 0xff : sr2; sr2_reads++; break;
		case OPCODE_RDSR3: d[i] = kind == NO_SR23 ? 0xff : kind == QE_SR1 ? 0x07 : 0; break;	/* another register on Macronix */
		case OPCODE_READ: d[i] = mem[(addr() + nread) % sizeof(mem)]; break;
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
		if (writable() && ncmd > 4) {
			program(addr(), cmd + 4, kind == SST25 ? 1 : ncmd - 4);	/* SST25: one byte only */
			if (kind == QE_SR1) busy = 2;
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
	case OPCODE_P4E: if (writable()) erase(addr(), 4096); wel = 0; break;
	case OPCODE_SE:
		/* SST26: the first 32 KB are 8 KB blocks, so a 64 KB erase there takes one 8 KB block */
		if (writable()) {
			erase(addr(), kind == SST26 && addr() < 32768 ? 8192 : 65536);
			if (kind == QE_SR1) busy = 5;
		}
		wel = 0;
		break;
	case OPCODE_BE: case OPCODE_BE1:
		if (writable()) { memset(mem, 0xff, sizeof(mem)); if (kind == QE_SR1) busy = 20; }
		wel = 0;
		break;
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
	sr2 = k == QE_FIXED ? 0x02 : 0;
	locked = k == SST26;
	sr = k == SST26 ? 0 : 0x1c;		/* BP bits set at power-up */
	if (k == QE_SR1) sr = 0x40;		/* quad enable, not protected */
	memset(mem, 0x00, sizeof(mem));		/* not erased */
}

static void write_read_erase(const char *name, int k)
{
	static unsigned char buf[5000], got[5000];
	const struct chip_info *e = find(name);
	char what[160];
	if (!e) { snprintf(what, sizeof(what), "%s is in the table", name); check(0, what); return; }
	chip(e, k);
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

	if (!fail) printf("all norsim checks passed\n");
	return fail;
}

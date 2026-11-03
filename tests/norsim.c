/* tests/norsim.c — SPI NOR driver against a chip simulated at the SPI command
   level. First every chip of the table must be detected by its own JEDEC ID.
   Then three kinds of chips are written, read back and erased: an ordinary one
   (256-byte page program), an SST25VF-type one (0x02 programs a single byte,
   AAI word program, protected at power-up) and an SST26-type one (blocks
   locked until ULBPR, 8 KB blocks at the start). The driver is built into
   this file, so the checks can walk its chip table. */
#include <stdio.h>
#include <string.h>
#include "../src/types.h"
#include "../src/spi_controller.h"
#include "../src/spi_nor_flash.c"

unsigned int bsize;

enum { ORDINARY, SST25, SST26 };
static int kind;
static unsigned char id[5];
static unsigned char mem[4 << 20];
static unsigned char cmd[300];
static int ncmd, nread;
static int wel, aai, locked;
static int busy;			/* status reads that still show WIP after WRSR or ULBPR */
static unsigned char sr;			/* BP bits */
static unsigned long aai_addr;
static int spi_fail_at = -1;		/* the Nth AAI command fails on the wire */
static int naai;

static unsigned long addr(void) { return ((unsigned long)cmd[1] << 16) | (cmd[2] << 8) | cmd[3]; }
static int writable(void) { return wel && !(kind == SST26 ? locked : (sr & 0x3c)); }
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
	switch (cmd[0]) {
	case OPCODE_WREN: if (!busy) wel = 1; break;		/* ignored while the chip is busy */
	case OPCODE_WRDI: wel = 0; aai = 0; break;
	case OPCODE_WRSR: if (wel && ncmd > 1) { sr = cmd[1] & 0xbc; busy = 3; } wel = 0; break;
	case OPCODE_ULBPR: if (wel && kind == SST26) { locked = 0; busy = 3; } wel = 0; break;
	case OPCODE_PP:
		if (writable() && ncmd > 4)
			program(addr(), cmd + 4, kind == SST25 ? 1 : ncmd - 4);	/* SST25: one byte only */
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
		if (writable()) erase(addr(), kind == SST26 && addr() < 32768 ? 8192 : 65536);
		wel = 0;
		break;
	case OPCODE_BE: case OPCODE_BE1: if (writable()) memset(mem, 0xff, sizeof(mem)); wel = 0; break;
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
	kind = k; wel = aai = busy = 0;
	locked = k == SST26;
	sr = k == SST25 ? 0x1c : (k == ORDINARY ? 0x1c : 0);	/* BP bits set at power-up */
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
	int n = snor_write(buf, 0x1001, 4097);		/* odd start, odd length */
	memset(got, 0, sizeof(got));
	snor_read(got, 0x1001, 4097);
	snprintf(what, sizeof(what), "%s: write at an odd address, odd length, read back", name);
	check(n == 4097 && !memcmp(got, buf, 4097) && mem[0x1000] == 0xff && mem[0x1001 + 4097] == 0xff, what);
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

	write_read_erase("W25Q32BV", ORDINARY);
	write_read_erase("PCT25VF016B", SST25);
	write_read_erase("PCT26VF016", SST26);

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

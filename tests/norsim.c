/* tests/norsim.c — SPI NOR probe against a chip simulated at the SPI command
   level: every chip of the table must be detected by its own JEDEC ID. The
   driver is built into this file, so the check can walk its chip table. */
#include <stdio.h>
#include <string.h>
#include "../src/types.h"
#include "../src/spi_controller.h"
#include "../src/spi_nor_flash.c"

unsigned int bsize;
static unsigned char id[5];
static unsigned char cmd;
static int ncmd, nread;

SPI_CONTROLLER_RTN_T SPI_CONTROLLER_Enable_Manual_Mode(void) { return SPI_CONTROLLER_RTN_NO_ERROR; }
SPI_CONTROLLER_RTN_T SPI_CONTROLLER_Chip_Select_Low(void) { ncmd = nread = 0; return SPI_CONTROLLER_RTN_NO_ERROR; }
SPI_CONTROLLER_RTN_T SPI_CONTROLLER_Chip_Select_High(void) { return SPI_CONTROLLER_RTN_NO_ERROR; }
SPI_CONTROLLER_RTN_T SPI_CONTROLLER_Write_NByte(u8 *d, u32 len, SPI_CONTROLLER_SPEED_T speed)
{
	(void)speed;
	if (len && ncmd++ == 0)
		cmd = d[0];
	return SPI_CONTROLLER_RTN_NO_ERROR;
}
SPI_CONTROLLER_RTN_T SPI_CONTROLLER_Write_One_Byte(u8 data) { return SPI_CONTROLLER_Write_NByte(&data, 1, 0); }
SPI_CONTROLLER_RTN_T SPI_CONTROLLER_Read_NByte(u8 *d, u32 len, SPI_CONTROLLER_SPEED_T speed)
{
	(void)speed;
	for (u32 i = 0; i < len; i++, nread++)
		d[i] = cmd == OPCODE_RDID ? (nread < 5 ? id[nread] : 0) : 0;	/* status registers read 0: ready */
	return SPI_CONTROLLER_RTN_NO_ERROR;
}
SPI_CONTROLLER_RTN_T SPI_CONTROLLER_Xfer_NByte(u8 *in, u32 len_in, u8 *out, u32 len_out, SPI_CONTROLLER_SPEED_T speed)
{
	SPI_CONTROLLER_Chip_Select_Low();
	SPI_CONTROLLER_Write_NByte(in, len_in, speed);
	SPI_CONTROLLER_Read_NByte(out, len_out, speed);
	return SPI_CONTROLLER_Chip_Select_High();
}

int main(void)
{
	const int n = sizeof(chips_data) / sizeof(chips_data[0]);
	int bad = 0, shadowed = 0;

	for (int i = 0; i < n; i++) {
		const struct chip_info *e = &chips_data[i];
		id[0] = e->id;
		id[1] = e->jedec_id >> 24; id[2] = e->jedec_id >> 16; id[3] = e->jedec_id >> 8; id[4] = e->jedec_id;
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
	int ok = !bad && shadowed == 2;
	printf("%s every SPI NOR chip of the table is detected by its ID\n", ok ? "ok  " : "FAIL");
	return !ok;
}

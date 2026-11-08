/* tests/spisim.c — src/spi_controller.c against a programmer that records
   its transfers: short writes while CS is low are held and go out with the
   data, the read or CS high that follows, in one transfer. */
#include <stdio.h>
#include <string.h>
#include "../src/spi_controller.c"

static int sends, cs_changes, fail_send;
static unsigned int last_w, last_r;
static unsigned char wire[8192];

static int rec_send(unsigned int w, unsigned int r, const unsigned char *wa, unsigned char *ra)
{
	sends++; last_w = w; last_r = r;
	memcpy(wire, wa, w);
	memset(ra, 0x5a, r);
	return fail_send ? -1 : 0;
}
static int fail_cs;
static int rec_cs(bool low) { cs_changes++; return !low && fail_cs ? -1 : 0; }

static const struct programmer rec = { .key = "rec", .name = "rec", .send = rec_send, .cs = rec_cs };
const struct programmer *programmer = &rec;

static int fail;
static void check(int ok, const char *what) { printf("%s %s\n", ok ? "ok  " : "FAIL", what); if (!ok) fail = 1; }

int main(void)
{
	static u8 page[256], big[6000];
	u8 v = 0;

	/* status read: opcode and read in one transfer */
	sends = 0;
	SPI_CONTROLLER_Chip_Select_Low();
	SPI_CONTROLLER_Write_One_Byte(0x05);
	SPI_CONTROLLER_Read_NByte(&v, 1, SPI_CONTROLLER_SPEED_SINGLE);
	SPI_CONTROLLER_Chip_Select_High();
	check(sends == 1 && last_w == 1 && last_r == 1 && wire[0] == 0x05 && v == 0x5a, "status read: the opcode goes with the read, one transfer");

	/* write enable: sent at CS high */
	sends = 0;
	SPI_CONTROLLER_Chip_Select_Low();
	SPI_CONTROLLER_Write_One_Byte(0x06);
	check(sends == 0, "a one-byte command is held while CS is low");
	SPI_CONTROLLER_Chip_Select_High();
	check(sends == 1 && last_w == 1 && wire[0] == 0x06, "  and goes out at CS high");

	/* page program: opcode, address and data in one transfer, its result returned */
	sends = 0;
	memset(page, 0xa5, sizeof(page));
	SPI_CONTROLLER_Chip_Select_Low();
	SPI_CONTROLLER_Write_One_Byte(0x02);
	SPI_CONTROLLER_Write_One_Byte(0x12);
	SPI_CONTROLLER_Write_One_Byte(0x34);
	SPI_CONTROLLER_Write_One_Byte(0x56);
	int ret = SPI_CONTROLLER_Write_NByte(page, sizeof(page), SPI_CONTROLLER_SPEED_SINGLE);
	SPI_CONTROLLER_Chip_Select_High();
	check(ret == 0 && sends == 1 && last_w == 260 && wire[0] == 0x02 && wire[3] == 0x56 && wire[4] == 0xa5,
	      "page program: opcode, address and 256 bytes in one transfer");
	fail_send = 1;
	SPI_CONTROLLER_Chip_Select_Low();
	SPI_CONTROLLER_Write_One_Byte(0x02);
	ret = SPI_CONTROLLER_Write_NByte(page, sizeof(page), SPI_CONTROLLER_SPEED_SINGLE);
	fail_send = 0;
	SPI_CONTROLLER_Chip_Select_High();
	check(ret != 0, "  a failed data transfer is the result of the data write");

	/* a held command that fails at CS high fails the next call (the status read) */
	fail_send = 1;
	SPI_CONTROLLER_Chip_Select_Low();
	SPI_CONTROLLER_Write_One_Byte(0x06);
	ret = SPI_CONTROLLER_Chip_Select_High();
	fail_send = 0;
	SPI_CONTROLLER_Chip_Select_Low();
	SPI_CONTROLLER_Write_One_Byte(0x05);
	int ret2 = SPI_CONTROLLER_Read_NByte(&v, 1, SPI_CONTROLLER_SPEED_SINGLE);
	SPI_CONTROLLER_Chip_Select_High();
	SPI_CONTROLLER_Chip_Select_Low();
	SPI_CONTROLLER_Write_One_Byte(0x05);
	int ret3 = SPI_CONTROLLER_Read_NByte(&v, 1, SPI_CONTROLLER_SPEED_SINGLE);
	SPI_CONTROLLER_Chip_Select_High();
	check(ret != 0 && ret2 != 0 && ret3 == 0, "a held command that fails at CS high fails the next read, once");

	/* CS high that fails (the CH347 sends the last frame of a write with it)
	   fails the next call, once */
	SPI_CONTROLLER_Chip_Select_Low();
	SPI_CONTROLLER_Write_One_Byte(0x02);
	SPI_CONTROLLER_Write_NByte(page, sizeof(page), SPI_CONTROLLER_SPEED_SINGLE);
	fail_cs = 1;
	ret = SPI_CONTROLLER_Chip_Select_High();
	fail_cs = 0;
	SPI_CONTROLLER_Chip_Select_Low();
	SPI_CONTROLLER_Write_One_Byte(0x05);
	ret2 = SPI_CONTROLLER_Read_NByte(&v, 1, SPI_CONTROLLER_SPEED_SINGLE);
	SPI_CONTROLLER_Chip_Select_High();
	SPI_CONTROLLER_Chip_Select_Low();
	SPI_CONTROLLER_Write_One_Byte(0x05);
	ret3 = SPI_CONTROLLER_Read_NByte(&v, 1, SPI_CONTROLLER_SPEED_SINGLE);
	SPI_CONTROLLER_Chip_Select_High();
	check(ret != 0 && ret2 != 0 && ret3 == 0, "a page program whose CS high fails fails the next read, once");

	/* a short data write (a last page of 10 bytes) goes out at once: its result is the call's */
	fail_send = 1;
	SPI_CONTROLLER_Chip_Select_Low();
	SPI_CONTROLLER_Write_One_Byte(0x02);
	ret = SPI_CONTROLLER_Write_NByte(page, 10, SPI_CONTROLLER_SPEED_SINGLE);
	fail_send = 0;
	SPI_CONTROLLER_Chip_Select_High();
	spi_held_failed();
	check(ret != 0, "a failed 10-byte data write fails the call");

	/* more than the buffer: the held bytes first, then the data as it is */
	sends = 0;
	SPI_CONTROLLER_Chip_Select_Low();
	SPI_CONTROLLER_Write_One_Byte(0x02);
	ret = SPI_CONTROLLER_Write_NByte(big, sizeof(big), SPI_CONTROLLER_SPEED_SINGLE);
	SPI_CONTROLLER_Chip_Select_High();
	check(ret == 0 && sends == 2 && last_w == sizeof(big), "a write larger than the buffer: held bytes, then the data");

	/* CS low drops what a command without CS high left */
	sends = 0;
	SPI_CONTROLLER_Chip_Select_Low();
	SPI_CONTROLLER_Write_One_Byte(0x9f);
	SPI_CONTROLLER_Chip_Select_Low();
	SPI_CONTROLLER_Write_One_Byte(0x05);
	SPI_CONTROLLER_Read_NByte(&v, 1, SPI_CONTROLLER_SPEED_SINGLE);
	SPI_CONTROLLER_Chip_Select_High();
	check(sends == 1 && last_w == 1 && wire[0] == 0x05, "CS low starts a new command");

	if (!fail) printf("all spisim checks passed\n");
	return fail;
}

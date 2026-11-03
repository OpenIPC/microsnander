/*======================================================================================
 * MODULE NAME: spi
 * FILE NAME: spi_controller.c
 *
 * FUNCTIONS
 *
 *      SPI_CONTROLLER_Enable_Manual_Mode To provide interface for Enable SPI Controller Manual Mode.
 *      SPI_CONTROLLER_Write_One_Byte     To provide interface for write one byte to SPI bus.
 *      SPI_CONTROLLER_Write_NByte        To provide interface for write N bytes to SPI bus.
 *      SPI_CONTROLLER_Read_NByte         To provide interface for read N bytes from SPI bus.
 *      SPI_CONTROLLER_Chip_Select_Low    To provide interface for set chip select low in SPI bus.
 *      SPI_CONTROLLER_Chip_Select_High   To provide interface for set chip select high in SPI bus.
 *
 * DEPENDENCIES
 *
 * * $History: $
 * MODIFICTION HISTORY:
 *
 *======================================================================================
 */
#include <stddef.h>
#include <string.h>
#include "programmer.h"
#include "spi_controller.h"

/*
 * Short writes while CS is low (an opcode, an address, a status byte) are
 * kept and go out with what follows them, in one transfer: with the data of
 * a page program, with the read of a status register or of data, or at CS
 * high. Every transfer is a USB round trip, so a page program is one
 * transfer instead of five. Longer writes go out at once, so their result
 * is the result of the call.
 */
#define SPI_HOLD_MAX	16		/* longest write that is held back */
#define SPI_HOLD_BUF	(SPI_HOLD_MAX + 4096 + 256)	/* held bytes plus a page with its spare area */

static u8 held[SPI_HOLD_BUF];
static u32 nheld;

static int spi_flush(unsigned int readcnt, u8 *readarr)
{
	int ret;

	if (!nheld && !readcnt)
		return 0;
	ret = programmer->send(nheld, readcnt, held, readarr);
	nheld = 0;
	return ret;
}

SPI_CONTROLLER_RTN_T SPI_CONTROLLER_Enable_Manual_Mode( void )
{
	return 0;
}

SPI_CONTROLLER_RTN_T SPI_CONTROLLER_Write_NByte( u8 *ptr_data, u32 len, SPI_CONTROLLER_SPEED_T speed )
{
	(void)speed;
	if (nheld + len <= SPI_HOLD_BUF) {
		memcpy(held + nheld, ptr_data, len);
		nheld += len;
		if (len <= SPI_HOLD_MAX)
			return SPI_CONTROLLER_RTN_NO_ERROR;
		return (SPI_CONTROLLER_RTN_T)spi_flush(0, NULL);
	}
	if (spi_flush(0, NULL))
		return SPI_CONTROLLER_RTN_WRITE_DATAPFIFO_ERROR;
	return (SPI_CONTROLLER_RTN_T)programmer->send(len, 0, ptr_data, NULL);
}

SPI_CONTROLLER_RTN_T SPI_CONTROLLER_Write_One_Byte( u8  data )
{
	return SPI_CONTROLLER_Write_NByte(&data, 1, SPI_CONTROLLER_SPEED_SINGLE);
}

SPI_CONTROLLER_RTN_T SPI_CONTROLLER_Read_NByte( u8 *ptr_rtn_data, u32 len, SPI_CONTROLLER_SPEED_T speed )
{
	(void)speed;
	return (SPI_CONTROLLER_RTN_T)spi_flush(len, ptr_rtn_data);
}

/* CS low: whatever was held is from before, it was sent at CS high */
SPI_CONTROLLER_RTN_T SPI_CONTROLLER_Chip_Select_Low( void )
{
	nheld = 0;
	return (SPI_CONTROLLER_RTN_T)programmer->cs(true);
}

/* CS high ends the command: the held bytes go out first */
SPI_CONTROLLER_RTN_T SPI_CONTROLLER_Chip_Select_High( void )
{
	int ret = spi_flush(0, NULL);

	if (programmer->cs(false))
		ret = -1;
	return (SPI_CONTROLLER_RTN_T)ret;
}

#if 0
SPI_CONTROLLER_RTN_T SPI_CONTROLLER_Xfer_NByte( u8 *ptr_data_in, u32 len_in, u8 *ptr_data_out, u32 len_out, SPI_CONTROLLER_SPEED_T speed )
{
	return (SPI_CONTROLLER_RTN_T)programmer->send(len_out, len_in, ptr_data_out, ptr_data_in);
}
#endif
/* End of [spi_controller.c] package */

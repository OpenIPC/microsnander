/*
 * ch347_spi.h — SPI over the WCH CH347T/CH347F.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef __CH347_SPI_H__
#define __CH347_SPI_H__

#include <stdbool.h>

int ch347_spi_init(void);
int ch347_spi_shutdown(void);
int ch347_spi_send_command(unsigned int writecnt, unsigned int readcnt, const unsigned char *writearr, unsigned char *readarr);
int ch347_cs(bool low);
int ch347_set_speed(const char *speed);
const char *ch347_speed_list(void);

#endif /* __CH347_SPI_H__ */

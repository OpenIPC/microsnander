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

/* the I2C master of the CH347 (CH347T in mode 1, CH347F), for the SigmaStar
   ISP: one transaction of up to CH347_I2C_MAX bytes a packet */
#define CH347_I2C_MAX	60
int ch347_i2c_speed(unsigned int speed);
int ch347_i2c_write(unsigned char addr, const unsigned char *buf, unsigned int len);
int ch347_i2c_read(unsigned char addr, unsigned char *buf, unsigned int len);
const char *ch347_speed_list(void);

#endif /* __CH347_SPI_H__ */

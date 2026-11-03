/*
 * Copyright (C) 2018-2021 McMCC <mcmcc@mail.ru>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 */
#ifndef __CH341_SPI_H__
#define __CH341_SPI_H__

#include <string.h>
#include <stdio.h>
#include <stdbool.h>

#define min(a,b) (((a)<(b))?(a):(b))
#define max(a,b) (((a)>(b))?(a):(b))

int ch341a_spi_init(void);
int ch341a_spi_shutdown(void);
int ch341a_spi_send_command(unsigned int writecnt, unsigned int readcnt, const unsigned char *writearr, unsigned char *readarr);
int enable_pins(bool enable);
int ch341a_cs(bool low);	/* sent with the next stream */
int config_stream(unsigned int speed);

/* the I2C master of the CH341A (SDA, SCL), for the SigmaStar ISP; a USB
   packet of 32 bytes holds one transaction of up to CH341A_I2C_MAX bytes */
#define CH341A_I2C_MAX	24
int ch341a_i2c_speed(unsigned int speed);
int ch341a_i2c_write(unsigned char addr, const unsigned char *buf, unsigned int len);
int ch341a_i2c_read(unsigned char addr, unsigned char *buf, unsigned int len);

#endif /* __CH341_SPI_H__ */
/* End of [ch341a_spi.h] package */

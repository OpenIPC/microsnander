/*
 * Copyright (C) 2018-2021 McMCC <mcmcc@mail.ru>
 * snorcmd_api.h
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */
#ifndef __SNORCMD_API_H__
#define __SNORCMD_API_H__

int snor_read(unsigned char *buf, unsigned long from, unsigned long len);
int snor_erase(unsigned long offs, unsigned long len);
int snor_write(unsigned char *buf, unsigned long to, unsigned long len);
int snor_unprotect(void);
int snor_read_status(unsigned char sr[3]);
extern const char *flash_chip_name;	/* name of the chip found, for file names */
const char *snor_lookup(const unsigned char id[3], unsigned long *size, unsigned long *sector, int *addr4b);
long snor_init(void);
void support_snor_list(void);

#endif /* __SNORCMD_API_H__ */
/* End of [snorcmd_api.h] package */

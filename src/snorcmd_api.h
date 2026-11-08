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
unsigned long snor_erase_unit(void);	/* the smallest erase block: 4 KB, 32 KB or the sector */
int snor_write(unsigned char *buf, unsigned long to, unsigned long len);
int snor_block_locks(void);	/* 1: the chip locks its blocks at power-up (SST26), its own way */
int snor_unprotect(void);	/* 0; -1: the chip does not answer; -2: a register stays (snor_unprotect_why) */
int snor_clear_qe(void);	/* -uu: snor_unprotect(), then QE; the same results */
extern char snor_unprotect_why[200];
int snor_read_status(unsigned char sr[3]);
int snor_read_status_at(unsigned long addr, unsigned char sr[3]);	/* of the die that holds addr */
int snor_info(void);		/* -i: the read-only range and the unique ID; 0, -1 a read failed */
long snor_security_size(void);	/* -O: bytes of the security registers; 0: none known */
int snor_read_security(unsigned char *buf);	/* 0; -1 failed; -2 none known */
int snor_sr1_macronix(void);	/* 1: only SR1 is a status register, with the bits of Macronix */
extern const char *flash_chip_name;	/* name of the chip found, for file names */
const char *snor_lookup(const unsigned char id[3], unsigned long *size, unsigned long *sector, int *addr4b);
long snor_init(void);
void support_snor_list(void);

#endif /* __SNORCMD_API_H__ */
/* End of [snorcmd_api.h] package */

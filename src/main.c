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

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <getopt.h>
#include <time.h>
#include <stdio.h>

#include "flashcmd_api.h"
#include "programmer.h"
#include "ch347_spi.h"
#include "spi_nand_flash.h"

struct flash_cmd prog;
extern unsigned int bsize;

#ifdef EEPROM_SUPPORT
#include "ch341a_i2c.h"
#include "bitbang_microwire.h"
extern struct EEPROM eeprom_info;
extern char eepromname[12];
extern int eepromsize;
extern int mw_eepromsize;
extern int org;
#define EHELP	" -E             select I2C EEPROM {24c01|24c02|24c04|24c08|24c16|24c32|24c64|24c128|24c256|24c512|24c1024}\n" \
		"                select Microwire EEPROM {93c06|93c16|93c46|93c56|93c66|93c76|93c86|93c96} (need SPI-to-MW adapter)\n" \
		" -8             set organization 8-bit for Microwire EEPROM(default 16-bit) and set jumper on SPI-to-MW adapter\n" \
		" -f <addr len>  set manual address size in bits for Microwire EEPROM(default auto)\n"
#else
#define EHELP	""
#endif

#define _VER	"1.14.1"

void title(void)
{
#ifdef EEPROM_SUPPORT
	printf("\nMicroSNANDer - Serial Nor/nAND/Eeprom programmeR v." _VER "\n\n");
#else
	printf("\nMicroSNANDer - Spi Nor/nAND programmER v." _VER "\n\n");
#endif
}

/* Exit code: 0 for -h, 1 for an unknown option or no operation. */
void usage(int code)
{
	const char use[] =
		"  Usage:\n"\
		" -h             display this message\n"\
		" -d             disable internal ECC(use read and write page size + OOB size)\n"\
		" -I             ECC ignore errors(for read test only)\n"\
		" -k             skip BAD pages (SPI NAND): use the next page instead\n"\
		" -L             print list support chips\n"\
		" -i             read the chip ID info\n"\
		"" EHELP ""\
		" -e             erase chip(full or use with -a [-l])\n"\
		" -l <bytes>     manually set length\n"\
		" -a <address>   manually set address\n"\
		" -w <filename>  write chip with data from filename\n"\
		" -r <filename>  read chip and save data to filename\n"\
		" -u             unprotect chip\n"\
		" -v             verify after write on chip\n"\
		" -P <name>      programmer: ch341a, ch347, ezp, auto (default: the one connected)\n"\
		" -S <speed>     SPI clock of the CH347: 60M, 30M (default), 15M, 7.5M, 3.75M, 1.875M\n";
	printf(use);
	exit(code);
}

int main(int argc, char* argv[])
{
	int c, vr = 0, ret = 0, rc = 1, speed_set = 0;
	char *str, *fname = NULL, op = 0;
	unsigned char *buf;
	int long long len = 0, addr = 0, flen = 0, wlen = 0;
	FILE *fp;

	title();

#ifdef EEPROM_SUPPORT
	while ((c = getopt(argc, argv, "diIhveukLl:a:w:r:E:f:8P:S:")) != -1)
#else
	while ((c = getopt(argc, argv, "diIhveukLl:a:w:r:P:S:")) != -1)
#endif
	{
		switch(c)
		{
#ifdef EEPROM_SUPPORT
			case 'E':
				if ((eepromsize = parseEEPsize(optarg, &eeprom_info)) > 0) {
					memset(eepromname, 0, sizeof(eepromname));
					strncpy(eepromname, optarg, 10);
					if (len > eepromsize) {
						printf("Error set size %lld, max size %d for EEPROM %s!!!\n", len, eepromsize, eepromname);
						exit(1);
					}
				} else if ((mw_eepromsize = deviceSize_3wire(optarg)) > 0) {
					memset(eepromname, 0, sizeof(eepromname));
					strncpy(eepromname, optarg, 10);
					org = 1;
					if (len > mw_eepromsize) {
						printf("Error set size %lld, max size %d for EEPROM %s!!!\n", len, mw_eepromsize, eepromname);
						exit(1);
					}
				} else {
					printf("Unknown EEPROM chip %s!!!\n", optarg);
					exit(1);
				}
				break;
			case '8':
				if (mw_eepromsize <= 0)
				{
					printf("-8 option only for Microwire EEPROM chips!!!\n");
					exit(1);
				}
				org = 0;
				break;
			case 'f':
				if (mw_eepromsize <= 0)
				{
					printf("-f option only for Microwire EEPROM chips!!!\n");
					exit(1);
				}
				str = strdup(optarg);
				fix_addr_len = strtoll(str, NULL, *str && *(str + 1) == 'x' ? 16 : 10);
				if (fix_addr_len > 32) {
						printf("Address len is very big!!!\n");
						exit(1);
				}
				break;
#endif
			case 'I':
				ECC_ignore = 1;
				break;
			case 'k':
				Skip_BAD_page = 1;
				break;
			case 'P':
				if (programmer_select(optarg) < 0) {
					printf("Unknown programmer %s; use one of: %s.\n\n", optarg, programmer_names());
					exit(1);
				}
				break;
			case 'S':
				speed_set = 1;
				if (ch347_set_speed(optarg) < 0) {
					printf("Unknown SPI speed %s; use one of: %s.\n\n", optarg, ch347_speed_list());
					exit(1);
				}
				break;
			case 'd':
				ECC_fcheck = 0;
				_ondie_ecc_flag = 0;
				break;
			case 'l':
				str = strdup(optarg);
				len = strtoll(str, NULL, *str && *(str + 1) == 'x' ? 16 : 10);
				break;
			case 'a':
				str = strdup(optarg);
				addr = strtoll(str, NULL, *str && *(str + 1) == 'x' ? 16 : 10);
				break;
			case 'v':
				vr = 1;
				break;
			case 'i':
			case 'e':
			case 'u':
				if(!op)
					op = c;
				else
					op = 'x';
				break;
			case 'r':
			case 'w':
				if(!op) {
					op = c;
					fname = strdup(optarg);
				} else
					op = 'x';
				break;
			case 'L':
				support_flash_list();
				exit(0);
			case 'h':
				usage(0);
			default:
				usage(1);
		}
	}

	if (op == 0) usage(1);

	if (op == 'x' || (ECC_ignore && !ECC_fcheck) || (ECC_ignore && Skip_BAD_page) || (op == 'w' && ECC_ignore)) {
		printf("Conflicting options, only one option at a time.\n\n");
		return 1;
	}

	ret = programmer_init(speed_set);
	if (ret < 0) {
		if (ret == -1)
			printf("Programmer device not found!\n\n");
		return 1;
	}

#ifdef EEPROM_SUPPORT
	if ((eepromsize > 0 || mw_eepromsize > 0) && strcmp(programmer->key, "ch341a")) {
		printf("EEPROM chips are supported with the CH341A only.\n\n");
		goto out;
	}
#endif
	if((flen = flash_cmd_init(&prog)) <= 0)
		goto out;

#ifdef EEPROM_SUPPORT
	if ((eepromsize || mw_eepromsize) && op == 'i') {
		printf("Programmer not supported auto detect EEPROM!\n\n");
		goto out;
	}
#endif
	if (op == 'i') {
		rc = 0;
		goto out;
	}

	if (op == 'e') {
		printf("ERASE:\n");
		if(addr && !len)
			len = flen - addr;
		else if(!addr && !len) {
			len = flen;
			printf("Set full erase chip!\n");
		}
		if(len % bsize) {
			printf("Please set len = 0x%016llX multiple of the block size 0x%08X\n", len, bsize);
			goto out;
		}
		printf("Erase addr = 0x%016llX, len = 0x%016llX\n", addr, len);
		ret = prog.flash_erase(addr, len);
		if(!ret) {
			printf("Status: OK\n");
			rc = 0;
		} else
			printf("Status: BAD(%d)\n", ret);
		goto out;
	}

	if (op == 'u') {
		printf("UNLOCK:\n");
		if (!prog.flash_unprotect) {
			printf("Unprotect is not supported for this chip type or programmer (SPI NOR through CH341A or CH347 only).\n");
			goto out;
		}
		ret = prog.flash_unprotect();
		if(!ret) {
			printf("Status: OK\n");
			rc = 0;
		} else
			printf("Status: BAD(%d)\n", ret);
		goto out;
	}

	if(addr && !len)
		len = flen - addr;
	else if(!addr && !len)
		len = flen;
	/* -k moves data to later pages, but only inside -a/-l (without -l: to the chip end) */
	Skip_BAD_end = addr + len;
	buf = (unsigned char *)malloc(len + 1);
	if (!buf) {
		printf("Malloc failed for read buffer.\n");
		goto out;
	}

	if (op == 'w') {
		printf("WRITE:\n");
		fp = fopen(fname, "rb");
		if (!fp) {
			printf("Couldn't open file %s for reading.\n", fname);
			free(buf);
			goto out;
		}
		wlen = fread(buf, 1, len, fp);
		if (ferror(fp)) {
			printf("Error reading file [%s]\n", fname);
			fclose(fp);
			free(buf);
			goto out;
		}
		fclose(fp);
		if (wlen <= 0) {
			printf("File %s is empty.\n", fname);
			free(buf);
			goto out;
		}
		/* Only the bytes taken from the file are written and verified, never the
		   rest of the buffer. */
		if (wlen < len) {
			if (len != flen)
				printf("File %s is shorter than the length, writing 0x%llX bytes.\n", fname, wlen);
			len = wlen;
		}
		printf("Write addr = 0x%016llX, len = 0x%016llX\n", addr, len);
		ret = prog.flash_write(buf, addr, len);
		/* the drivers return the number of bytes written: less than len is a failure */
		if (ret != len) {
			if (ret < 0)
				printf("Status: BAD(%d)\n", ret);
			else
				printf("Status: BAD, wrote 0x%X of 0x%llX bytes\n", ret, len);
			free(buf);
			goto out;
		}
		printf("Status: OK\n");
		if (vr) {
			/* Read the range back into buf and compare it with the file, which
			   holds exactly the bytes written (its first len bytes): one buffer
			   as large as the image, the file in small chunks. */
			static unsigned char cmp[65536];
			long long off = 0;
			size_t n = 0, k = 0;

			printf("VERIFY:\n");
			fp = fopen(fname, "rb");
			if (!fp) {
				printf("Couldn't open file %s for reading.\n", fname);
				free(buf);
				goto out;
			}
			memset(buf, 0, len);
			printf("Read addr = 0x%016llX, len = 0x%016llX\n", addr, len);
			ret = prog.flash_read(buf, addr, len);
			if (ret != len) {
				printf("Status: BAD(%d)\n", ret);
			} else {
				while (off < len) {
					n = len - off < (long long)sizeof(cmp) ? (size_t)(len - off) : sizeof(cmp);
					if (fread(cmp, 1, n, fp) != n) {
						k = n;	/* the file changed: not a chip difference */
						break;
					}
					for (k = 0; k < n && cmp[k] == buf[off + k]; k++)
						;
					if (k < n)
						break;
					off += n;
				}
				if (off == len) {
					printf("Status: OK\n");
					rc = 0;
				} else if (k < n)
					printf("Status: BAD, first difference at 0x%016llX: wrote 0x%02X, read 0x%02X\n",
						addr + off + k, cmp[k], buf[off + k]);
				else
					printf("Error reading file [%s]\n", fname);
			}
			fclose(fp);
		} else
			rc = 0;
		free(buf);
		goto out;
	}

	if (op == 'r') {
		printf("READ:\n");
		printf("Read addr = 0x%016llX, len = 0x%016llX\n", addr, len);
		ret = prog.flash_read(buf, addr, len);
		if (ret != len) {
			printf("Status: BAD(%d)\n", ret);
			free(buf);
			goto out;
		}
		fp = fopen(fname, "wb");
		if (!fp) {
			printf("Couldn't open file %s for writing.\n", fname);
			free(buf);
			goto out;
		}
		/* fclose() flushes: its result counts as much as that of fwrite() */
		int werr = fwrite(buf, 1, len, fp) != (size_t)len;
		if (fclose(fp) != 0 || werr) {
			printf("Error writing file [%s]\n", fname);
			free(buf);
			goto out;
		}
		free(buf);
		printf("Status: OK\n");
		rc = 0;
	}

out:
	programmer_shutdown();
	return rc;
}

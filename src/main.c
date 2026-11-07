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
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include <signal.h>
#include <stdio.h>

#include "flashcmd_api.h"
#include "programmer.h"
#include "ch347_spi.h"
#include "sigmastar.h"
#include "selftest.h"
#include "web.h"
#include "timer.h"
#include "spi_nand_flash.h"

struct flash_cmd prog;
extern unsigned int bsize;

#define _VER	"1.27.0"

const char *ms_version = _VER;

/* a number in decimal or 0x hexadecimal, all of the argument; -1 if not */
static int parse_num(const char *arg, const char *opt, long long *val)
{
	char *end;
	int hex = arg[0] == '0' && (arg[1] == 'x' || arg[1] == 'X');

	errno = 0;
	*val = strtoll(arg, &end, hex ? 16 : 10);
	if (!*arg || *end || errno || *val < 0) {
		printf("%s: not a number: %s\n\n", opt, arg);
		return -1;
	}
	return 0;
}

void title(void)
{
	printf("\nMicroSNANDer - Spi Nor/nAND programmER v." _VER "\n\n");
}

/* Exit code: 0 for -h, 1 for an unknown option or no operation. */
void usage(int code)
{
	const char use[] =
		"  Usage:\n"\
		" -h             display this message\n"\
		" -d             disable internal ECC (use read and write page size + OOB size)\n"\
		" -I             ECC ignore errors (for read test only)\n"\
		" -k             skip BAD pages (SPI NAND): use the next page instead; blocks\n"\
		"                with a factory bad mark are skipped whole\n"\
		" -b             list the blocks with a factory bad mark (SPI NAND; -a, -l: a range);\n"\
		"                an erase leaves such blocks as they are\n"\
		" -L             print list support chips\n"\
		" -i             read the chip ID info\n"\
		" -e             erase chip (full or use with -a [-l])\n"\
		" -l <bytes>     manually set length\n"\
		" -a <address>   manually set address\n"\
		" -w <filename>  write chip with data from filename\n"\
		" -r <filename>  read chip and save data to filename\n"\
		" -u             unprotect chip; -uu: also clear QE (quad enable), a chip of a\n"\
		"                board that boots in quad mode may then not boot\n"\
		" -t             self-test: protection, then erase, write and read back one block\n"\
		"                (the first; -a for another) and put its data back\n"\
		" -v             verify after write on chip\n"\
		" -P <name>      programmer: ch341a, ch347, ezp, auto (default: the one connected)\n"\
		" -S <speed>     SPI clock of the CH347: 60M, 30M (default), 15M, 7.5M, 3.75M, 1.875M;\n"\
		"                with -m sigmastar the I2C clock: 20K, 100K (default), 400K, 750K\n"\
		" -m sigmastar   the flash of a SigmaStar board through the ISP of its SoC: the SoC's UART\n"\
		"                (TX to SDA, RX to SCL, GND) on the I2C of the CH341A or the CH347\n"\
		" -q             list the addresses that answer on the I2C of the programmer (alone, or\n"\
		"                before the operation: -m sigmastar -i -q)\n"\
		" -W [addr:]port web mode: a page with Detect, Read, Erase, Write, Registers\n"\
		" -K <password>  web mode: ask for this password (any user name)\n"\
		" -o <dir>       web mode: where Read saves the dumps (default: the current one)\n";
	printf(use);
	exit(code);
}

#define PART_MIN	(64 * 1024)

static long long part_size;		/* 0: the whole range in one piece */

/* the next part from at: up to the next multiple of part_size, so that all the
   parts but the first start on a block */
static long long part_len(long long at, long long left)
{
	long long k = part_size ? part_size - at % part_size : left;

	return k < left ? k : left;
}

/* In parts the drivers would each print a progress and a time of their own */
static void parts_begin(void)
{
	timer_start();
	timer_quiet = part_size != 0;
}

static time_t parts_shown;		/* when the progress line was printed; 0: none is open */

static void parts_progress(const char *what, long long done, long long len)
{
	time_t now = time(0);

	if (!part_size || now == parts_shown)
		return;
	parts_shown = now;
	printf("\b%s %lld%% [%lld] of [%lld] bytes      \r", what, 100 * done / len, done, len);
	fflush(stdout);
}

/* what: the last line, NULL after a failure, which has its own message */
static void parts_end(const char *what, long long done, long long len)
{
	if (part_size && what)
		printf("%s 100%% [%lld] of [%lld] bytes      \n", what, done, len);
	else if (parts_shown)
		printf("\n");			/* the failure gets a line of its own */
	parts_shown = 0;
	timer_quiet = 0;
	if (part_size)
		timer_end();
}

/* A dump goes to a new file next to fname and into place at the end: half a dump
   looks like a dump, and a read that fails leaves an earlier dump as it was.
   NULL: no such file here. */
static char *dump_temp(const char *fname, FILE **fp)
{
	char *tmp = malloc(strlen(fname) + 8);
	int fd;

	if (!tmp)
		return NULL;
	sprintf(tmp, "%s.XXXXXX", fname);
	if ((fd = mkstemp(tmp)) < 0) {
		free(tmp);
		return NULL;
	}
	/* it stays private (mkstemp: 0600) until it is the dump */
	if (!(*fp = fdopen(fd, "w+b"))) {
		close(fd);
		unlink(tmp);
		free(tmp);
		*fp = NULL;
		return NULL;
	}
	return tmp;
}

/* The finished dump into the earlier file: the same file, with its owner, its
   permissions and its other names. buf: a work area of n bytes. */
static int dump_copy(FILE *from, FILE *to, unsigned char *buf, size_t n)
{
	size_t got;

	rewind(from);
	if (ftruncate(fileno(to), 0))
		return -1;
	while ((got = fread(buf, 1, n, from)) > 0)
		if (fwrite(buf, 1, got, to) != got)
			return -1;
	return ferror(from) ? -1 : 0;
}

/* Read the range back and compare it with the file, which holds exactly the bytes
   written (its first len bytes), in small chunks: 0 when they are the same */
static int verify(unsigned char *buf, const char *fname, long long addr, long long len)
{
	static unsigned char cmp[65536];
	long long done, k, off;
	size_t n, i;
	int ret;
	FILE *fp = fopen(fname, "rb");

	if (!fp) {
		printf("Couldn't open file %s for reading.\n", fname);
		return 1;
	}
	printf("Read addr = 0x%016llX, len = 0x%016llX\n", addr, len);
	parts_begin();
	for (done = 0; done < len; done += k) {
		k = part_len(addr + done, len - done);
		memset(buf, 0, k);
		ret = ms_stop ? -1 : prog.flash_read(buf, addr + done, k);
		if (ms_stop)		/* Ctrl+C during the call: not a finished operation */
			ret = -1;
		if (ret != k) {
			parts_end(NULL, 0, 0);
			printf("Status: BAD(%d)\n", ret);
			fclose(fp);
			return 1;
		}
		for (off = 0; off < k; off += n) {
			n = k - off < (long long)sizeof(cmp) ? (size_t)(k - off) : sizeof(cmp);
			if (fread(cmp, 1, n, fp) != n) {	/* the file changed: not a chip difference */
				parts_end(NULL, 0, 0);
				printf("Error reading file [%s]\n", fname);
				fclose(fp);
				return 1;
			}
			for (i = 0; i < n; i++)
				if (cmp[i] != buf[off + i]) {
					parts_end(NULL, 0, 0);
					printf("Status: BAD, first difference at 0x%016llX: wrote 0x%02X, read 0x%02X\n",
					       addr + done + off + i, cmp[i], buf[off + i]);
					fclose(fp);
					return 1;
				}
		}
		parts_progress("Read", done + k, len);
	}
	parts_end("Read", len, len);
	printf("Status: OK\n");
	fclose(fp);
	return 0;
}

/* Ctrl+C during an operation: it stops at the next step, where the chip is
   as after a whole command (out of its 4-byte address mode, die 0 selected,
   chip select high); a second Ctrl+C quits at once */
static void on_stop(int sig)
{
	static const char m[] = "\nStopping after the current step; Ctrl+C again quits at once.\n";

	ms_stop = 1;
	signal(sig, SIG_DFL);
	if (write(STDOUT_FILENO, m, sizeof(m) - 1) < 0)
		return;
}

int main(int argc, char* argv[])
{
	int c, vr = 0, ret = 0, rc = 1, speed_set = 0;
	const char *speed = NULL;
	int scan = 0;
	char *fname = NULL, op = 0, *web = NULL, *web_key = NULL, *web_dir = NULL;
	unsigned char *buf;
	int long long len = 0, addr = 0, flen = 0, wlen = 0, done = 0, k = 0;
	int regular;
	struct stat st;
	int unlock = 0;		/* how many -u: 2 also clears QE */
	FILE *fp = NULL;

	title();

	while ((c = getopt(argc, argv, "diIhveutbkqLl:a:w:r:P:S:W:K:o:m:")) != -1)
	{
		switch(c)
		{
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
			case 'S':			/* SPI or I2C: known after -m */
				speed = optarg;
				break;
			case 'm':
				if (sigmastar_mode(optarg) < 0) {
					printf("Unknown mode %s; the one there is: sigmastar.\n\n", optarg);
					exit(1);
				}
				break;
			case 'd':
				ECC_fcheck = 0;
				_ondie_ecc_flag = 0;
				break;
			case 'l':
				if (parse_num(optarg, "-l", &len))
					exit(1);
				break;
			case 'a':
				if (parse_num(optarg, "-a", &addr))
					exit(1);
				break;
			case 'v':
				vr = 1;
				break;
			case 'W':
				web = optarg;
				break;
			case 'K':
				web_key = optarg;
				break;
			case 'o':
				web_dir = optarg;
				break;
			case 'q':			/* before the operation, or alone */
				scan = 1;
				break;
			case 'u':
				/* -u and -uu, nothing more and with no other operation */
				if (++unlock > 2 || (op && op != 'u'))
					op = 'x';
				else
					op = 'u';
				break;
			case 'i':
			case 'e':
			case 't':
			case 'b':
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

	if (speed) {
		speed_set = 1;
		if (sigmastar_on() ? sigmastar_set_speed(speed) < 0 : ch347_set_speed(speed) < 0) {
			if (sigmastar_on())
				printf("Unknown I2C speed %s; use one of: %s.\n\n", speed, sigmastar_speed_list());
			else
				printf("Unknown SPI speed %s; use one of: %s.\n\n", speed, ch347_speed_list());
			exit(1);
		}
	}

	if (web) {
		/* the web mode: the page does the operations, one programmer, auto */
		/* the page writes whole files from 0 with ECC on: no raw NAND
		   (-d), no ECC ignore (-I), no BAD page skipping (-k) */
		if (op || scan || vr || speed_set || !ECC_fcheck || ECC_ignore || Skip_BAD_page || sigmastar_on()) {
			printf("-W runs the web mode: give no -i, -r, -w, -e, -u, -t, -b, -q, -v, -S, -m, -d, -I or -k with it.\n\n");
			return 1;
		}
		return web_serve(web, web_key, web_dir, addr, len);
	}
	if (web_key || web_dir) {
		printf("-K and -o go with -W only.\n\n");
		return 1;
	}

	if (op == 0 && scan)
		op = 'q';			/* -q alone: only the bus */
	if (op == 0) usage(1);
	if (scan && op != 'q' && !sigmastar_on()) {	/* the bus stays in I2C after it */
		printf("-q goes alone, or before an operation with -m sigmastar.\n\n");
		return 1;
	}

	if (op == 'x' || (ECC_ignore && !ECC_fcheck) || (ECC_ignore && Skip_BAD_page) || (op == 'w' && ECC_ignore) ||
	    (op == 't' && (!ECC_fcheck || ECC_ignore || Skip_BAD_page))) {	/* -t: one block, ECC on, no skipping */
		printf("Conflicting options, only one option at a time.\n\n");
		return 1;
	}

	/* in the SigmaStar mode -S is the I2C clock, not the SPI one */
	ret = programmer_init(speed_set && !sigmastar_on());
	if (ret < 0) {
		if (ret == -1)
			printf("Programmer device not found!\n\n");
		return 1;
	}

	if (scan && i2c_scan(programmer))
		goto out;
	if (op == 'q') {
		rc = 0;
		goto out;
	}
	if (sigmastar_on()) {
		const struct programmer *isp = sigmastar_attach(programmer);

		if (!isp)
			goto out;
		programmer = isp;
	}

	if((flen = flash_cmd_init(&prog)) <= 0)
		goto out;
	/* the EZP runs a whole range as one stream in its own firmware: no step
	   to stop at, so Ctrl+C ends the run at once there, as before */
	if (prog.kind != FLASH_FIRMWARE) {
		signal(SIGINT, on_stop);
		signal(SIGTERM, on_stop);
	}
	if (addr >= flen || (len && len > flen - addr)) {
		printf("The range 0x%llX + 0x%llX is past the chip end (0x%llX).\n", addr, len, (long long)flen);
		goto out;
	}

	if (op == 'i') {
		rc = 0;
		goto out;
	}

	if (op == 't') {
		struct selftest t = {
			.cmd = &prog, .chip = flash_chip_name ? flash_chip_name : "chip", .programmer = programmer->name,
			.size = flen, .addr = addr, .block = bsize, .kind = prog.kind, .lock = prog.lock,
		};

		if (prog.kind == FLASH_NOR && programmer->send) {
			t.sr_valid = !snor_read_status_at(t.addr, t.sr);
			t.sr_macronix = t.sr_valid && snor_sr1_macronix();
			t.block_locks = snor_block_locks();
		}
		rc = selftest(&t);		/* 0 OK, 2 protected, 1 failed */
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
		if (!ret && ms_stop)
			ret = -1;
		if(!ret) {
			printf("Status: OK\n");
			rc = 0;
		} else
			printf("Status: BAD(%d)\n", ret);
		goto out;
	}

	if (op == 'b') {
		printf("BAD BLOCKS:\n");
		if (prog.kind != FLASH_NAND) {
			printf("Only a SPI NAND has bad blocks marked at the factory.\n");
			goto out;
		}
		if (addr && !len)
			len = flen - addr;
		else if (!addr && !len)
			len = flen;
		if (!snand_bad_blocks(addr, len)) {
			printf("Status: OK\n");
			rc = 0;
		} else
			printf("Status: BAD\n");
		goto out;
	}

	if (op == 'u') {
		printf("UNLOCK:\n");
		if (!prog.flash_unprotect) {
			printf("Unprotect is not supported for this chip type or programmer (SPI NOR through CH341A or CH347 only).\n");
			goto out;
		}
		ret = unlock > 1 ? prog.flash_clear_qe() : prog.flash_unprotect();
		if (!ret && ms_stop)
			ret = -1;
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

	if (op == 'w' && !(fp = fopen(fname, "rb"))) {
		printf("WRITE:\n");
		printf("Couldn't open file %s for reading.\n", fname);
		goto out;
	}
	/* size 0 says nothing: /proc and /sys have regular files of that size with data */
	regular = !fp || (!fstat(fileno(fp), &st) && S_ISREG(st.st_mode) && st.st_size > 0);
	/* A range goes in parts of whole blocks, so a board with little memory takes a
	   chip of any size. As one piece: -k (the pages after a skipped one move), a
	   raw dump (its pages are not whole blocks of data), the EZP (it checks the
	   whole range before it writes anything) and a file that is no regular file
	   (a pipe has one pass, and its length is known only at its end). */
	if (regular && !Skip_BAD_page && ECC_fcheck && prog.kind != FLASH_FIRMWARE && bsize) {
		part_size = bsize;
		while (part_size < PART_MIN)
			part_size += bsize;
	}
	buf = (unsigned char *)malloc((part_size ? part_size : len) + 1);
	if (!buf) {
		printf("Malloc failed for read buffer.\n");
		if (fp)
			fclose(fp);
		goto out;
	}

	if (op == 'w') {
		printf("WRITE:\n");
		if (regular)
			wlen = st.st_size < len ? st.st_size : len;
		else
			wlen = fread(buf, 1, len, fp);
		if (ferror(fp)) {
			printf("Error reading file [%s]\n", fname);
			fclose(fp);
			free(buf);
			goto out;
		}
		if (wlen <= 0) {
			printf("File %s is empty.\n", fname);
			fclose(fp);
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
		parts_begin();
		for (done = 0; done < len; done += k) {
			k = part_len(addr + done, len - done);
			if (regular) {
				size_t got = fread(buf, 1, k, fp);

				if (ferror(fp)) {
					parts_end(NULL, 0, 0);
					printf("Error reading file [%s]\n", fname);
					fclose(fp);
					free(buf);
					goto out;
				}
				/* the size of a file in /sys is only an upper limit */
				if ((long long)got < k) {
					parts_end(NULL, 0, 0);
					printf("File %s ends early, writing 0x%llX of 0x%llX bytes.\n", fname, done + (long long)got, len);
					parts_begin();
					k = got;
					len = done + k;
					if (!k)
						break;
				}
			}
			ret = ms_stop ? -1 : prog.flash_write(buf, addr + done, k);
			/* the drivers return the number of bytes written: less than k is a
			   failure, and so is a Ctrl+C during the call (with what was written) */
			if (ret != k || ms_stop) {
				parts_end(NULL, 0, 0);
				if (ret < 0)
					printf("Status: BAD(%d)\n", ret);
				else
					printf("Status: BAD, wrote 0x%llX of 0x%llX bytes\n", done + ret, len);
				fclose(fp);
				free(buf);
				goto out;
			}
			parts_progress("Written", done + k, len);
		}
		parts_end(len ? "Written" : NULL, len, len);
		fclose(fp);
		if (!len) {
			printf("File %s is empty.\n", fname);
			free(buf);
			goto out;
		}
		printf("Status: OK\n");
		if (vr) {
			printf("VERIFY:\n");
			rc = verify(buf, fname, addr, len);
		} else
			rc = 0;
		free(buf);
		goto out;
	}

	if (op == 'r') {
		char *tmp = NULL;
		FILE *old = NULL;
		int ok, kept = 0, have = !lstat(fname, &st);

		printf("READ:\n");
		/* a link, a FIFO or a device is written as it is */
		if (!have || S_ISREG(st.st_mode)) {
			int fd = have ? open(fname, O_WRONLY) : -1;	/* not truncated */

			if (have && (fd < 0 || !(old = fdopen(fd, "wb")))) {
				printf("Couldn't open file %s for writing.\n", fname);
				if (fd >= 0)
					close(fd);
				free(buf);
				goto out;
			}
			tmp = dump_temp(fname, &fp);
		}
		if (!tmp) {
			if (old)
				fclose(old);
			old = NULL;
			fp = fopen(fname, "wb");
		}
		if (!fp) {
			printf("Couldn't open file %s for writing.\n", fname);
			free(buf);
			goto out;
		}
		printf("Read addr = 0x%016llX, len = 0x%016llX\n", addr, len);
		parts_begin();
		for (done = 0; done < len; done += k) {
			k = part_len(addr + done, len - done);
			ret = ms_stop ? -1 : prog.flash_read(buf, addr + done, k);
			if (ms_stop)		/* Ctrl+C during the call: not a finished operation */
				ret = -1;
			if (ret != k) {
				parts_end(NULL, 0, 0);
				printf("Status: BAD(%d)\n", ret);
				break;
			}
			if (fwrite(buf, 1, k, fp) != (size_t)k) {
				parts_end(NULL, 0, 0);
				printf("Error writing file [%s]\n", fname);
				ret = -1;
				break;
			}
			parts_progress("Read", done + k, len);
		}
		if (done == len)
			parts_end("Read", len, len);
		ok = done == len;
		/* fflush and fclose: their results count as much as that of fwrite() */
		if (old) {
			if (ok && !fflush(fp)) {
				int bad = dump_copy(fp, old, buf, part_size ? part_size : len);

				if (fclose(old) != 0 || bad) {
					/* the earlier dump is gone by now: this one must stay */
					printf("Error writing file [%s], the dump is in %s\n", fname, tmp);
					kept = 1;
					ok = 0;
				}
			} else {
				if (ok)
					printf("Error writing file [%s]\n", fname);
				ok = 0;
				fclose(old);
			}
		}
		if (fclose(fp) != 0 && ok) {
			printf("Error writing file [%s]\n", fname);
			ok = 0;
		}
		if (tmp) {
			if (!old && ok) {
				mode_t mask = umask(0);

				umask(mask);
				if (chmod(tmp, 0666 & ~mask) || rename(tmp, fname)) {
					printf("Error writing file [%s]\n", fname);
					ok = 0;
				}
			}
			if ((old || !ok) && !kept)
				unlink(tmp);
			free(tmp);
		}
		free(buf);
		if (!ok)
			goto out;
		printf("Status: OK\n");
		rc = 0;
	}

out:
	if (ms_stop)
		printf("Stopped by Ctrl+C.\n");
	programmer_shutdown();
	return rc;
}

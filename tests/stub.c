/* tests/stub.c — test double for main.c: the programmer and a 64 KiB chip in chip.img.
   MS_CHIP=nor|nand|none, MS_CORRUPT=<offset> flips a byte on write,
   MS_FAIL=erase|read|shortread|write|partial|unprotect makes that operation fail. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "flashcmd_api.h"
int ECC_fcheck = 1, ECC_ignore = 0; unsigned char _ondie_ecc_flag = 1; unsigned int bsize = 4096;
static unsigned char chip[65536];
static const char *env(const char *n) { const char *v = getenv(n); return v ? v : ""; }
static int r(unsigned char *b, unsigned long f, unsigned long l) { if (!strcmp(env("MS_FAIL"),"read")) return -1; if (!strcmp(env("MS_FAIL"),"shortread")) { memcpy(b, chip + f, l / 2); return l / 2; } memcpy(b, chip + f, l); return l; }
static int w(unsigned char *b, unsigned long t, unsigned long l) { if (!strcmp(env("MS_FAIL"),"write")) return -1; if (!strcmp(env("MS_FAIL"),"partial")) { memcpy(chip + t, b, l / 2); return l / 2; } memcpy(chip + t, b, l); if (*env("MS_CORRUPT")) chip[t + atol(env("MS_CORRUPT"))] ^= 0x5a; return l; }
static int e(unsigned long o, unsigned long l) { if (!strcmp(env("MS_FAIL"),"erase")) return -2; memset(chip + o, 0xff, l); return 0; }
static int u(void) { return strcmp(env("MS_FAIL"),"unprotect") ? 0 : -3; }
int ch341a_spi_init(void) { FILE *f = fopen("chip.img", "rb"); if (f) { if (fread(chip, 1, sizeof(chip), f) != sizeof(chip)) memset(chip, 0, sizeof(chip)); fclose(f); } return 0; }
int ch341a_spi_shutdown(void) { FILE *f = fopen("chip.img", "wb"); if (!f || fwrite(chip, 1, sizeof(chip), f) != sizeof(chip)) return -1; fclose(f); return 0; }
void support_flash_list(void) {}
long flash_cmd_init(struct flash_cmd *c) {
	if (!strcmp(env("MS_CHIP"), "none")) return -1;
	c->flash_read = r; c->flash_write = w; c->flash_erase = e;
	if (strcmp(env("MS_CHIP"), "nand")) c->flash_unprotect = u;
	return sizeof(chip);
}

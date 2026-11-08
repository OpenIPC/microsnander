/* tests/stub.c — test double for main.c: the programmer and a 64 KiB chip in chip.img.
   MS_BIG=1 makes it 1 MiB, MS_LARGEST=1 prints the longest read or write,
   MS_CHIP=nor|nand|none, MS_CORRUPT=<offset> flips a byte on write,
   MS_FAIL=erase|erasebyte|read|shortread|write|partial|unprotect makes that operation fail. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include "flashcmd_api.h"
#include "timer.h"
static int stops;
const char *flash_chip_name = "W25Q80";
int ECC_fcheck = 1, ECC_ignore = 0, Skip_BAD_page = 0, Erase_marked_blocks = 0; unsigned long long Skip_BAD_end; unsigned char _ondie_ecc_flag = 1; unsigned int bsize = 4096;
static unsigned char chip[1 << 20];	/* MS_BIG=1: all of it; else 64 KiB */
static unsigned long largest;		/* MS_LARGEST=1: the longest read or write is printed */
static void note(unsigned long l) { if (l > largest) largest = l; }
static const char *env(const char *n) { const char *v = getenv(n); return v ? v : ""; }
static int r(unsigned char *b, unsigned long f, unsigned long l) { note(l); if (!strcmp(env("MS_FAIL"),"stopread") && ++stops == 2) ms_stop = 1; if (!strcmp(env("MS_FAIL"),"read")) return -1; if (!strcmp(env("MS_FAIL"),"shortread")) { memcpy(b, chip + f, l / 2); return l / 2; } memcpy(b, chip + f, l); return l; }
static int w(unsigned char *b, unsigned long t, unsigned long l) { note(l); if (ms_stop) return -1; if (*env("MS_PROTECT")) return l; if (!strcmp(env("MS_FAIL"),"write")) return -1; if (!strcmp(env("MS_FAIL"),"stopwrite")) { memcpy(chip + t, b, l); ms_stop = 1; return l; } if (!strcmp(env("MS_FAIL"),"partial")) { memcpy(chip + t, b, l / 2); return l / 2; } memcpy(chip + t, b, l); if (*env("MS_CORRUPT")) { unsigned long x = atol(env("MS_CORRUPT")); if (x >= t && x < t + l) chip[x] ^= 0x5a; } return l; }
/* MS_FAIL=stop: Ctrl+C comes during the first erase, which then stops after it erased */
static int e(unsigned long o, unsigned long l) { if (*env("MS_PROTECT")) return 0; if (!strcmp(env("MS_FAIL"),"erase")) return -2; if (ms_stop) return -1; memset(chip + o, 0xff, l); if (!strcmp(env("MS_FAIL"),"erasebyte") && l > 0x1234) chip[o + 0x1234] = 0x5a; if (!strcmp(env("MS_FAIL"),"stop") && !stops++) { ms_stop = 1; return -1; } if (!strcmp(env("MS_FAIL"),"stoprestore") && stops++ == 1) raise(SIGINT); return 0; }
static int u(void) { return strcmp(env("MS_FAIL"),"unprotect") ? 0 : -3; }
static int q(void) { printf("clear QE\n"); return strcmp(env("MS_FAIL"),"qe") ? 0 : -2; }
#include <strings.h>
#include "programmer.h"
/* the I2C of the CH341A and the CH347: a SigmaStar ISP bridge at 0x49 and 0x59, or (MS_I2C=none) nothing */
static int i2c_sp(unsigned int sp) { (void)sp; return 0; }
static int i2c_w(unsigned char a, const unsigned char *b, unsigned int n) { (void)b; (void)n; return strcmp(env("MS_I2C"), "none") && (a == 0x49 || a == 0x59) ? 0 : -1; }
static int i2c_r(unsigned char a, unsigned char *b, unsigned int n) { if (n) memset(b, 0, n); return i2c_w(a, NULL, 0); }
static const struct programmer stubprogs[] = {
	{ .key = "ch341a", .name = "CH341A", .send = (void *)1, .i2c_speed = i2c_sp, .i2c_write = i2c_w, .i2c_read = i2c_r, .i2c_max = 24 },
	{ .key = "ch347", .name = "CH347", .send = (void *)1, .clock = true, .i2c_speed = i2c_sp, .i2c_write = i2c_w, .i2c_read = i2c_r, .i2c_max = 60 },
	{ .key = "ezp", .name = "EZP" } };
static const struct programmer *chosen = &stubprogs[0];	/* auto: the stub has a CH341A */
const struct programmer *programmer;
int programmer_select(const char *n) {
	if (!strcasecmp(n, "auto")) { chosen = &stubprogs[0]; return 0; }
	for (int i = 0; i < 3; i++) if (!strcasecmp(n, stubprogs[i].key)) { chosen = &stubprogs[i]; return 0; }
	return -1; }
const char *programmer_names(void) { return "ch341a, ch347, ezp, auto"; }
int ch347_set_speed(const char *s) { return strcasecmp(s, "60M") && strcasecmp(s, "30M") && strcasecmp(s, "15M") && strcasecmp(s, "7.5M") && strcasecmp(s, "3.75M") && strcasecmp(s, "1.875M") && strcasecmp(s, "937.5K") && strcasecmp(s, "468.75K") ? -1 : 0; }
const char *ch347_speed_list(void) { return "60M, 30M, 15M, 7.5M, 3.75M, 1.875M, 937.5K, 468.75K"; }
int programmer_init(bool need_clock) { if (need_clock && !chosen->clock) { printf("-S: fixed clock\n"); return -2; } programmer = chosen; if (*env("MS_PROGRAMMER")) printf("programmer: %s\n", programmer->key); size_t n = *env("MS_BIG") ? sizeof(chip) : 65536; FILE *f = fopen("chip.img", "rb"); if (f) { if (fread(chip, 1, n, f) != n) memset(chip, 0, sizeof(chip)); fclose(f); } return 0; }
int web_serve(const char *l, const char *k, const char *d, unsigned long long a, unsigned long long n)
{ printf("web: %s key=%s dir=%s addr=0x%llx len=0x%llx\n", l, k ? k : "-", d ? d : "-", a, n); return 0; }
int programmer_shutdown(void) { if (*env("MS_LARGEST")) printf("largest 0x%lx\n", largest); size_t n = *env("MS_BIG") ? sizeof(chip) : 65536; FILE *f = fopen("chip.img", "wb"); if (!f || fwrite(chip, 1, n, f) != n) return -1; fclose(f); return 0; }
void support_flash_list(void) {}
long flash_cmd_init(struct flash_cmd *c) {
	if (!strcmp(env("MS_CHIP"), "none")) return -1;
	c->flash_read = r; c->flash_write = w; c->flash_erase = e;
	if (strcmp(env("MS_CHIP"), "nand")) { c->flash_unprotect = u; c->flash_clear_qe = q; }
	c->kind = !strcmp(env("MS_CHIP"), "nand") ? FLASH_NAND : FLASH_NOR;
	c->lock = *env("MS_LOCK") ? (int)strtol(env("MS_LOCK"), NULL, 16) : -1;
	if (programmer && !strcmp(programmer->key, "ezp")) c->kind = FLASH_FIRMWARE;
	return *env("MS_BIG") ? sizeof(chip) : 65536;
}

/* MS_SR=1c0000: the status registers SR1 SR2 SR3 */
int snor_read_status_at(unsigned long a, unsigned char sr[3]);
int snor_read_status(unsigned char sr[3]) { return snor_read_status_at(0, sr); }
int snor_read_status_at(unsigned long a, unsigned char sr[3]) { (void)a; unsigned long v = strtoul(*env("MS_SR") ? env("MS_SR") : "000200", NULL, 16); sr[0] = v >> 16; sr[1] = v >> 8; sr[2] = v; return 0; }
int snor_block_locks(void) { return *env("MS_SST26") != 0; }
/* -i and -O: MS_SECURITY=0 a chip without known security registers, =fail a read that fails */
int snor_info(void) { printf("Protection: none.\n"); return !strcmp(env("MS_INFO"), "fail") ? -1 : 0; }
long snor_security_size(void) { return !strcmp(env("MS_SECURITY"), "0") ? 0 : 768; }
int snor_read_security(unsigned char *b) { if (!strcmp(env("MS_SECURITY"), "fail")) return -1; for (int i = 0; i < 768; i++) b[i] = (unsigned char)i; return 0; }
int snor_sr1_macronix(void) { return *env("MS_MACRONIX") != 0; }
/* -b on the stub NAND: MS_FAIL=marks makes the marks fail */
int snand_bad_blocks(unsigned long a, unsigned long l) { printf("bad blocks 0x%lx 0x%lx\n", a, l); return strcmp(env("MS_FAIL"), "marks") ? 0 : -1; }

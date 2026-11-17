/* tests/websim.c — src/web.c with a fake programmer and a 1 MB flash behind
   it: requests go through a socket pair to handle(), the operations run in
   their thread, and the dumps land in a temporary directory. */
#include <stdlib.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include "../src/ops.c"
#include "../src/web.c"
#include "../src/voltage.c"
#include "../src/selftest.c"
volatile sig_atomic_t ms_stop;		/* timer.c is not built in */

#define SIM_SIZE (1 << 20)
static unsigned char mem[SIM_SIZE];
static int unprotects, opens, fail_open;
const char *ms_version = "test";
static const char *extra = "";		/* more request headers */
#define CHIP80 "Host: 127.0.0.1:8080\r\nX-MicroSNANDer-Chip: W25Q80;1048576\r\n"
const char *flash_chip_name;
static const struct programmer fake = { .key = "ch347", .name = "CH347", .send = (void *)1 };
static const struct programmer fake341 = { .key = "ch341a", .name = "CH341A", .send = (void *)1 };
static int use_ch341a, no_chip;		/* the CH341A in place of the CH347; no chip in it */
const struct programmer *programmer;

int programmer_init(bool need_clock) { (void)need_clock; if (fail_open) return -1; programmer = use_ch341a ? &fake341 : &fake; opens++; return 0; }
int programmer_shutdown(void) { programmer = NULL; return 0; }
static int f_read(unsigned char *b, unsigned long from, unsigned long len) { memcpy(b, mem + from, len); return len; }
static int write_no, fail_write;	/* fail_write: the write of that number (from 1) fails */
static int f_write(unsigned char *b, unsigned long to, unsigned long len) { if (fail_write && ++write_no == fail_write) return -1; for (unsigned long i = 0; i < len; i++) mem[to + i] &= b[i]; return len; }
static int f_erase(unsigned long offs, unsigned long len) { memset(mem + offs, 0xff, len); return 0; }
static int f_unprotect(void) { unprotects++; return 0; }
static int swap_chip, inits;		/* swap_chip: another chip from the second open on */
static const char *sim_chip;		/* in place of W25Q80 */
long flash_cmd_init(struct flash_cmd *c)
{
	c->flash_read = f_read; c->flash_write = f_write; c->flash_erase = f_erase; c->flash_unprotect = f_unprotect;
	if (no_chip)
		return -1;
	flash_chip_name = sim_chip ? sim_chip : swap_chip && inits++ ? "W25Q16" : "W25Q80";
	return SIM_SIZE;
}
char snor_unprotect_why[200];
int snor_unprotect(void) { return 0; }		/* the real driver is not in this test */
unsigned int bsize = 4096;
int snor_block_locks(void) { return 0; }
static int sim_macronix;
int snor_sr1_macronix(void) { return sim_macronix; }
static unsigned char sim_sr1 = 0x1c;		/* BP bits set: a protected chip */
int snor_read_status(unsigned char sr[3]) { sr[0] = sim_sr1; sr[1] = 0x02; sr[2] = 0x60; return 0; }
int snor_write_status(int reg, unsigned char mask, unsigned char val) { if (reg) return -3; sim_sr1 = (sim_sr1 & ~mask) | (val & mask); return 0; }

static char resp[1 << 21];
static size_t nresp;

static void *serve_one(void *arg)
{
	int fd = (int)(long)arg;

	handle(fd);
	close(fd);
	return NULL;
}

static size_t short_by;		/* the client sends this many bytes less than its Content-Length, then closes */

/* one request through handle(); the answer in resp */
static int request(const char *method, const char *path, const char *auth, const void *body, size_t blen)
{
	int sv[2];
	char head[512];

	socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
	int k = snprintf(head, sizeof(head), "%s %s HTTP/1.0\r\n%s%s%s%s%sContent-Length: %zu\r\n\r\n",
			 method, path, *extra ? extra : "Host: 127.0.0.1:8080\r\n",
			 strcmp(method, "POST") ? "" : "X-MicroSNANDer: 1\r\n",
			 auth ? "Authorization: Basic " : "", auth ? auth : "", auth ? "\r\n" : "", blen);
	pid_t p = fork();
	if (!p) {				/* the client side writes the request */
		close(sv[1]);
		send_all(sv[0], head, k);
		if (blen > short_by) send_all(sv[0], body, blen - short_by);
		_exit(0);
	}
	pthread_t t;

	if (short_by) {			/* the client is gone: the server sees the end, nobody reads */
		close(sv[0]);
		waitpid(p, NULL, 0);
		serve_one((void *)(long)sv[1]);
		resp[nresp = 0] = 0;
		return 0;
	}
	pthread_create(&t, NULL, serve_one, (void *)(long)sv[1]);	/* the server side, while the answer is read */
	nresp = 0;
	ssize_t r;
	while ((r = recv(sv[0], resp + nresp, sizeof(resp) - 1 - nresp, 0)) > 0)
		nresp += r;
	resp[nresp] = 0;
	pthread_join(t, NULL);
	close(sv[0]);
	waitpid(p, NULL, 0);
	return atoi(resp + 9);			/* "HTTP/1.0 NNN" */
}

static const char *body_of(void) { char *b = strstr(resp, "\r\n\r\n"); return b ? b + 4 : ""; }

static void wait_idle(void)
{
	for (int i = 0; i < 2000; i++) {
		pthread_mutex_lock(&st.lock);
		int b = st.busy;
		pthread_mutex_unlock(&st.lock);
		if (!b) return;
		usleep(1000);
	}
}

static int dev_fails, dev_opens, dev_closes;
static int dev_open(void) { dev_opens++; return dev_fails ? -1 : 0; }
static void dev_close(void) { dev_closes++; }
static volatile int served;
static void *serve_thread(void *dir) { long r = web_serve("127.0.0.1:0", NULL, dir, 0, 0); served = 1; return (void *)r; }

static int fail;
static void check(int ok, const char *what) { printf("%s %s\n", ok ? "ok  " : "FAIL", what); if (!ok) fail = 1; }

int main(void)
{
	static unsigned char img[200000];	/* over three parts of 64 KB */
	char dir[] = "/tmp/websimXXXXXX";

	if (!mkdtemp(dir)) return 1;
	web_dir = dir;
	for (unsigned i = 0; i < SIM_SIZE; i++) mem[i] = (unsigned char)(i * 7 + (i >> 9));

	check(request("GET", "/", NULL, NULL, 0) == 200 && strstr(body_of(), "<title>MicroSNANDer Web</title>") && strstr(body_of(), "Registers"),
	      "GET / gives the page");
	check(strstr(body_of(), "if(s.seq<seen&&s.seq!==0)return") != NULL, "  an older state does not replace a newer one on the page");
	check(strstr(body_of(), "id=\"hexpanel\"") && strstr(body_of(), "Range:'bytes='+a+'-'+(a+HXP-1)") &&
	      strstr(body_of(), "if(b&&my===hx.req)hexDraw(") &&
	      strstr(body_of(), "[0-9a-f]{1,12}$/i") && strstr(body_of(), "if(hx.size&&n>=hx.size)") &&
	      strstr(body_of(), "if(my!==hx.req)return null;") && strstr(body_of(), "if(s.file)hexOpen(s.file);else hexClose()"),
	      "  the hex view of the last dump, read with Range, only the last answer used, hidden with no dump");
	check(strstr(body_of(), "function detectThen") && strstr(body_of(), "(o==='erase'||o==='write'||o==='selftest'||o==='unprotect')&&!s.chip"),
	      "  Erase and Write wait for a chip: off without one, a Detect before the question");

	web_password = "secret";
	check(request("GET", "/api/state", NULL, NULL, 0) == 401 &&
	      strstr(resp, "WWW-Authenticate: Basic realm=\"microsnander\", charset=\"UTF-8\""), "with -K: no password, 401");
	check(request("GET", "/api/state", "eDp3cm9uZw==", NULL, 0) == 401, "  a wrong password, 401");	/* x:wrong */
	check(request("GET", "/api/state", "eDpzZWNyZXQ=", NULL, 0) == 200, "  the right one (any user name), 200");	/* x:secret */
	web_password = NULL;

	check(request("POST", "/api/detect", NULL, NULL, 0) == 202 && strstr(resp, "Started 1."), "Detect starts, as operation 1");
	wait_idle();
	request("GET", "/api/state", NULL, NULL, 0);
	check(strstr(body_of(), "\"programmer\":\"CH347\"") && strstr(body_of(), "\"chip\":\"W25Q80\"") &&
	      strstr(body_of(), "\"size\":1048576") && strstr(body_of(), "\"sr\":[28,2,96]") && strstr(body_of(), "\"ok\":true"),
	      "  the state has the programmer, the chip, its size and the status registers");
	check(strstr(body_of(), "\"voltage\":\"3.3\"") && strstr(body_of(), "\"warning\":\"\""),
	      "  a 3.3 V chip on the CH347: no warning");
	check(strstr(body_of(), "\"seq\":1,") != NULL, "  the state is of operation 1");

	/* Erase for a chip confirmed but not in the socket: refused, nothing erased */
	mem[0] = 0x12;
	extra = "Host: 127.0.0.1:8080\r\nX-MicroSNANDer-Chip: W25Q16;2097152\r\n";
	check(request("POST", "/api/erase", NULL, NULL, 0) == 202, "Erase for the W25Q16 confirmed, a W25Q80 in the socket");
	extra = "";
	wait_idle();
	request("GET", "/api/state", NULL, NULL, 0);
	check(strstr(body_of(), "\"ok\":false") && strstr(body_of(), "W25Q80 (1 MB) now, not the W25Q16 (2 MB)") && mem[0] == 0x12,
	      "  refused before anything is erased");
	extra = "Host: 127.0.0.1:8080\r\nX-MicroSNANDer-Chip: W25Q80;1048576\r\n";
	request("POST", "/api/erase", NULL, NULL, 0);
	extra = "";
	wait_idle();
	request("GET", "/api/state", NULL, NULL, 0);
	check(strstr(body_of(), "\"ok\":true") && mem[0] == 0xff, "  for the W25Q80 that is there: erased");

	sim_chip = "W25Q128FW";
	request("POST", "/api/detect", NULL, NULL, 0);
	wait_idle();
	request("GET", "/api/state", NULL, NULL, 0);
	check(strstr(body_of(), "\"voltage\":\"1.8\"") && strstr(body_of(), "\"warning\":\"W25Q128FW is a 1.8 V chip"),
	      "  a 1.8 V chip: the voltage and the warning in the state");
	use_ch341a = 1;
	request("POST", "/api/detect", NULL, NULL, 0);
	wait_idle();
	request("GET", "/api/state", NULL, NULL, 0);
	check(strstr(body_of(), "\"programmer\":\"CH341A\"") && strstr(body_of(), "1.8 V chip") && strstr(body_of(), "5 V on the chip lines"),
	      "  the same chip on the CH341A: the 1.8 V and the 5 V warning");
	use_ch341a = 0; no_chip = 1;
	request("POST", "/api/detect", NULL, NULL, 0);
	wait_idle();
	request("GET", "/api/state", NULL, NULL, 0);
	check(strstr(body_of(), "\"programmer\":\"CH347\"") && strstr(body_of(), "\"chip\":\"\"") &&
	      strstr(body_of(), "\"warning\":\"\"") && strstr(body_of(), "\"sr\":null"),
	      "  then the CH347 with no chip: the chip and the warnings of the CH341A are gone");
	use_ch341a = 0; no_chip = 0;
	request("POST", "/api/detect", NULL, NULL, 0);
	wait_idle();
	no_chip = 1;					/* the same programmer, the chip taken out */
	request("POST", "/api/detect", NULL, NULL, 0);
	wait_idle();
	request("GET", "/api/state", NULL, NULL, 0);
	check(strstr(body_of(), "\"chip\":\"\"") && strstr(body_of(), "\"ok\":false"),
	      "  a failed Detect on the same programmer forgets the chip: Erase and Write go off");
	no_chip = 0;
	sim_chip = NULL;
	request("POST", "/api/detect", NULL, NULL, 0);
	wait_idle();

	check(request("POST", "/api/read", NULL, NULL, 0) == 202, "Read starts");
	wait_idle();
	request("GET", "/api/state", NULL, NULL, 0);
	char file[128] = "";
	const char *f = strstr(body_of(), "\"file\":\"");
	if (f) sscanf(f + 8, "%127[^\"]", file);
	int named = strlen(file) == strlen("flash-202611031706-W25Q80.bin") && !strncmp(file, "flash-20", 8) && strstr(file, "-W25Q80.bin");
	check(named, "  the dump is flash-YYYYMMDDHHMM-W25Q80.bin");
	char path[256];
	snprintf(path, sizeof(path), "/dumps/%s", file);
	check(request("GET", path, NULL, NULL, 0) == 200 && strstr(resp, "attachment; filename=") &&
	      nresp - (body_of() - resp) == SIM_SIZE && !memcmp(body_of(), mem, SIM_SIZE), "  and downloads with the chip content");
	{
		char want[64];

		extra = "Host: 127.0.0.1:8080\r\nRange: bytes=4096-4607\r\n";
		snprintf(want, sizeof(want), "Content-Range: bytes 4096-4607/%d\r\n", SIM_SIZE);
		check(request("GET", path, NULL, NULL, 0) == 206 && strstr(resp, want) && nresp - (body_of() - resp) == 512 &&
		      !memcmp(body_of(), mem + 4096, 512), "  a part of it with Range (the hex view): 206, those 512 bytes");
		extra = "Host: 127.0.0.1:8080\r\nRange: bytes=1048000-\r\n";
		check(request("GET", path, NULL, NULL, 0) == 206 && nresp - (body_of() - resp) == SIM_SIZE - 1048000 &&
		      !memcmp(body_of(), mem + 1048000, SIM_SIZE - 1048000), "  an open range: to the end");
		extra = "Host: 127.0.0.1:8080\r\nRange: bytes=1048000-2000000\r\n";
		check(request("GET", path, NULL, NULL, 0) == 206 && nresp - (body_of() - resp) == SIM_SIZE - 1048000,
		      "  a range past the end is cut at the end");
		snprintf(want, sizeof(want), "Range: bytes=%d-\r\n", SIM_SIZE);
		char hdr[128];
		snprintf(hdr, sizeof(hdr), "Host: 127.0.0.1:8080\r\n%s", want);
		extra = hdr;
		check(request("GET", path, NULL, NULL, 0) == 416 && strstr(resp, "Content-Range: bytes */"), "  from past the end: 416");
		extra = "Host: 127.0.0.1:8080\r\nRange:\tbytes=16-31 \r\n";
		check(request("GET", path, NULL, NULL, 0) == 206 && nresp - (body_of() - resp) == 16 && !memcmp(body_of(), mem + 16, 16),
		      "  a tab before the range and a space after it: still that part");
		extra = "Host: 127.0.0.1:8080\r\nRange: bytes=0-15\r\nRange: bytes=32-47\r\n";
		check(request("GET", path, NULL, NULL, 0) == 200 && nresp - (body_of() - resp) == SIM_SIZE,
		      "  two Range headers: the whole dump");
		extra = "Host: 127.0.0.1:8080\r\nRange: bytes=0-15,32-47\r\n";
		check(request("GET", path, NULL, NULL, 0) == 200 && nresp - (body_of() - resp) == SIM_SIZE,
		      "  several ranges: the whole dump, as without Range");
		extra = "";
	}
	request("POST", "/api/read", NULL, NULL, 0);
	wait_idle();
	request("GET", "/api/state", NULL, NULL, 0);
	char second[160];
	snprintf(second, sizeof(second), "\"file\":\"%.*s-2.bin\"", (int)strlen(file) - 4, file);
	check(strstr(body_of(), second) != NULL, "  a second read in the same minute keeps the first: -2");
	char lnk[300];
	snprintf(lnk, sizeof(lnk), "%s/flash-link.bin", dir);
	if (symlink("/etc/passwd", lnk)) {}
	check(request("GET", "/dumps/flash-link.bin", NULL, NULL, 0) == 404, "  a link in the dump directory is not served");
	check(request("GET", "/dumps/../websim.c", NULL, NULL, 0) == 404 && request("GET", "/dumps/flash-x/../a.bin", NULL, NULL, 0) == 404,
	      "  no other file is served");

	web_addr = 0x1000; web_len = 0x2000;
	request("POST", "/api/read", NULL, NULL, 0);
	wait_idle();
	request("GET", "/api/state", NULL, NULL, 0);
	check(strstr(body_of(), "-W25Q80-0x1000-0x2000.bin\"") != NULL, "with -a/-l the dump name has the range");
	web_addr = web_len = 0;

	mem[0] = 0x12;
	check(request("POST", "/api/erase", NULL, NULL, 0) == 400 && mem[0] == 0x12, "Erase without the chip confirmed: 400, nothing erased");
	check(request("POST", "/api/write", NULL, img, sizeof(img)) == 400 && !st.uploading, "Write without the chip confirmed: 400, the upload slot is free");
	extra = CHIP80;
	check(request("POST", "/api/erase", NULL, NULL, 0) == 202, "Erase starts");
	wait_idle();
	request("GET", "/api/state", NULL, NULL, 0);
	check(strstr(body_of(), "\"sr\":[28,2,96]") && strstr(body_of(), "\"chip\":\"W25Q80\""), "  the registers and the chip stay shown after it");
	int erased = 1;
	for (unsigned i = 0; i < SIM_SIZE; i++) if (mem[i] != 0xff) { erased = 0; break; }
	check(erased, "  the whole chip is erased");

	for (unsigned i = 0; i < sizeof(img); i++) img[i] = (unsigned char)(i * 3 + 1);
	check(request("POST", "/api/write", NULL, img, sizeof(img)) == 202, "Write starts with the uploaded file");
	wait_idle();
	request("GET", "/api/state", NULL, NULL, 0);
	check(!memcmp(mem, img, sizeof(img)) && mem[sizeof(img)] == 0xff && strstr(body_of(), "\"ok\":true") && strstr(body_of(), "verified"),
	      "  written from address 0 and verified");
	int left = 0;
	DIR *d = opendir(dir);
	for (struct dirent *e; d && (e = readdir(d)); )
		left += !strncmp(e->d_name, ".microsnander-write", 19);
	if (d) closedir(d);
	check(d && !left, "  the uploaded file was kept on the disk without a name and is gone");
	request("POST", "/api/write", NULL, img, sizeof(img));	/* over the data: the verify finds it */
	wait_idle();
	img[0] ^= 0xff;
	request("POST", "/api/write", NULL, img, sizeof(img));
	wait_idle();
	request("GET", "/api/state", NULL, NULL, 0);
	check(strstr(body_of(), "\"ok\":false") && strstr(body_of(), "Verify failed"), "  a write that does not take fails the verify");
	check(strstr(body_of(), "\"phase\":\"verify\"") && !strstr(body_of(), "\"total\":0,"), "  the phase and the progress stay after the failure, for the bar");
	{
		int opens_before = opens;
		struct ms_chip c;
		char m[128];
		FILE *e = tmpfile();
		check(ms_write_file(&c, e, NULL, NULL, m, sizeof(m)) < 0 && strstr(m, "empty") && opens == opens_before,
		      "  an empty file is refused before the programmer is opened");
		ms_expect("W25Q16", 2097152);		/* a write for another chip that ends early */
		check(ms_write_file(&c, e, NULL, NULL, m, sizeof(m)) < 0 && !ms_detect(&c, m, sizeof(m)) && !strcmp(c.chip, "W25Q80"),
		      "  and its chip check does not stay for the next Detect");
		ms_expect("W25Q16", 2097152);
		check(!ms_detect(&c, m, sizeof(m)), "  a Detect is never bound to the chip of an Erase or Write");
		fclose(e);
	}
	{						/* an -o path longer than any fixed buffer */
		static char deep[1200];
		int k = snprintf(deep, sizeof(deep), "%s", dir);
		while (k < 1000) {
			k += snprintf(deep + k, sizeof(deep) - k, "/%s", "a-long-directory-name-for-the-dumps");
			mkdir(deep, 0755);
		}
		web_dir = deep;
		FILE *f = upload_file();
		check(f != NULL, "  the file to write is stored under an -o path of over 1000 characters");
		if (f) fclose(f);
		web_dir = dir;
	}
	short_by = 100;					/* the browser goes away in the middle of the file */
	unsigned int seq_before = st.seq;
	memset(mem, 0xff, sizeof(img));
	check(request("POST", "/api/write", NULL, img, sizeof(img)) == 0 && !st.uploading && mem[0] == 0xff,
	      "a file that stops coming: no answer to a client that is gone, the upload slot free, nothing written");
	short_by = 0;
	request("GET", "/api/state", NULL, NULL, 0);
	check(strstr(body_of(), "\"op\":\"write\"") && strstr(body_of(), "\"ok\":false") && st.seq == seq_before + 1 &&
	      strstr(body_of(), "The file stopped coming after") && strstr(body_of(), "the connection was closed") &&
	      strstr(body_of(), "nothing was written"),
	      "  the state says it, as a failed Write, for the page");
	request("GET", "/", NULL, NULL, 0);
	check(strstr(body_of(), "the link broke while the file was sent: see the state") && strstr(body_of(), "if(r.status===408)poll();return}") && strstr(body_of(), "if(++k<20)setTimeout(again,2000)"),
	      "  and the page tells it when its upload breaks, reads the state after a 408, and for 40 s after a broken upload");
	st.uploading = 1;				/* another upload is coming in */
	check(request("POST", "/api/write", NULL, img, sizeof(img)) == 409, "  a second upload at the same time: 409, before it takes space");
	check(request("POST", "/api/erase", NULL, NULL, 0) == 409, "  an erase while a file is coming in: 409");
	st.uploading = 0;
	web_dir = "/nonexistent-microsnander";
	check(request("POST", "/api/write", NULL, img, sizeof(img)) == 500 && strstr(resp, "Cannot store the file"),
	      "  no place for the file in the dump directory: 500, nothing starts");
	check(!st.uploading, "  and the upload slot is free again");
	web_dir = dir;
	extra = "";

	/* the self-test: protected (BP set) stops before any erase; then OK on the first block */
	memcpy(mem, "loader", 6);
	extra = "Host: 127.0.0.1:8080\r\nX-MicroSNANDer-Chip: W25Q80;1048576\r\n";
	check(request("POST", "/api/selftest", NULL, NULL, 0) == 202, "Self-test starts for the chip confirmed");
	extra = "";
	wait_idle();
	request("GET", "/api/state", NULL, NULL, 0);
	check(strstr(body_of(), "\"op\":\"selftest\"") && strstr(body_of(), "\"ok\":false") &&
	      strstr(body_of(), "PROTECTED, the registers protect part of the chip") && !memcmp(mem, "loader", 6),
	      "  BP bits set: PROTECTED, nothing erased");
	sim_sr1 = 0;
	extra = "Host: 127.0.0.1:8080\r\nX-MicroSNANDer-Chip: W25Q80;1048576\r\n";
	request("POST", "/api/selftest", NULL, NULL, 0);
	extra = "";
	wait_idle();
	request("GET", "/api/state", NULL, NULL, 0);
	check(strstr(body_of(), "\"ok\":true") && strstr(body_of(), "Self-test: OK") && !memcmp(mem, "loader", 6),
	      "  no protection: OK, and the first block holds its old data again");
	extra = "Host: 127.0.0.1:8080\r\nX-MicroSNANDer-Chip: W25Q16;2097152\r\n";
	request("POST", "/api/selftest", NULL, NULL, 0);
	extra = "";
	wait_idle();
	request("GET", "/api/state", NULL, NULL, 0);
	check(strstr(body_of(), "not the W25Q16") != NULL, "  for another chip than the one in the socket: refused");
	check(request("POST", "/api/selftest", NULL, NULL, 0) == 400 && !memcmp(mem, "loader", 6),
	      "  without the chip confirmed: 400, nothing starts");
	write_no = 0;
	fail_write = 2;					/* the pattern goes in, the old data does not */
	extra = "Host: 127.0.0.1:8080\r\nX-MicroSNANDer-Chip: W25Q80;1048576\r\n";
	request("POST", "/api/selftest", NULL, NULL, 0);
	extra = "";
	wait_idle();
	fail_write = 0;
	request("GET", "/api/state", NULL, NULL, 0);
	check(strstr(body_of(), "\"ok\":false") && strstr(body_of(), "\"file\":\"selftest-W25Q80-0x0.bin\"") &&
	      strstr(body_of(), "did not come back as it was: its old data is in /tmp/websim"),
	      "  a block that does not come back: saved in the dump directory, its name in the state");
	check(request("GET", "/dumps/selftest-W25Q80-0x0.bin", NULL, NULL, 0) == 200 && !memcmp(body_of(), "loader", 6),
	      "  and the page can download it");
	memcpy(mem, "loader", 6);
	sim_sr1 = 0x1c;
	check(strstr(web_page, "id=\"b-selftest\" disabled") != NULL, "  the page has the Self-test button, off without a chip");
	check(strstr(web_page, "id=\"bar\"") && strstr(web_page, "document.title=") && !strstr(web_page, "needle"), "  the page has the bar and the state in the tab title");
	check(strstr(web_page, "show('Link offline','bad');$('bar').className='bg';[].forEach.call($('bar').children,function(e){e.className=''})") != NULL,
	      "  no link: the bar goes dark, it does not keep the last result");
	check(strstr(web_page, "rel=\"icon\" id=\"icon\"") && strstr(web_page, "icon(s.busy?'ffb347':s.ok===true?'5ad38f':s.ok===false?'c8452c':'b09873')")
	      && strstr(web_page, "function offline(){bulb('l-link','bad');icon('c8452c');"),
	      "  the tab icon: a lamp in the colour of the state, red with no link");
	check(strstr(web_page, "@media (prefers-reduced-motion:reduce)") && strstr(web_page, "m+=speed(s);")
	      && strstr(web_page, "rate.seq!==s.seq||rate.phase!==s.phase){") && strstr(web_page, "dt>=0.25&&s.done>=rate.d")
	      && strstr(web_page, "var now=performance.now();"),
	      "  less motion: no running light; the speed starts again with each operation and phase");
	check(strstr(web_page, ".btn:disabled.engaged{filter:none}") && strstr(web_page, "detectThen(b,function(s){if(confirm('Clear the protection"),
	      "  the running button keeps its colour; Clear protection asks about the chip found");

	check(request("POST", "/api/unprotect", NULL, NULL, 0) == 400 && unprotects == 0, "Clear protection without the chip confirmed: 400");
	extra = "Host: 127.0.0.1:8080\r\nX-MicroSNANDer-Chip: W25Q16;2097152\r\n";
	request("POST", "/api/unprotect", NULL, NULL, 0);
	wait_idle();
	check(unprotects == 0, "  for another chip than the one in the socket: refused");
	extra = CHIP80;
	check(request("POST", "/api/unprotect", NULL, NULL, 0) == 202, "Clear protection starts");
	extra = "";
	wait_idle();
	request("GET", "/api/state", NULL, NULL, 0);
	check(unprotects == 1 && strstr(body_of(), "\"sr\":[28,2,96]"), "  unprotect, then the registers again");
	check(strstr(body_of(), "\"sr_macronix\":false") != NULL, "  not Macronix");
	sim_macronix = 1;
	request("POST", "/api/registers", NULL, NULL, 0);
	wait_idle();
	request("GET", "/api/state", NULL, NULL, 0);
	check(strstr(body_of(), "\"sr_macronix\":true") != NULL, "  a Macronix chip is told, the page shows its one register");
	sim_macronix = 0;
	check(strstr(web_page, "'SRWD','QE','BP3'") != NULL, "  the page has the bits of Macronix");

	swap_chip = 1; inits = 0;			/* another chip in the socket between detect and read */
	request("POST", "/api/read", NULL, NULL, 0);
	wait_idle();
	request("GET", "/api/state", NULL, NULL, 0);
	check(strstr(body_of(), "\"ok\":false") && strstr(body_of(), "The chip changed"), "a chip changed between the detect and the read: refused");
	swap_chip = 0;

	fail_open = 1;
	request("POST", "/api/detect", NULL, NULL, 0);
	wait_idle();
	request("GET", "/api/state", NULL, NULL, 0);
	check(strstr(body_of(), "\"ok\":false") && strstr(body_of(), "No programmer found") && strstr(body_of(), "\"programmer\":\"CH347\""),
	      "no programmer: the operation fails with a message, the last programmer stays shown");
	fail_open = 0;

	pthread_mutex_lock(&st.lock); st.busy = 1; pthread_mutex_unlock(&st.lock);
	check(request("POST", "/api/read", NULL, NULL, 0) == 409, "an operation while one runs: 409");
	pthread_mutex_lock(&st.lock); st.busy = 0; pthread_mutex_unlock(&st.lock);
	extra = "Host: 127.0.0.1\r\n";		/* a POST without the page header */
	{
		int sv[2]; char rq[] = "POST /api/erase HTTP/1.0\r\nHost: 127.0.0.1\r\nContent-Length: 0\r\n\r\n";
		socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
		send_all(sv[0], rq, sizeof(rq) - 1);
		handle(sv[1]); close(sv[1]);
		nresp = recv(sv[0], resp, sizeof(resp) - 1, 0); resp[nresp > 0 ? nresp : 0] = 0; close(sv[0]);
		check(atoi(resp + 9) == 403, "a POST without X-MicroSNANDer (another site): 403");
	}
	extra = "Host: evil.example:8080\r\n";
	check(request("GET", "/api/state", NULL, NULL, 0) == 403, "a Host of another name (DNS rebinding): 403");
	extra = "Host: localhost:8080\r\n";
	check(request("GET", "/api/state", NULL, NULL, 0) == 200, "  localhost: 200");
	extra = "";
	check(password_of("") == NULL && password_of(NULL) == NULL && !strcmp(password_of("x"), "x"),
	      "an empty password is none: the page is open");
	{
		unsigned char h[32];
		static const unsigned char abc[32] = {
			0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea, 0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
			0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c, 0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad };
		static const unsigned char two[32] = {	/* the 56-byte FIPS 180-2 message: two blocks */
			0x24, 0x8d, 0x6a, 0x61, 0xd2, 0x06, 0x38, 0xb8, 0xe5, 0xc0, 0x26, 0x93, 0x0c, 0x3e, 0x60, 0x39,
			0xa3, 0x3c, 0xe4, 0x59, 0x64, 0xff, 0x21, 0x67, 0xf6, 0xec, 0xed, 0xd4, 0x19, 0xdb, 0x06, 0xc1 };
		const char *m = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
		int ok = 1;

		sha256("abc", 3, h);
		ok &= !memcmp(h, abc, 32);
		sha256(m, strlen(m), h);
		ok &= !memcmp(h, two, 32);
		check(ok && password_equal("secret", "secret") && !password_equal("secret", "secreT") &&
		      !password_equal("", "secret"), "SHA-256 of the FIPS vectors; the password compared by it");
	}

	check(request("GET", "/nope", NULL, NULL, 0) == 404 && request("POST", "/api/nope", NULL, NULL, 0) == 404, "unknown paths: 404");

	/* ---- the Android app: device hooks, its own operations, any Host, stop ---- */
	web_device_open = dev_open;
	web_device_close = dev_close;
	dev_fails = 1;
	request("POST", "/api/detect", NULL, NULL, 0);
	wait_idle();
	request("GET", "/api/state", NULL, NULL, 0);
	check(strstr(body_of(), "\"ok\":false") && strstr(body_of(), "No programmer, or no access to it.") && dev_opens == 1 && dev_closes == 0,
	      "the app has no programmer for the page: the operation fails, nothing to close");
	dev_fails = 0;
	request("POST", "/api/detect", NULL, NULL, 0);
	wait_idle();
	request("GET", "/api/state", NULL, NULL, 0);
	check(strstr(body_of(), "\"ok\":true") && dev_opens == 2 && dev_closes == 1, "  with one: opened for the operation and closed after it");
	web_device_open = NULL;
	web_device_close = NULL;

	check(web_local_begin(OP_READ) == 0, "the app's own operation takes the slot");
	check(web_local_begin(OP_DETECT) == -1, "  a second one of the app: refused");
	check(request("POST", "/api/detect", NULL, NULL, 0) == 409, "  the page's while it runs: 409");
	web_local_progress("read", 50, 100);
	request("GET", "/api/state", NULL, NULL, 0);
	check(strstr(body_of(), "\"busy\":1,\"local\":true,\"op\":\"read\"") && strstr(body_of(), "\"done\":50,\"total\":100"),
	      "  the page shows it with its progress");
	{
		struct ms_chip c = { .programmer = "CH341A", .chip = "W25Q80", .size = SIM_SIZE };

		web_local_end(&c, 1, "Read by the app.", NULL);
	}
	request("GET", "/api/state", NULL, NULL, 0);
	check(strstr(body_of(), "\"busy\":0,\"local\":true") && strstr(body_of(), "\"ok\":true") &&
	      strstr(body_of(), "Read by the app.") && strstr(body_of(), "\"programmer\":\"CH341A\""), "  and how it ended");
	web_show_dump("flash-202611161200-W25Q80.bin");
	{
		char js[3072];

		web_state_json(js, sizeof(js));
		check(strstr(js, "\"file\":\"flash-202611161200-W25Q80.bin\"") != NULL, "a dump the app kept: the page links it");
	}
	request("POST", "/api/detect", NULL, NULL, 0);
	wait_idle();
	request("GET", "/api/state", NULL, NULL, 0);
	check(strstr(body_of(), "\"local\":false") != NULL, "  the page's operation after it is the page's");

	extra = "Host: box1.vpn.example.org\r\n";
	web_allow_any_host(1);
	check(request("GET", "/api/state", NULL, NULL, 0) == 200, "the app: a Host of any name, as a tunnel gives");
	web_allow_any_host(0);
	/* without a password: only names under the camex server, against DNS rebinding */
	web_allow_host_domain("prx1.Example.org");
	extra = "Host: box1.prx1.example.org:85\r\n";
	check(request("GET", "/api/state", NULL, NULL, 0) == 200, "no password: a name under the camex server, as its nginx gives");
	extra = "Host: box1.vpn.example.org\r\n";
	check(request("GET", "/api/state", NULL, NULL, 0) == 403, "  another name: 403");
	extra = "Host: xprx1.example.org\r\n";
	check(request("GET", "/api/state", NULL, NULL, 0) == 403, "  a name that only ends the same: 403");
	extra = "Host: prx1.example.org.evil.example\r\n";
	check(request("GET", "/api/state", NULL, NULL, 0) == 403, "  the server's name inside another: 403");
	web_allow_host_domain("");
	extra = "Host: box1.prx1.example.org\r\n";
	check(request("GET", "/api/state", NULL, NULL, 0) == 403, "  no tunnel: 403");
	extra = "";

	{
		pthread_t t;
		void *ret;

		web_stop();				/* before it serves: it returns at once */
		check(web_serve("127.0.0.1:0", NULL, dir, 0, 0) == 0, "web_stop before web_serve: it returns 0 at once");
		pthread_create(&t, NULL, serve_thread, dir);
		usleep(100000);
		web_stop();
		pthread_join(t, &ret);
		check((long)ret == 0, "  while it serves: it returns 0");
		pthread_create(&t, NULL, serve_thread, dir);	/* and it serves again */
		usleep(100000);
		web_stop();
		pthread_join(t, &ret);
		check((long)ret == 0, "  served again and stopped again");

		/* a page's operation runs: web_serve returns only after it, so its
		   password and directory can be freed then */
		pthread_mutex_lock(&st.lock);
		st.busy = 1;
		st.local = 0;
		pthread_mutex_unlock(&st.lock);
		served = 0;
		pthread_create(&t, NULL, serve_thread, dir);
		usleep(100000);
		web_stop();
		usleep(300000);
		int waited = !served;
		pthread_mutex_lock(&st.lock);
		st.busy = 0;
		pthread_mutex_unlock(&st.lock);
		pthread_join(t, &ret);
		check(waited && served && (long)ret == 0, "  a stop waits for the page's operation to end");
	}
	check(!name_ok("localhost.evil.example", "localhost") && !name_ok("localhost.evil.example", "localhost.localdomain") &&
	      name_ok("box", "box") && name_ok("BOX.local", "box") && !name_ok("boxer", "box"),
	      "this machine's name: box, box.local; never localhost.<a site> (Android's name is localhost)");

	char cmd[300];
	snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
	if (system(cmd)) {}
	if (!fail) printf("all websim checks passed\n");
	return fail;
}

/* tests/websim.c — src/web.c with a fake programmer and a 1 MB flash behind
   it: requests go through a socket pair to handle(), the operations run in
   their thread, and the dumps land in a temporary directory. */
#include <stdlib.h>
#include <sys/wait.h>
#include "../src/ops.c"
#include "../src/web.c"

#define SIM_SIZE (1 << 20)
static unsigned char mem[SIM_SIZE];
static int unprotects, opens, fail_open;
const char *ms_version = "test";
static const char *extra = "";		/* more request headers */
const char *flash_chip_name;
static const struct programmer fake = { .key = "ch347", .name = "CH347", .send = (void *)1 };
const struct programmer *programmer;

int programmer_init(bool need_clock) { (void)need_clock; if (fail_open) return -1; programmer = &fake; opens++; return 0; }
int programmer_shutdown(void) { programmer = NULL; return 0; }
static int f_read(unsigned char *b, unsigned long from, unsigned long len) { memcpy(b, mem + from, len); return len; }
static int f_write(unsigned char *b, unsigned long to, unsigned long len) { for (unsigned long i = 0; i < len; i++) mem[to + i] &= b[i]; return len; }
static int f_erase(unsigned long offs, unsigned long len) { memset(mem + offs, 0xff, len); return 0; }
static int f_unprotect(void) { unprotects++; return 0; }
static int swap_chip, inits;		/* swap_chip: another chip from the second open on */
long flash_cmd_init(struct flash_cmd *c)
{
	c->flash_read = f_read; c->flash_write = f_write; c->flash_erase = f_erase; c->flash_unprotect = f_unprotect;
	flash_chip_name = swap_chip && inits++ ? "W25Q16" : "W25Q80";
	return SIM_SIZE;
}
int snor_read_status(unsigned char sr[3]) { sr[0] = 0x1c; sr[1] = 0x02; sr[2] = 0x60; return 0; }

static char resp[1 << 21];
static size_t nresp;

static void *serve_one(void *arg)
{
	int fd = (int)(long)arg;

	handle(fd);
	close(fd);
	return NULL;
}

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
		if (blen) send_all(sv[0], body, blen);
		_exit(0);
	}
	pthread_t t;
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

static int fail;
static void check(int ok, const char *what) { printf("%s %s\n", ok ? "ok  " : "FAIL", what); if (!ok) fail = 1; }

int main(void)
{
	static unsigned char img[5000];
	char dir[] = "/tmp/websimXXXXXX";

	if (!mkdtemp(dir)) return 1;
	web_dir = dir;
	for (unsigned i = 0; i < SIM_SIZE; i++) mem[i] = (unsigned char)(i * 7 + (i >> 9));

	check(request("GET", "/", NULL, NULL, 0) == 200 && strstr(body_of(), "<title>MicroSNANDer Web</title>") && strstr(body_of(), "Registers"),
	      "GET / gives the page");

	web_password = "secret";
	check(request("GET", "/api/state", NULL, NULL, 0) == 401 && strstr(resp, "WWW-Authenticate: Basic"), "with -K: no password, 401");
	check(request("GET", "/api/state", "eDp3cm9uZw==", NULL, 0) == 401, "  a wrong password, 401");	/* x:wrong */
	check(request("GET", "/api/state", "eDpzZWNyZXQ=", NULL, 0) == 200, "  the right one (any user name), 200");	/* x:secret */
	web_password = NULL;

	check(request("POST", "/api/detect", NULL, NULL, 0) == 202, "Detect starts");
	wait_idle();
	request("GET", "/api/state", NULL, NULL, 0);
	check(strstr(body_of(), "\"programmer\":\"CH347\"") && strstr(body_of(), "\"chip\":\"W25Q80\"") &&
	      strstr(body_of(), "\"size\":1048576") && strstr(body_of(), "\"sr\":[28,2,96]") && strstr(body_of(), "\"ok\":true"),
	      "  the state has the programmer, the chip, its size and the status registers");

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
	request("POST", "/api/write", NULL, img, sizeof(img));	/* over the data: the verify finds it */
	wait_idle();
	img[0] ^= 0xff;
	request("POST", "/api/write", NULL, img, sizeof(img));
	wait_idle();
	request("GET", "/api/state", NULL, NULL, 0);
	check(strstr(body_of(), "\"ok\":false") && strstr(body_of(), "Verify failed"), "  a write that does not take fails the verify");

	check(request("POST", "/api/unprotect", NULL, NULL, 0) == 202, "Clear protection starts");
	wait_idle();
	request("GET", "/api/state", NULL, NULL, 0);
	check(unprotects == 1 && strstr(body_of(), "\"sr\":[28,2,96]"), "  unprotect, then the registers again");

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
	check(web_serve("127.0.0.1:0", "", dir, 0, 0) == 1, "an empty -K password is refused");

	check(request("GET", "/nope", NULL, NULL, 0) == 404 && request("POST", "/api/nope", NULL, NULL, 0) == 404, "unknown paths: 404");

	char cmd[300];
	snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
	if (system(cmd)) {}
	if (!fail) printf("all websim checks passed\n");
	return fail;
}

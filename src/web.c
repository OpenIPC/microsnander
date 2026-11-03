/*
 * web.c — the web mode (-W [address:]port): microsnander runs as a service
 * and serves a page with Detect, Read, Erase, Write and Registers for the
 * one programmer that is connected (chosen as without -W). Linux only.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * A small HTTP/1.0 server, one connection at a time. An operation runs in
 * a thread of its own; the page asks /api/state for its progress. Every
 * operation opens the programmer and closes it at the end, so it can be
 * replugged between operations. With a password (-K) every request needs
 * HTTP basic authentication (any user name).
 *
 *   GET  /                 the page
 *   GET  /api/state        the state as JSON
 *   POST /api/detect       find the programmer and the chip
 *   POST /api/read         read the chip (or -a/-l) into a file in the dump directory
 *   POST /api/erase        erase the whole chip
 *   POST /api/write        write the request body from address 0, then verify
 *   POST /api/registers    read the status registers (SPI NOR)
 *   POST /api/unprotect    clear the protection (as -u), then read them
 *   GET  /dumps/<file>     a dump made by Read
 */
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include "web.h"
#include "flashcmd_api.h"
#include "programmer.h"

#define WEB_PART	(64 * 1024)		/* read, write and verify in parts: the progress */
#define WEB_HEADERS	8192
#define WEB_MAX_BODY	(512UL << 20)		/* largest file to write */

enum { OP_NONE, OP_DETECT, OP_READ, OP_ERASE, OP_WRITE, OP_REGISTERS, OP_UNPROTECT };
static const char *const op_names[] = { "", "detect", "read", "erase", "write", "registers", "unprotect" };

static struct {
	pthread_mutex_t lock;
	int busy, op, ok;			/* ok: 1 done, 0 failed, -1 nothing yet */
	unsigned long long done, total;
	char phase[16];				/* "", "write", "verify" */
	char message[256];
	char programmer[32], chip[64];
	long long size;
	int sr_valid;
	unsigned char sr[3];
	char file[128];
} st = { .lock = PTHREAD_MUTEX_INITIALIZER, .ok = -1 };

static const char *web_dir = ".";
static const char *web_password;
static unsigned long long web_addr, web_len;
static unsigned char *upload;
static unsigned long upload_len;

static void set_msg(const char *fmt, ...)
{
	va_list ap;

	pthread_mutex_lock(&st.lock);
	va_start(ap, fmt);
	vsnprintf(st.message, sizeof(st.message), fmt, ap);
	va_end(ap);
	pthread_mutex_unlock(&st.lock);
}

static void set_progress(const char *phase, unsigned long long done, unsigned long long total)
{
	pthread_mutex_lock(&st.lock);
	snprintf(st.phase, sizeof(st.phase), "%s", phase);
	st.done = done;
	st.total = total;
	pthread_mutex_unlock(&st.lock);
}

/* ---- the operations, in the worker thread ---- */

static struct flash_cmd prog;
static long long flen;

static int chip_open(void)
{
	int ret = programmer_init(false);

	if (ret == -2) {
		set_msg("Several programmers, or two of one kind, are connected: connect one.");
		return -1;
	}
	if (ret < 0) {
		set_msg("No programmer found.");
		return -1;
	}
	memset(&prog, 0, sizeof(prog));
	flash_chip_name = NULL;
	flen = flash_cmd_init(&prog);
	pthread_mutex_lock(&st.lock);
	snprintf(st.programmer, sizeof(st.programmer), "%s", programmer->name);
	snprintf(st.chip, sizeof(st.chip), "%s", flen > 0 && flash_chip_name ? flash_chip_name : "");
	st.size = flen > 0 ? flen : 0;
	pthread_mutex_unlock(&st.lock);
	if (flen <= 0) {
		set_msg("No chip found.");
		programmer_shutdown();
		return -1;
	}
	return 0;
}

static void chip_close(void)
{
	programmer_shutdown();
}

/* the status registers: SPI NOR through an SPI bridge only */
static int registers(void)
{
	unsigned char sr[3];
	int ok = prog.flash_unprotect && programmer->send && !snor_read_status(sr);

	pthread_mutex_lock(&st.lock);
	st.sr_valid = ok;
	if (ok)
		memcpy(st.sr, sr, 3);
	pthread_mutex_unlock(&st.lock);
	return ok ? 0 : -1;
}

/* flash-YYYYMMDDHHMM-CHIP[-0xADDR-0xLEN].bin */
static void dump_name(char *name, size_t n, const char *chip, int range, unsigned long long a, unsigned long long l)
{
	char stamp[16], up[64];
	time_t now = time(NULL);
	struct tm tm;
	size_t i;

	localtime_r(&now, &tm);
	strftime(stamp, sizeof(stamp), "%Y%m%d%H%M", &tm);
	for (i = 0; chip[i] && i < sizeof(up) - 1; i++) {
		char c = chip[i];

		up[i] = (c >= 'a' && c <= 'z') ? c - 32 : (c == '/' || c == ' ' ? '_' : c);
	}
	up[i] = 0;
	if (range)
		snprintf(name, n, "flash-%s-%s-0x%llx-0x%llx.bin", stamp, up, a, l);
	else
		snprintf(name, n, "flash-%s-%s.bin", stamp, up);
}

static int op_read(void)
{
	static unsigned char part[WEB_PART];
	unsigned long long a = web_addr, l = web_len ? web_len : (unsigned long long)flen - web_addr;
	char name[128], path[1024];
	FILE *f;

	if (web_addr >= (unsigned long long)flen || l > (unsigned long long)flen - web_addr) {
		set_msg("The range 0x%llx + 0x%llx is past the chip end (0x%llx).", web_addr, l, flen);
		return -1;
	}
	dump_name(name, sizeof(name), st.chip, web_len || web_addr, a, l);
	snprintf(path, sizeof(path), "%s/%s", web_dir, name);
	f = fopen(path, "wb");
	if (!f) {
		set_msg("Cannot create %s: %s", path, strerror(errno));
		return -1;
	}
	for (unsigned long long done = 0; done < l; ) {
		unsigned long n = l - done < WEB_PART ? l - done : WEB_PART;

		set_progress("read", done, l);
		if (prog.flash_read(part, a + done, n) != (int)n || fwrite(part, 1, n, f) != n) {
			fclose(f);
			unlink(path);
			set_msg("Read failed at 0x%llx.", a + done);
			return -1;
		}
		done += n;
	}
	set_progress("read", l, l);
	if (fclose(f)) {
		unlink(path);
		set_msg("Cannot write %s.", path);
		return -1;
	}
	pthread_mutex_lock(&st.lock);
	snprintf(st.file, sizeof(st.file), "%s", name);
	pthread_mutex_unlock(&st.lock);
	set_msg("Read 0x%llx bytes into %s.", l, name);
	return 0;
}

static int op_erase(void)
{
	set_progress("erase", 0, 0);
	if (prog.flash_erase(0, flen)) {
		set_msg("Erase failed.");
		return -1;
	}
	set_msg("The whole chip is erased.");
	return 0;
}

/* from address 0 (a loader or a full image), then read back and compared */
static int op_write(void)
{
	static unsigned char part[WEB_PART];
	unsigned long l = upload_len;

	if (!l || l > (unsigned long long)flen) {
		set_msg(l ? "The file (0x%lx bytes) is larger than the chip (0x%llx)." : "The file is empty.", l, flen);
		return -1;
	}
	for (unsigned long done = 0; done < l; ) {
		unsigned long n = l - done < WEB_PART ? l - done : WEB_PART;

		set_progress("write", done, l);
		if (prog.flash_write(upload + done, done, n) != (int)n) {
			set_msg("Write failed at 0x%lx.", done);
			return -1;
		}
		done += n;
	}
	for (unsigned long done = 0; done < l; ) {
		unsigned long n = l - done < WEB_PART ? l - done : WEB_PART;

		set_progress("verify", done, l);
		if (prog.flash_read(part, done, n) != (int)n) {
			set_msg("Read back failed at 0x%lx.", done);
			return -1;
		}
		for (unsigned long i = 0; i < n; i++)
			if (part[i] != upload[done + i]) {
				set_msg("Verify failed: 0x%02x at 0x%lx, the file has 0x%02x.", part[i], done + i, upload[done + i]);
				return -1;
			}
		done += n;
	}
	set_progress("verify", l, l);
	set_msg("Wrote and verified 0x%lx bytes from address 0.", l);
	return 0;
}

static int op_unprotect(void)
{
	if (!prog.flash_unprotect) {
		set_msg("Clearing the protection is for SPI NOR through the CH341A or the CH347 only.");
		return -1;
	}
	if (prog.flash_unprotect()) {
		set_msg("Clearing the protection failed.");
		return -1;
	}
	registers();
	set_msg("The protection is cleared.");
	return 0;
}

static void *worker(void *arg)
{
	int op = (int)(long)arg, ret = -1;

	if (!chip_open()) {
		switch (op) {
		case OP_DETECT:
			registers();
			set_msg("Found %s with %s, %lld MB.", st.programmer, st.chip, flen >> 20);
			ret = 0;
			break;
		case OP_READ:
			ret = op_read();
			break;
		case OP_ERASE:
			ret = op_erase();
			break;
		case OP_WRITE:
			ret = op_write();
			break;
		case OP_REGISTERS:
			ret = registers();
			if (ret)
				set_msg("The status registers are readable for SPI NOR through the CH341A or the CH347 only.");
			else
				set_msg("Status registers read.");
			break;
		case OP_UNPROTECT:
			ret = op_unprotect();
			break;
		}
		chip_close();
	}
	free(upload);
	upload = NULL;
	pthread_mutex_lock(&st.lock);
	st.ok = ret == 0;
	st.busy = 0;
	pthread_mutex_unlock(&st.lock);
	return NULL;
}

/* start an operation; 0, or -1 when one is running */
static int start(int op)
{
	pthread_t t;

	pthread_mutex_lock(&st.lock);
	if (st.busy) {
		pthread_mutex_unlock(&st.lock);
		return -1;
	}
	st.busy = 1;
	st.op = op;
	st.ok = -1;
	st.done = st.total = 0;
	st.phase[0] = 0;
	st.message[0] = 0;
	if (op == OP_DETECT || op == OP_REGISTERS || op == OP_UNPROTECT)
		st.sr_valid = 0;
	pthread_mutex_unlock(&st.lock);
	if (pthread_create(&t, NULL, worker, (void *)(long)op)) {
		pthread_mutex_lock(&st.lock);
		st.busy = 0;
		pthread_mutex_unlock(&st.lock);
		return -1;
	}
	pthread_detach(t);
	return 0;
}

/* ---- HTTP ---- */

#include "web_page.h"

static int send_all(int fd, const void *p, size_t n)
{
	const char *c = p;

	while (n) {
		ssize_t w = send(fd, c, n, MSG_NOSIGNAL);

		if (w <= 0)
			return -1;
		c += w;
		n -= w;
	}
	return 0;
}

static void reply(int fd, const char *status, const char *type, const char *extra, const void *body, size_t n)
{
	char h[512];
	int k = snprintf(h, sizeof(h), "HTTP/1.0 %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
			 "Cache-Control: no-store\r\nConnection: close\r\n%s\r\n", status, type, n, extra ? extra : "");

	if (!send_all(fd, h, k) && n)
		send_all(fd, body, n);
}

static void reply_text(int fd, const char *status, const char *text)
{
	reply(fd, status, "text/plain; charset=utf-8", NULL, text, strlen(text));
}

static void json_str(char *out, size_t n, const char *s)
{
	size_t k = 0;

	for (; *s && k + 7 < n; s++) {
		if (*s == '"' || *s == '\\')
			out[k++] = '\\', out[k++] = *s;
		else if ((unsigned char)*s < 0x20)
			k += snprintf(out + k, n - k, "\\u%04x", *s);
		else
			out[k++] = *s;
	}
	out[k] = 0;
}

static void reply_state(int fd)
{
	char body[1024], msg[512], prg[64], chip[128], file[256], sr[32] = "null";

	pthread_mutex_lock(&st.lock);
	json_str(msg, sizeof(msg), st.message);
	json_str(prg, sizeof(prg), st.programmer);
	json_str(chip, sizeof(chip), st.chip);
	json_str(file, sizeof(file), st.file);
	if (st.sr_valid)
		snprintf(sr, sizeof(sr), "[%u,%u,%u]", st.sr[0], st.sr[1], st.sr[2]);
	int n = snprintf(body, sizeof(body),
			 "{\"busy\":%d,\"op\":\"%s\",\"ok\":%s,\"phase\":\"%s\",\"done\":%llu,\"total\":%llu,"
			 "\"message\":\"%s\",\"programmer\":\"%s\",\"chip\":\"%s\",\"size\":%lld,\"sr\":%s,\"file\":\"%s\"}",
			 st.busy, op_names[st.op], st.ok < 0 ? "null" : st.ok ? "true" : "false", st.phase,
			 st.done, st.total, msg, prg, chip, st.size, sr, file);
	pthread_mutex_unlock(&st.lock);
	reply(fd, "200 OK", "application/json", NULL, body, n);
}

/* the header line "\r\n<name>" (the name without case), or NULL */
static const char *find_header(const char *headers, const char *name)
{
	size_t n = strlen(name);

	for (const char *p = headers; (p = strstr(p, "\r\n")); p += 2)
		if (!strncasecmp(p + 2, name, n))
			return p;
	return NULL;
}

static int b64val(int c)
{
	if (c >= 'A' && c <= 'Z') return c - 'A';
	if (c >= 'a' && c <= 'z') return c - 'a' + 26;
	if (c >= '0' && c <= '9') return c - '0' + 52;
	if (c == '+') return 62;
	if (c == '/') return 63;
	return -1;
}

/* the password of "Authorization: Basic ..." matches (any user name) */
static int authorized(const char *headers)
{
	const char *h;
	char dec[256];
	size_t k = 0;
	int bits = 0, acc = 0;

	if (!web_password)
		return 1;
	if (!(h = find_header(headers, "Authorization: Basic ")))
		return 0;
	for (h += 23; *h && *h != '\r' && k < sizeof(dec) - 1; h++) {
		int v = b64val(*h);

		if (v < 0)
			continue;
		acc = (acc << 6) | v;
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			dec[k++] = (acc >> bits) & 0xff;
		}
	}
	dec[k] = 0;
	h = strchr(dec, ':');
	return h && !strcmp(h + 1, web_password);
}

/* a dump name made by Read: no path, flash-*.bin */
static int dump_ok(const char *name)
{
	size_t n = strlen(name);

	return n > 10 && !strncmp(name, "flash-", 6) && !strcmp(name + n - 4, ".bin") && !strchr(name, '/') && !strstr(name, "..");
}

static void send_dump(int fd, const char *name)
{
	char path[1024], h[512];
	struct stat sb;
	FILE *f;

	snprintf(path, sizeof(path), "%s/%s", web_dir, name);
	if (!dump_ok(name) || stat(path, &sb) || !(f = fopen(path, "rb"))) {
		reply_text(fd, "404 Not Found", "No such dump.\n");
		return;
	}
	int k = snprintf(h, sizeof(h), "HTTP/1.0 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Length: %lld\r\n"
			 "Content-Disposition: attachment; filename=\"%s\"\r\nConnection: close\r\n\r\n", (long long)sb.st_size, name);
	if (!send_all(fd, h, k)) {
		static char buf[65536];
		size_t n;

		while ((n = fread(buf, 1, sizeof(buf), f)) > 0 && !send_all(fd, buf, n))
			;
	}
	fclose(f);
}

static void handle(int fd)
{
	static char head[WEB_HEADERS + 1];
	size_t got = 0;
	char *end = NULL, method[8], path[256];
	unsigned long clen = 0;

	while (got < WEB_HEADERS && !end) {
		ssize_t r = recv(fd, head + got, WEB_HEADERS - got, 0);

		if (r <= 0)
			return;
		got += r;
		head[got] = 0;
		end = strstr(head, "\r\n\r\n");
	}
	if (!end || sscanf(head, "%7s %255s", method, path) != 2) {
		reply_text(fd, "400 Bad Request", "Bad request.\n");
		return;
	}
	if (!authorized(head)) {
		reply(fd, "401 Unauthorized", "text/plain", "WWW-Authenticate: Basic realm=\"microsnander\"\r\n", "Password needed.\n", 17);
		return;
	}
	const char *cl = find_header(head, "Content-Length:");
	if (cl)
		clen = strtoul(cl + 17, NULL, 10);

	if (!strcmp(method, "GET") && !strcmp(path, "/")) {
		reply(fd, "200 OK", "text/html; charset=utf-8", NULL, web_page, sizeof(web_page) - 1);
		return;
	}
	if (!strcmp(method, "GET") && !strcmp(path, "/api/state")) {
		reply_state(fd);
		return;
	}
	if (!strcmp(method, "GET") && !strncmp(path, "/dumps/", 7)) {
		send_dump(fd, path + 7);
		return;
	}
	if (strcmp(method, "POST")) {
		reply_text(fd, "404 Not Found", "Not found.\n");
		return;
	}

	static const struct { const char *path; int op; } routes[] = {
		{ "/api/detect", OP_DETECT }, { "/api/read", OP_READ }, { "/api/erase", OP_ERASE },
		{ "/api/write", OP_WRITE }, { "/api/registers", OP_REGISTERS }, { "/api/unprotect", OP_UNPROTECT },
	};
	int op = 0;

	for (unsigned i = 0; i < sizeof(routes) / sizeof(routes[0]); i++)
		if (!strcmp(path, routes[i].path))
			op = routes[i].op;
	if (!op) {
		reply_text(fd, "404 Not Found", "Not found.\n");
		return;
	}
	pthread_mutex_lock(&st.lock);
	int busy = st.busy;
	pthread_mutex_unlock(&st.lock);
	if (busy) {
		reply_text(fd, "409 Conflict", "An operation is running.\n");
		return;
	}
	if (op == OP_WRITE) {
		size_t have = got - (end + 4 - head);
		unsigned char *body;

		if (!clen || clen > WEB_MAX_BODY || have > clen) {
			reply_text(fd, "413 Payload Too Large", "No file, or too large.\n");
			return;
		}
		if (!(body = malloc(clen))) {
			reply_text(fd, "500 Internal Server Error", "Out of memory.\n");
			return;
		}
		memcpy(body, end + 4, have);
		while (have < clen) {
			ssize_t r = recv(fd, body + have, clen - have, 0);

			if (r <= 0) {
				free(body);
				return;
			}
			have += r;
		}
		upload = body;
		upload_len = clen;
	}
	if (start(op)) {
		free(upload);
		upload = NULL;
		reply_text(fd, "409 Conflict", "An operation is running.\n");
		return;
	}
	reply_text(fd, "202 Accepted", "Started.\n");
}

int web_serve(const char *listen_on, const char *password, const char *dir,
	      unsigned long long addr, unsigned long long len)
{
	char host[64] = "0.0.0.0";
	const char *colon = strrchr(listen_on, ':');
	long port;
	struct sockaddr_in sa;
	int s, one = 1;

	if (colon) {
		size_t n = colon - listen_on;

		if (n >= sizeof(host)) n = sizeof(host) - 1;
		memcpy(host, listen_on, n);
		host[n] = 0;
		listen_on = colon + 1;
	}
	port = strtol(listen_on, NULL, 10);
	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_port = htons(port);
	if (port < 0 || port > 65535 || inet_pton(AF_INET, host, &sa.sin_addr) != 1) {
		printf("-W: give [address:]port, for example 8080 or 127.0.0.1:8080.\n");
		return 1;
	}
	web_password = password && *password ? password : NULL;
	web_dir = dir && *dir ? dir : ".";
	web_addr = addr;
	web_len = len;

	s = socket(AF_INET, SOCK_STREAM, 0);
	setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	if (s < 0 || bind(s, (struct sockaddr *)&sa, sizeof(sa)) || listen(s, 8)) {
		printf("Cannot listen on %s:%ld: %s\n", host, port, strerror(errno));
		return 1;
	}
	socklen_t sl = sizeof(sa);
	getsockname(s, (struct sockaddr *)&sa, &sl);
	printf("Web mode on http://%s:%d/%s, dumps in %s\n", host, ntohs(sa.sin_port),
	       web_password ? " (password)" : "", web_dir);
	fflush(stdout);

	for (;;) {
		int c = accept(s, NULL, NULL);

		if (c < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		struct timeval tv = { 30, 0 };
		setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
		handle(c);
		close(c);
	}
	close(s);
	return 1;
}

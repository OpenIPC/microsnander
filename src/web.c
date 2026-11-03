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
#include <fcntl.h>
#include "web.h"
#include "flashcmd_api.h"
#include "programmer.h"
#include "ops.h"
#include "voltage.h"

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
	int voltage;
	char warning[320];
	int sr_valid;
	unsigned char sr[3];
	char file[160];
} st = { .lock = PTHREAD_MUTEX_INITIALIZER, .ok = -1 };

extern const char *ms_version;

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

/* the chip found by an operation, into the state; an operation that found
   nothing, or reads no registers, leaves the last ones shown */
static void keep_chip(const struct ms_chip *c, int registers_op)
{
	pthread_mutex_lock(&st.lock);
	if (c->programmer[0] && strcmp(c->programmer, st.programmer)) {
		/* another programmer: what the last one found is not in it */
		snprintf(st.programmer, sizeof(st.programmer), "%s", c->programmer);
		st.chip[0] = 0;
		st.size = 0;
		st.warning[0] = 0;
		st.sr_valid = 0;
	}
	if (c->chip[0]) {
		snprintf(st.chip, sizeof(st.chip), "%s", c->chip);
		st.size = c->size;
		st.voltage = c->voltage;
		snprintf(st.warning, sizeof(st.warning), "%s", c->warning);
	}
	if (registers_op || c->sr_valid) {
		st.sr_valid = c->sr_valid;
		memcpy(st.sr, c->sr, 3);
	}
	pthread_mutex_unlock(&st.lock);
}

static void on_progress(void *ctx, const char *phase, unsigned long long done, unsigned long long total)
{
	(void)ctx;
	set_progress(phase, done, total);
}

/* the dump file: a second read in the same minute does not replace the
   first, it gets -2, -3, ...; a link is never followed */
static FILE *dump_create(const char *chip, unsigned long long a, unsigned long long l, char *name, size_t n,
			 char *path, size_t pn)
{
	char base[128];
	int fd = -1;
	FILE *f;

	ms_dump_name(base, sizeof(base), chip, web_len || web_addr, a, l);
	base[strlen(base) - 4] = 0;		/* without .bin */
	for (int k = 1; k < 100 && fd < 0; k++) {
		if (k == 1)
			snprintf(name, n, "%s.bin", base);
		else
			snprintf(name, n, "%s-%d.bin", base, k);
		snprintf(path, pn, "%s/%s", web_dir, name);
		fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0644);
		if (fd < 0 && errno != EEXIST)
			break;
	}
	if (fd < 0)
		return NULL;
	if (!(f = fdopen(fd, "wb")))
		close(fd);
	return f;
}

static int op_read(struct ms_chip *c, char *msg, size_t n)
{
	char name[140], path[1024];
	unsigned long long l;
	FILE *f;

	struct ms_chip found;

	if (ms_detect(c, msg, n))		/* the chip name for the file */
		return -1;
	keep_chip(c, 0);
	found = *c;
	l = web_len ? web_len : (web_addr < (unsigned long long)c->size ? c->size - web_addr : 0);
	if (web_addr >= (unsigned long long)c->size || l > (unsigned long long)c->size - web_addr) {
		snprintf(msg, n, "The range 0x%llx + 0x%llx is past the chip end (0x%llx).", web_addr, l, c->size);
		return -1;
	}
	if (!(f = dump_create(c->chip, web_addr, l, name, sizeof(name), path, sizeof(path)))) {
		snprintf(msg, n, "Cannot create %.180s: %s", path, strerror(errno));
		return -1;
	}
	/* the read opens the chip again: it has to be the one the file is named after */
	int failed = ms_read(c, f, web_addr, l, on_progress, NULL, msg, n);

	if (!failed && (strcmp(c->chip, found.chip) || c->size != found.size)) {
		snprintf(msg, n, "The chip changed during the read (%.60s, was %.60s): run again.", c->chip, found.chip);
		failed = 1;
	}
	if (failed) {
		fclose(f);
		unlink(path);
		return -1;
	}
	if (fclose(f)) {
		unlink(path);
		snprintf(msg, n, "Cannot write %.200s.", path);
		return -1;
	}
	pthread_mutex_lock(&st.lock);
	snprintf(st.file, sizeof(st.file), "%s", name);
	pthread_mutex_unlock(&st.lock);
	snprintf(msg, n, "Read 0x%llx bytes into %s.", l, name);
	return 0;
}

static void *worker(void *arg)
{
	int op = (int)(long)arg, ret = -1;
	struct ms_chip c = { .size = 0 };
	char msg[256] = "";

	switch (op) {
	case OP_DETECT:    ret = ms_detect(&c, msg, sizeof(msg)); break;
	case OP_READ:      ret = op_read(&c, msg, sizeof(msg)); break;
	case OP_ERASE:     ret = ms_erase(&c, on_progress, NULL, msg, sizeof(msg)); break;
	case OP_WRITE:     ret = ms_write(&c, upload, upload_len, on_progress, NULL, msg, sizeof(msg)); break;
	case OP_REGISTERS: ret = ms_registers(&c, msg, sizeof(msg)); break;
	case OP_UNPROTECT: ret = ms_unprotect(&c, msg, sizeof(msg)); break;
	}
	keep_chip(&c, op == OP_DETECT || op == OP_REGISTERS || op == OP_UNPROTECT);
	set_msg("%s", msg);
	free(upload);
	upload = NULL;
	pthread_mutex_lock(&st.lock);
	st.ok = ret == 0;
	st.busy = 0;
	pthread_mutex_unlock(&st.lock);
	return NULL;
}

/* start an operation (with the file to write); 0, or -1 when one is running */
static int start(int op, unsigned char *body, unsigned long blen)
{
	pthread_t t;

	pthread_mutex_lock(&st.lock);
	if (st.busy) {
		pthread_mutex_unlock(&st.lock);
		return -1;
	}
	upload = body;
	upload_len = blen;
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
		upload = NULL;
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
	char body[2048], msg[512], prg[64], chip[128], file[256], warn[640], sr[32] = "null";

	pthread_mutex_lock(&st.lock);
	json_str(msg, sizeof(msg), st.message);
	json_str(prg, sizeof(prg), st.programmer);
	json_str(chip, sizeof(chip), st.chip);
	json_str(file, sizeof(file), st.file);
	json_str(warn, sizeof(warn), st.chip[0] ? st.warning : "");
	if (st.sr_valid)
		snprintf(sr, sizeof(sr), "[%u,%u,%u]", st.sr[0], st.sr[1], st.sr[2]);
	int n = snprintf(body, sizeof(body),
			 "{\"busy\":%d,\"op\":\"%s\",\"ok\":%s,\"phase\":\"%s\",\"done\":%llu,\"total\":%llu,"
			 "\"message\":\"%s\",\"programmer\":\"%s\",\"chip\":\"%s\",\"size\":%lld,\"voltage\":\"%s\","
			 "\"warning\":\"%s\",\"sr\":%s,\"file\":\"%s\","
			 "\"version\":\"%s\"}",
			 st.busy, op_names[st.op], st.ok < 0 ? "null" : st.ok ? "true" : "false", st.phase,
			 st.done, st.total, msg, prg, chip, st.size, st.chip[0] ? ms_voltage_name(st.voltage) : "",
			 warn, sr, file, ms_version);
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

/* The Host of the request: an IP address, localhost, or this machine's name.
   A page of another site that resolves its own name to this address (DNS
   rebinding) sends its own name, and is refused. */
static int host_ok(const char *headers)
{
	const char *h = find_header(headers, "Host:");
	char host[256], me[256];
	size_t n = 0;

	if (!h)
		return 1;			/* HTTP/1.0 without Host */
	for (h += 7; *h == ' '; h++)
		;
	if (*h == '[')
		return 1;			/* an IPv6 address */
	while (*h && *h != '\r' && *h != ':' && n < sizeof(host) - 1)
		host[n++] = *h++;
	host[n] = 0;
	struct in_addr ia;
	if (inet_pton(AF_INET, host, &ia) == 1 || !strcasecmp(host, "localhost"))
		return 1;
	if (!gethostname(me, sizeof(me))) {
		me[sizeof(me) - 1] = 0;
		size_t m = strlen(me);

		if (!strcasecmp(host, me) || (!strncasecmp(host, me, m) && host[m] == '.'))
			return 1;	/* name or name.local / name.domain */
	}
	return 0;
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
	FILE *f = NULL;
	int in = -1;

	/* a regular file of the dump directory, not a link to elsewhere */
	snprintf(path, sizeof(path), "%s/%s", web_dir, name);
	if (dump_ok(name))
		in = open(path, O_RDONLY | O_NOFOLLOW);
	if (in < 0 || fstat(in, &sb) || !S_ISREG(sb.st_mode) || !(f = fdopen(in, "rb"))) {
		if (in >= 0)
			close(in);
		reply_text(fd, "404 Not Found", "No such dump.\n");
		return;
	}
	int k = snprintf(h, sizeof(h), "HTTP/1.0 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Length: %lld\r\n"
			 "Content-Disposition: attachment; filename=\"%s\"\r\nConnection: close\r\n\r\n", (long long)sb.st_size, name);
	if (!send_all(fd, h, k)) {
		char buf[16384];
		size_t n;

		while ((n = fread(buf, 1, sizeof(buf), f)) > 0 && !send_all(fd, buf, n))
			;
	}
	fclose(f);
}

static void handle(int fd)
{
	char head[WEB_HEADERS + 1];
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
	if (!host_ok(head)) {
		reply_text(fd, "403 Forbidden", "Use the address of this machine.\n");
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
	/* a header another site cannot send without asking first (no CORS
	   here), so a page elsewhere cannot start an erase or a write */
	if (!find_header(head, "X-MicroSNANDer:")) {
		reply_text(fd, "403 Forbidden", "Operations come from the page only.\n");
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
	unsigned char *body = NULL;
	if (op == OP_WRITE) {
		size_t have = got - (end + 4 - head);

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
	}
	if (start(op, body, clen)) {
		free(body);
		reply_text(fd, "409 Conflict", "An operation is running.\n");
		return;
	}
	reply_text(fd, "202 Accepted", "Started.\n");
}

#define WEB_MAX_CONN	16
static int connections;

static void *conn_thread(void *arg)
{
	int c = (int)(long)arg;

	handle(c);
	close(c);
	pthread_mutex_lock(&st.lock);
	connections--;
	pthread_mutex_unlock(&st.lock);
	return NULL;
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
	if (password && !*password) {
		printf("-K: the password is empty.\n");
		return 1;
	}
	web_password = password;
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

	/* a thread for each connection, so a slow client does not hold the others */
	for (;;) {
		int c = accept(s, NULL, NULL);
		pthread_t t;

		if (c < 0) {
			if (errno == EMFILE || errno == ENFILE || errno == ENOBUFS || errno == ENOMEM)
				usleep(100000);		/* out of resources: wait, do not stop */
			continue;			/* EINTR, ECONNABORTED, EPROTO, network errors */
		}
		struct timeval tv = { 30, 0 };
		setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
		setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
		pthread_mutex_lock(&st.lock);
		int many = connections >= WEB_MAX_CONN;
		if (!many)
			connections++;
		pthread_mutex_unlock(&st.lock);
		if (many || pthread_create(&t, NULL, conn_thread, (void *)(long)c)) {
			if (!many) {
				pthread_mutex_lock(&st.lock);
				connections--;
				pthread_mutex_unlock(&st.lock);
			}
			reply_text(c, "503 Service Unavailable", "Too many connections.\n");
			close(c);
			continue;
		}
		pthread_detach(t);
	}
}

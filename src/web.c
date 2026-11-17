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
 *   POST /api/selftest     -t on the first block
 *   GET  /dumps/<file>     a dump made by Read (Range: bytes=A-B gives a part, for the hex view)
 *
 * The Android app runs the same server in a service (web_serve in a thread of
 * its own, web_stop to end it): it opens the USB device for each operation
 * (web_device_open/web_device_close), its own buttons take the same slot as
 * the page (web_local_begin/web_local_end), and its screen reads the state
 * (web_state_json).
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
#define WEB_MAX_CONN	16
#define WEB_MAX_BODY	(512UL << 20)		/* largest file to write */

static const char *const op_names[] = { "", "detect", "read", "erase", "write", "registers", "unprotect", "selftest",
				       "register" };

static struct {
	pthread_mutex_t lock;
	int busy, op, ok;			/* ok: 1 done, 0 failed, -1 nothing yet */
	int uploading;				/* a file to write is coming: one at a time */
	unsigned int seq;			/* the number of the last operation started */
	unsigned long long done, total;
	char phase[16];				/* "", "write", "verify" */
	char message[512];
	char programmer[32], chip[64];
	long long size;
	int voltage;
	char warning[320];
	int sr_valid;
	unsigned char sr[3];
	int sr_macronix;
	char file[160];
	int local;				/* the operation is the app's own, not the page's */
	char tunnel[160];			/* the app's camex tunnel: "connected 10.8.0.5", "" none */
} st = { .lock = PTHREAD_MUTEX_INITIALIZER, .ok = -1 };

extern const char *ms_version;

static const char *web_dir = ".";
static const char *web_password;
static unsigned long long web_addr, web_len;
static FILE *upload;			/* the file to write, unlinked in the dump directory */
static int web_any_host;		/* any Host: (the app with a password, reached by a tunnel name) */
static char web_host_domain[128];	/* Host names under it are taken too (the camex server), "" none */

int (*web_device_open)(void);
void (*web_device_close)(void);

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
	if (c->programmer[0] && !c->chip[0]) {
		/* the programmer answered, no chip in it: the last one is not there */
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
		st.sr_macronix = c->sr_macronix;
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

/* the chip the page confirmed the operation for ("": any) */
static char want_chip[64];
static long long want_size;

/* The threads are joined, not detached. A detached thread unmaps its own stack
   as it ends, and in the static mipsel build that faults on a real kernel: the
   code of the libc takes the stack for its last system calls from t9, which its
   non-PIC call leaves unset. Every thread says that it has ended, under st.lock,
   and the accept loop joins it. Between two rounds of that loop there are at most
   WEB_MAX_CONN connections and an operation from each, and one operation before. */
static pthread_t ended[2 * WEB_MAX_CONN + 1];
static int ended_n;

static void thread_ended(void)
{
	ended[ended_n++] = pthread_self();
}

static void reap_threads(void)
{
	pthread_t t[sizeof(ended) / sizeof(ended[0])];
	int n;

	pthread_mutex_lock(&st.lock);
	n = ended_n;
	memcpy(t, ended, n * sizeof(t[0]));
	ended_n = 0;
	pthread_mutex_unlock(&st.lock);
	while (n--)
		pthread_join(t[n], NULL);
}

static void *worker(void *arg)
{
	int op = (int)(long)arg, ret = -1;
	struct ms_chip c = { .size = 0 };
	char msg[512] = "";
	int opened = !web_device_open || !web_device_open();

	if (!opened) {
		snprintf(msg, sizeof(msg), "No programmer, or no access to it.");
		op = OP_NONE;
	}
	switch (op) {
	case OP_DETECT:    ret = ms_detect(&c, msg, sizeof(msg)); break;
	case OP_READ:      ret = op_read(&c, msg, sizeof(msg)); break;
	case OP_ERASE:     ms_expect(want_chip, want_size);
			   ret = ms_erase(&c, on_progress, NULL, msg, sizeof(msg)); break;
	case OP_WRITE:     ms_expect(want_chip, want_size);
			   ret = ms_write_file(&c, upload, on_progress, NULL, msg, sizeof(msg)); break;
	case OP_REGISTERS: ret = ms_registers(&c, msg, sizeof(msg)); break;
	case OP_UNPROTECT: ms_expect(want_chip, want_size);
			   ret = ms_unprotect(&c, msg, sizeof(msg)); break;
	case OP_SELFTEST: {
		char saved[160];

		ms_expect(want_chip, want_size);		/* it erases the first block */
		ret = ms_selftest(&c, web_dir, saved, sizeof(saved), msg, sizeof(msg));
		if (saved[0]) {					/* the block that did not come back */
			pthread_mutex_lock(&st.lock);
			snprintf(st.file, sizeof(st.file), "%s", saved);
			pthread_mutex_unlock(&st.lock);
		}
		break;
	}
	}
	if (opened && web_device_close)
		web_device_close();
	keep_chip(&c, op == OP_DETECT || op == OP_REGISTERS || op == OP_UNPROTECT || op == OP_SELFTEST);
	set_msg("%s", msg);
	if (upload)
		fclose(upload);
	upload = NULL;
	pthread_mutex_lock(&st.lock);
	st.ok = ret == 0;
	st.busy = 0;
	thread_ended();
	pthread_mutex_unlock(&st.lock);
	return NULL;
}

/* start an operation (with the file to write, which it closes; for the
   chip confirmed, "" for any); 0 and its number in *seq, or -1 when one is
   running (then nothing is started and body is still the caller's) */
static int start(int op, FILE *body, const char *chip, long long size, unsigned int *seq)
{
	pthread_t t;

	pthread_mutex_lock(&st.lock);
	if (st.busy) {
		pthread_mutex_unlock(&st.lock);
		return -1;
	}
	snprintf(want_chip, sizeof(want_chip), "%s", chip ? chip : "");
	want_size = size;
	*seq = ++st.seq;
	upload = body;
	if (body)
		st.uploading = 0;		/* the upload is the operation's now */
	st.busy = 1;
	st.local = 0;
	st.op = op;
	st.ok = -1;
	st.done = st.total = 0;
	st.phase[0] = 0;
	st.message[0] = 0;
	if (op == OP_DETECT || op == OP_REGISTERS || op == OP_UNPROTECT || op == OP_SELFTEST)
		st.sr_valid = 0;
	pthread_mutex_unlock(&st.lock);
	if (pthread_create(&t, NULL, worker, (void *)(long)op)) {
		pthread_mutex_lock(&st.lock);
		upload = NULL;
		st.busy = 0;
		pthread_mutex_unlock(&st.lock);
		return -1;
	}
	return 0;
}

/* ---- the app's own operations (Android): the same slot as the page's ---- */

int web_local_begin(int op)
{
	pthread_mutex_lock(&st.lock);
	if (st.busy || st.uploading) {
		pthread_mutex_unlock(&st.lock);
		return -1;
	}
	st.seq++;
	st.busy = 1;
	st.local = 1;
	st.op = op;
	st.ok = -1;
	st.done = st.total = 0;
	st.phase[0] = 0;
	st.message[0] = 0;
	if (op == OP_DETECT || op == OP_REGISTERS || op == OP_UNPROTECT || op == OP_SELFTEST || op == OP_REGISTER)
		st.sr_valid = 0;
	pthread_mutex_unlock(&st.lock);
	return 0;
}

void web_local_progress(const char *phase, unsigned long long done, unsigned long long total)
{
	set_progress(phase, done, total);
}

void web_local_end(const struct ms_chip *c, int ok, const char *msg, const char *file)
{
	pthread_mutex_lock(&st.lock);
	int op = st.op;
	pthread_mutex_unlock(&st.lock);

	keep_chip(c, op == OP_DETECT || op == OP_REGISTERS || op == OP_UNPROTECT || op == OP_SELFTEST || op == OP_REGISTER);
	set_msg("%s", msg);
	pthread_mutex_lock(&st.lock);
	if (file && file[0])
		snprintf(st.file, sizeof(st.file), "%s", file);
	st.ok = ok;
	st.busy = 0;
	pthread_mutex_unlock(&st.lock);
}

/* a dump the app keeps in the dump directory: the page links it and shows it in hex */
void web_show_dump(const char *name)
{
	pthread_mutex_lock(&st.lock);
	snprintf(st.file, sizeof(st.file), "%s", name);
	pthread_mutex_unlock(&st.lock);
}

/* the state of the app's camex tunnel, for the page ("" none) */
void web_set_tunnel(const char *text)
{
	pthread_mutex_lock(&st.lock);
	snprintf(st.tunnel, sizeof(st.tunnel), "%s", text);
	pthread_mutex_unlock(&st.lock);
}

/* ---- HTTP ---- */

#ifdef WEB_PAGE_H		/* the Android build's own copy, by its full path */
#include WEB_PAGE_H
#else
#include "web_page.h"
#endif

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

size_t web_state_json(char *body, size_t size)
{
	char msg[1100], prg[64], chip[128], file[256], warn[640], sr[32] = "null", tun[400];

	pthread_mutex_lock(&st.lock);
	json_str(msg, sizeof(msg), st.message);
	json_str(prg, sizeof(prg), st.programmer);
	json_str(chip, sizeof(chip), st.chip);
	json_str(file, sizeof(file), st.file);
	json_str(warn, sizeof(warn), st.chip[0] ? st.warning : "");
	json_str(tun, sizeof(tun), st.tunnel);
	if (st.sr_valid)
		snprintf(sr, sizeof(sr), "[%u,%u,%u]", st.sr[0], st.sr[1], st.sr[2]);
	int n = snprintf(body, size,
			 "{\"busy\":%d,\"local\":%s,\"op\":\"%s\",\"ok\":%s,\"phase\":\"%s\",\"done\":%llu,\"total\":%llu,"
			 "\"message\":\"%s\",\"programmer\":\"%s\",\"chip\":\"%s\",\"size\":%lld,\"voltage\":\"%s\","
			 "\"warning\":\"%s\",\"sr\":%s,\"sr_macronix\":%s,\"file\":\"%s\","
			 "\"seq\":%u,\"version\":\"%s\",\"tunnel\":\"%s\"}",
			 st.busy, st.local ? "true" : "false", op_names[st.op], st.ok < 0 ? "null" : st.ok ? "true" : "false", st.phase,
			 st.done, st.total, msg, prg, chip, st.size, st.chip[0] ? ms_voltage_name(st.voltage) : "",
			 warn, sr, st.sr_valid && st.sr_macronix ? "true" : "false", file, st.seq, ms_version, tun);
	pthread_mutex_unlock(&st.lock);
	return n < 0 ? 0 : (size_t)n < size ? (size_t)n : size - 1;
}

static void reply_state(int fd)
{
	char body[3072];
	size_t n = web_state_json(body, sizeof(body));

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

/* SHA-256 (FIPS 180-4), for the password check only */
static const unsigned int sha_k[64] = {
	0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
	0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
	0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
	0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
	0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
	0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
	0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
	0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_block(unsigned int h[8], const unsigned char *b)
{
	unsigned int w[64], v[8];

	for (int i = 0; i < 16; i++)
		w[i] = (unsigned int)b[4 * i] << 24 | b[4 * i + 1] << 16 | b[4 * i + 2] << 8 | b[4 * i + 3];
	for (int i = 16; i < 64; i++) {
		unsigned int s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
		unsigned int s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);

		w[i] = w[i - 16] + s0 + w[i - 7] + s1;
	}
	memcpy(v, h, sizeof(v));
	for (int i = 0; i < 64; i++) {
		unsigned int t1 = v[7] + (ROR(v[4], 6) ^ ROR(v[4], 11) ^ ROR(v[4], 25)) +
				  ((v[4] & v[5]) ^ (~v[4] & v[6])) + sha_k[i] + w[i];
		unsigned int t2 = (ROR(v[0], 2) ^ ROR(v[0], 13) ^ ROR(v[0], 22)) +
				  ((v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]));

		memmove(v + 1, v, 7 * sizeof(v[0]));
		v[4] += t1;
		v[0] = t1 + t2;
	}
	for (int i = 0; i < 8; i++)
		h[i] += v[i];
}

static void sha256(const void *data, size_t n, unsigned char out[32])
{
	unsigned int h[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
			      0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
	const unsigned char *p = data;
	unsigned char b[64];
	size_t left = n;

	for (; left >= 64; left -= 64, p += 64)
		sha256_block(h, p);
	memset(b, 0, sizeof(b));
	memcpy(b, p, left);
	b[left] = 0x80;
	if (left >= 56) {
		sha256_block(h, b);
		memset(b, 0, sizeof(b));
	}
	for (int i = 0; i < 8; i++)
		b[63 - i] = (unsigned char)((unsigned long long)n * 8 >> (8 * i));
	sha256_block(h, b);
	for (int i = 0; i < 32; i++)
		out[i] = h[i / 4] >> (24 - 8 * (i % 4));
}

/* the two passwords match, in a time that does not depend on where they differ:
   the SHA-256 of both, compared whole */
static int password_equal(const char *given, const char *want)
{
	unsigned char a[32], b[32], d = 0;

	sha256(given, strlen(given), a);
	sha256(want, strlen(want), b);
	for (int i = 0; i < 32; i++)
		d |= a[i] ^ b[i];
	return d == 0;
}

/* the password to ask for: an empty one is none, the page is open */
static const char *password_of(const char *p)
{
	return p && *p ? p : NULL;
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
	return h && password_equal(h + 1, web_password);
}

/* The Host of the request: an IP address, localhost, or this machine's name.
   A page of another site that resolves its own name to this address (DNS
   rebinding) sends its own name, and is refused. */
/* host is this machine's name me, or me.local / me.domain; not when me is
   localhost (Android): localhost.<a site> would let that site in by DNS rebinding */
static int name_ok(const char *host, const char *me)
{
	size_t m = strlen(me);

	if (!m || !strcasecmp(me, "localhost") || !strncasecmp(me, "localhost.", 10))
		return 0;
	return !strcasecmp(host, me) || (!strncasecmp(host, me, m) && host[m] == '.');
}

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
		if (name_ok(host, me))
			return 1;
	}
	pthread_mutex_lock(&st.lock);
	size_t d = strlen(web_host_domain);
	int under = d && n > d + 1 && host[n - d - 1] == '.' && !strcasecmp(host + n - d, web_host_domain);
	pthread_mutex_unlock(&st.lock);
	return under;			/* client.server: the tunnel's nginx */
}

/* a dump name made by Read (flash-*.bin) or the block a self-test saved
   (selftest-*.bin): no path */
static int dump_ok(const char *name)
{
	size_t n = strlen(name);
	int read = n > 10 && !strncmp(name, "flash-", 6), saved = n > 13 && !strncmp(name, "selftest-", 9);

	return (read || saved) && !strcmp(name + n - 4, ".bin") && !strchr(name, '/') && !strstr(name, "..");
}

/* Range: bytes=A-B or bytes=A- of a file of size bytes: 1 and the part in
   *from, *to; 0 no range (the whole file); -1 a range past the end */
static int dump_range(const char *range, long long size, long long *from, long long *to)
{
	long long a, b = -1;
	char *e;

	if (!range)
		return 0;
	range += 2 + 6;				/* "\r\nRange:" */
	while (*range == ' ' || *range == '\t')
		range++;
	if (strncmp(range, "bytes=", 6) || range[6] < '0' || range[6] > '9')
		return 0;			/* no unit we know, or a suffix range: the whole file */
	a = strtoll(range + 6, &e, 10);
	if (*e++ != '-')
		return 0;
	if (*e >= '0' && *e <= '9')
		b = strtoll(e, &e, 10);
	while (*e == ' ' || *e == '\t')
		e++;
	if (*e != '\r' || (b >= 0 && b < a))
		return 0;			/* several ranges or a bad one: the whole file */
	if (a >= size)
		return -1;
	*from = a;
	*to = b < 0 || b >= size ? size - 1 : b;
	return 1;
}

static void send_dump(int fd, const char *name, const char *range)
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
	long long from = 0, to = (long long)sb.st_size - 1;
	int part = dump_range(range, sb.st_size, &from, &to);
	int k;

	if (part < 0) {
		k = snprintf(h, sizeof(h), "HTTP/1.0 416 Range Not Satisfiable\r\nContent-Range: bytes */%lld\r\n"
			     "Content-Length: 0\r\nConnection: close\r\n\r\n", (long long)sb.st_size);
		send_all(fd, h, k);
		fclose(f);
		return;
	}
	/* nothing is read from f yet: the offset of its descriptor is where it starts
	   (lseek, as 32-bit Android before 7.0 has no fseeko with a 64-bit off_t) */
	if (part > 0 && lseek(fileno(f), (off_t)from, SEEK_SET) < 0) {
		fclose(f);
		reply_text(fd, "500 Internal Server Error", "Cannot read the dump.\n");
		return;
	}
	if (part > 0)
		k = snprintf(h, sizeof(h), "HTTP/1.0 206 Partial Content\r\nContent-Type: application/octet-stream\r\n"
			     "Content-Range: bytes %lld-%lld/%lld\r\nContent-Length: %lld\r\nConnection: close\r\n\r\n",
			     from, to, (long long)sb.st_size, to - from + 1);
	else
		k = snprintf(h, sizeof(h), "HTTP/1.0 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Length: %lld\r\n"
			     "Content-Disposition: attachment; filename=\"%s\"\r\nAccept-Ranges: bytes\r\nConnection: close\r\n\r\n",
			     (long long)sb.st_size, name);
	if (!send_all(fd, h, k)) {
		char buf[16384];
		long long left = to - from + 1;
		size_t n;

		while (left > 0 && (n = fread(buf, 1, left < (long long)sizeof(buf) ? (size_t)left : sizeof(buf), f)) > 0 &&
		       !send_all(fd, buf, n))
			left -= n;
	}
	fclose(f);
}

/* the upload slot is free again (the upload failed or was refused) */
static void upload_done(void)
{
	pthread_mutex_lock(&st.lock);
	st.uploading = 0;
	pthread_mutex_unlock(&st.lock);
}

/* the file stopped coming: a failed Write in the state, so the page (any page)
   says so instead of the last result; nothing was written */
static void upload_cut(unsigned long long have, unsigned long long len, int timeout)
{
	pthread_mutex_lock(&st.lock);
	st.uploading = 0;
	if (!st.busy) {
		st.seq++;
		st.op = OP_WRITE;
		st.ok = 0;
		st.phase[0] = 0;
		st.done = st.total = 0;
		snprintf(st.message, sizeof(st.message),
			 "The file stopped coming after %llu of %llu bytes (%s): nothing was written. Press Write again.",
			 have, len, timeout ? "no data for 30 s" : "the connection was closed");
	}
	pthread_mutex_unlock(&st.lock);
}

/* the file to write goes to the disk, not to memory: a SPI NAND image is up
   to 512 MB. It has no name, it goes when it is closed. */
static FILE *upload_file(void)
{
	static const char tail[] = "/.microsnander-write-XXXXXX";
	char *path = malloc(strlen(web_dir) + sizeof(tail));	/* -o can be any length */
	int fd, gone;
	FILE *f;

	if (!path)
		return NULL;
	strcpy(path, web_dir);
	strcat(path, tail);
	if ((fd = mkstemp(path)) < 0) {
		free(path);
		return NULL;
	}
	gone = !unlink(path);
	free(path);
	if (!gone) {				/* a name left would keep the image there */
		close(fd);
		return NULL;
	}
	if (!(f = fdopen(fd, "w+b")))
		close(fd);
	return f;
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
	if (!web_any_host && !host_ok(head)) {
		reply_text(fd, "403 Forbidden", "Use the address of this machine.\n");
		return;
	}
	if (!authorized(head)) {
		reply(fd, "401 Unauthorized", "text/plain", "WWW-Authenticate: Basic realm=\"microsnander\", charset=\"UTF-8\"\r\n", "Password needed.\n", 17);
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
		/* one Range among the headers, not in what came after them; two: the whole file */
		const char *rg = find_header(head, "Range:"), *rg2 = rg ? find_header(rg + 2, "Range:") : NULL;

		if (rg && (rg >= end || (rg2 && rg2 < end)))
			rg = NULL;
		send_dump(fd, path + 7, rg);
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
		{ "/api/selftest", OP_SELFTEST },
	};
	int op = 0;

	for (unsigned i = 0; i < sizeof(routes) / sizeof(routes[0]); i++)
		if (!strcmp(path, routes[i].path))
			op = routes[i].op;
	if (!op) {
		reply_text(fd, "404 Not Found", "Not found.\n");
		return;
	}
	/* What changes the chip names the chip the user confirmed: "X-MicroSNANDer-Chip:
	   W25Q128BV;16777216"; without it the request is refused before an upload
	   takes space, and another chip in the socket by then is refused too */
	char want[64] = "";
	long long want_sz = 0;
	const char *h = find_header(head, "X-MicroSNANDer-Chip:");
	int confirmed = op == OP_ERASE || op == OP_WRITE || op == OP_SELFTEST || op == OP_UNPROTECT;

	if (h && confirmed) {
		size_t k = 0;

		for (h += 2 + strlen("X-MicroSNANDer-Chip:"); *h == ' '; h++)
			;
		while (*h && *h != ';' && *h != '\r' && k < sizeof(want) - 1)
			want[k++] = *h++;
		want[k] = 0;
		want_sz = *h == ';' ? strtoll(h + 1, NULL, 10) : 0;
	}
	if (confirmed && (!want[0] || want_sz <= 0)) {		/* the page always names the chip */
		reply_text(fd, "400 Bad Request", "This operation needs the chip confirmed: X-MicroSNANDer-Chip: NAME;SIZE.\n");
		return;
	}
	/* an upload holds the slot of the operation it brings: another upload is
	   refused before it takes space, another operation before it runs */
	pthread_mutex_lock(&st.lock);
	int busy = st.busy || st.uploading;
	if (!busy && op == OP_WRITE)
		st.uploading = 1;
	pthread_mutex_unlock(&st.lock);
	if (busy) {
		reply_text(fd, "409 Conflict", "An operation is running.\n");
		return;
	}
	FILE *body = NULL;
	if (op == OP_WRITE) {
		char part[16384];
		size_t have = got - (end + 4 - head);

		if (!clen || clen > WEB_MAX_BODY || have > clen) {
			upload_done();
			reply_text(fd, "413 Payload Too Large", "No file, or too large.\n");
			return;
		}
		if (!(body = upload_file())) {
			upload_done();
			reply_text(fd, "500 Internal Server Error", "Cannot store the file in the dump directory.\n");
			return;
		}
		int bad = fwrite(end + 4, 1, have, body) != have;
		while (!bad && have < clen) {
			ssize_t r = recv(fd, part, clen - have < sizeof(part) ? clen - have : sizeof(part), 0);

			if (r <= 0) {
				int timeout = r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK);

				fclose(body);
				upload_cut(have, clen, timeout);
				if (timeout)	/* the browser may still be there to read it */
					reply_text(fd, "408 Request Timeout", "The file stopped coming: nothing was written.\n");
				return;
			}
			bad = fwrite(part, 1, r, body) != (size_t)r;
			have += r;
		}
		if (bad || fflush(body)) {
			fclose(body);
			upload_done();
			reply_text(fd, "507 Insufficient Storage", "Cannot store the file in the dump directory.\n");
			return;
		}
	}
	unsigned int seq;

	if (start(op, body, want, want_sz, &seq)) {
		if (body) {
			fclose(body);
			upload_done();
		}
		reply_text(fd, "409 Conflict", "An operation is running.\n");
		return;
	}
	char started[32];

	snprintf(started, sizeof(started), "Started %u.\n", seq);	/* the number in /api/state "seq" */
	reply_text(fd, "202 Accepted", started);
}

static int connections;
static int conn_fd[WEB_MAX_CONN];		/* the open connections, for web_stop; -1 free */

static void *conn_thread(void *arg)
{
	int c = (int)(long)arg;

	handle(c);
	pthread_mutex_lock(&st.lock);
	for (int i = 0; i < WEB_MAX_CONN; i++)
		if (conn_fd[i] == c)
			conn_fd[i] = -1;
	close(c);
	connections--;
	thread_ended();
	pthread_mutex_unlock(&st.lock);
	return NULL;
}

static int web_sock = -1;
static volatile int web_stopping;

void web_stop(void)
{
	pthread_mutex_lock(&st.lock);
	web_stopping = 1;
	if (web_sock >= 0) {
		shutdown(web_sock, SHUT_RDWR);	/* accept() returns */
		for (int i = 0; i < WEB_MAX_CONN; i++)
			if (conn_fd[i] >= 0)
				shutdown(conn_fd[i], SHUT_RDWR);	/* their requests end */
	}
	pthread_mutex_unlock(&st.lock);
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
	web_password = password_of(password);
	pthread_mutex_lock(&st.lock);
	for (int i = 0; i < WEB_MAX_CONN; i++)
		conn_fd[i] = -1;
	pthread_mutex_unlock(&st.lock);
	web_dir = dir && *dir ? dir : ".";
	web_addr = addr;
	web_len = len;

	s = socket(AF_INET, SOCK_STREAM, 0);
	setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	if (s < 0 || bind(s, (struct sockaddr *)&sa, sizeof(sa)) || listen(s, 8)) {
		printf("Cannot listen on %s:%ld: %s\n", host, port, strerror(errno));
		if (s >= 0)
			close(s);
		return 1;
	}
	pthread_mutex_lock(&st.lock);
	web_sock = s;
	int stopped = web_stopping;
	pthread_mutex_unlock(&st.lock);
	socklen_t sl = sizeof(sa);
	getsockname(s, (struct sockaddr *)&sa, &sl);
	printf("Web mode on http://%s:%d/%s, dumps in %s\n", host, ntohs(sa.sin_port),
	       web_password ? " (password)" : "", web_dir);
	if (!web_password && (ntohl(sa.sin_addr.s_addr) >> 24) != 127)
		printf("No password (-K, MICROSNANDER_WEB_PASSWORD): anyone who reaches this port can erase or write the chip.\n");
	fflush(stdout);

	/* a thread for each connection, so a slow client does not hold the others */
	while (!stopped) {
		int c = accept(s, NULL, NULL);
		pthread_t t;

		reap_threads();
		if (web_stopping) {
			if (c >= 0)
				close(c);
			break;
		}
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
		if (!many) {
			connections++;
			for (int i = 0; i < WEB_MAX_CONN; i++)
				if (conn_fd[i] < 0) {
					conn_fd[i] = c;
					break;
				}
		}
		pthread_mutex_unlock(&st.lock);
		if (many || pthread_create(&t, NULL, conn_thread, (void *)(long)c)) {
			if (!many) {
				pthread_mutex_lock(&st.lock);
				connections--;
				for (int i = 0; i < WEB_MAX_CONN; i++)
					if (conn_fd[i] == c)
						conn_fd[i] = -1;
				pthread_mutex_unlock(&st.lock);
			}
			reply_text(c, "503 Service Unavailable", "Too many connections.\n");
			close(c);
		}
	}
	/* web_stop: the connections are cut; an operation of the page that runs
	   ends by itself, waited for here, so nothing uses the password or the
	   dump directory once this returns (the caller may free them) */
	pthread_mutex_lock(&st.lock);
	web_sock = -1;
	pthread_mutex_unlock(&st.lock);
	close(s);
	for (;;) {
		pthread_mutex_lock(&st.lock);
		int left = connections || (st.busy && !st.local);
		pthread_mutex_unlock(&st.lock);
		reap_threads();
		if (!left)
			break;
		usleep(100000);
	}
	pthread_mutex_lock(&st.lock);
	web_stopping = 0;
	pthread_mutex_unlock(&st.lock);
	return 0;
}

void web_allow_host_domain(const char *domain)
{
	pthread_mutex_lock(&st.lock);
	snprintf(web_host_domain, sizeof(web_host_domain), "%s", domain ? domain : "");
	pthread_mutex_unlock(&st.lock);
}

void web_allow_any_host(int on)
{
	web_any_host = on;
}

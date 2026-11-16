/*
 * web.h — the web mode (-W): a page with Detect, Read, Erase, Write and
 * Registers for the programmer that is connected.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef __WEB_H__
#define __WEB_H__

#include <stddef.h>
#include "ops.h"

enum { OP_NONE, OP_DETECT, OP_READ, OP_ERASE, OP_WRITE, OP_REGISTERS, OP_UNPROTECT, OP_SELFTEST, OP_REGISTER };

/* listen: "[address:]port"; password: NULL for none; dir: where dumps go;
   addr/len: the range a read takes (len 0: the whole chip). Returns when
   the server cannot run (the exit code) or after web_stop (0). */
int web_serve(const char *listen, const char *password, const char *dir,
	      unsigned long long addr, unsigned long long len);

/* ---- for the Android app ---- */

/* web_serve returns; an operation that runs ends by itself */
void web_stop(void);

/* a request with any Host: is taken (the app always has a password, and a
   tunnel may come by a name) */
void web_allow_any_host(int on);

/* around each operation of the page: open the programmer (0, or -1: none or
   no access, the operation fails) and close it; NULL: nothing to do */
extern int (*web_device_open)(void);
extern void (*web_device_close)(void);

/* the app's own operation (OP_*) in the slot of the page's: -1 when one is
   running (then nothing is taken), 0 and the slot is the app's until
   web_local_end; the page shows it as any other */
int web_local_begin(int op);
void web_local_progress(const char *phase, unsigned long long done, unsigned long long total);
void web_local_end(const struct ms_chip *c, int ok, const char *msg, const char *file);

/* a dump the app put in the dump directory: the page shows it as the last one */
void web_show_dump(const char *name);

/* the state the page reads at /api/state, as JSON; its length */
size_t web_state_json(char *buf, size_t size);

#endif /* __WEB_H__ */

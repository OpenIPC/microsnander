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

/* listen: "[address:]port"; password: NULL for none; dir: where dumps go;
   addr/len: the range a read takes (len 0: the whole chip). Returns when
   the server cannot run; the exit code. */
int web_serve(const char *listen, const char *password, const char *dir,
	      unsigned long long addr, unsigned long long len);

#endif /* __WEB_H__ */

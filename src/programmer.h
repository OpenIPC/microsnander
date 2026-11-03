/*
 * programmer.h — the USB programmers and the choice of one.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef __PROGRAMMER_H__
#define __PROGRAMMER_H__

#include <stdbool.h>

struct programmer {
	const char *key;		/* -P name */
	const char *name;
	unsigned short vid, pid[2];	/* USB IDs; 0: unused */
	int (*init)(void);
	int (*shutdown)(void);
	int (*send)(unsigned int writecnt, unsigned int readcnt, const unsigned char *writearr, unsigned char *readarr);
	int (*cs)(bool low);
};

extern const struct programmer *programmer;	/* the one in use */

int programmer_select(const char *name);	/* -P; -1: unknown name */
const char *programmer_names(void);
int programmer_init(void);			/* -1: none found or init failed; -2: several found, no -P */
int programmer_shutdown(void);

#endif /* __PROGRAMMER_H__ */

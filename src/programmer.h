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

struct flash_cmd;

struct programmer {
	const char *key;		/* -P name */
	const char *name;
	unsigned short vid, pid[3];	/* USB IDs; 0: unused */
	int (*init)(void);
	int (*shutdown)(void);
	int (*send)(unsigned int writecnt, unsigned int readcnt, const unsigned char *writearr, unsigned char *readarr);
	int (*cs)(bool low);
	bool clock;			/* the SPI clock can be set (-S) */
	/* a programmer that runs the SPI in its own firmware: sets up the
	   flash operations itself (no send/cs); returns the chip size or -1 */
	long (*flash_init)(struct flash_cmd *cmd);
};

extern const struct programmer *programmer;	/* the one in use */

int programmer_select(const char *name);	/* -P; -1: unknown name */
const char *programmer_names(void);
/* need_clock: -S was given. -1: none found or init failed; -2: refused
   (several found and no -P, or no settable clock), the message printed */
int programmer_init(bool need_clock);
int programmer_shutdown(void);

#endif /* __PROGRAMMER_H__ */

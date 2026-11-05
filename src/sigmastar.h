/*
 * sigmastar.h — the SigmaStar ISP (-m sigmastar) and the I2C bus scan (-q).
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef __MS_SIGMASTAR_H__
#define __MS_SIGMASTAR_H__

#include <stdbool.h>

struct programmer;

int sigmastar_mode(const char *name);		/* -m; -1: unknown mode */
bool sigmastar_on(void);
int sigmastar_set_speed(const char *name);	/* -S in this mode: the I2C clock */
const char *sigmastar_speed_list(void);

/* the open programmer p, with its send and cs going to the ISP bridge;
   NULL (the message printed) when there is no bridge or no I2C */
const struct programmer *sigmastar_attach(const struct programmer *p);

/* -q: which I2C addresses answer on the bus of the open programmer p */
int i2c_scan(const struct programmer *p);

#endif /* __MS_SIGMASTAR_H__ */

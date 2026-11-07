/*
 * Copyright (C) 2021 McMCC <mcmcc@mail.ru>
 * timer.h
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */
#ifndef __TIMER_H__
#define __TIMER_H__

#include <signal.h>

extern int timer_quiet;		/* main.c prints the time and the progress of the parts */

/* set by Ctrl+C (main.c): an operation stops between two steps, so the chip
   is left as after a complete command (not in its 4-byte address mode) */
extern volatile sig_atomic_t ms_stop;

void timer_start(void);
void timer_end(void);
int timer_progress(void);

#endif /* __TIMER_H__ */
/* End of [timer.h] package */

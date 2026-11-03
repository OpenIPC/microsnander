/*
 * ezp.h — EZP2019, EZP2019+ and EZP2023 programmers.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef __EZP_H__
#define __EZP_H__

struct flash_cmd;

int ezp_init(void);
int ezp_shutdown(void);
long ezp_flash_init(struct flash_cmd *cmd);	/* the chip size, or -1 */

#endif /* __EZP_H__ */

/*
 * Copyright (C) 2018-2021 McMCC <mcmcc@mail.ru>
 * flashcmd_api.c
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

#include <stdio.h>
#include "flashcmd_api.h"
#include "programmer.h"
#include "voltage.h"

const char *flash_chip_name;

/* the chip's voltage and the board's lines, after the chip is found */
static long warn(long flen)
{
	char w[320];

	if (flen > 0 && ms_voltage_warning(programmer ? programmer->name : NULL, flash_chip_name, w, sizeof(w)))
		printf("Warning: %s\n", w);
	return flen;
}

long flash_cmd_init(struct flash_cmd *cmd)
{
	long flen = -1;

	cmd->lock = -1;
	if (programmer && programmer->flash_init) {
		cmd->kind = FLASH_FIRMWARE;
		if ((flen = programmer->flash_init(cmd)) <= 0)
			printf("\nFlash not found!!!!\n\n");
		return warn(flen);
	}

	if ((flen = snand_init()) > 0) {
		cmd->kind = FLASH_NAND;
		cmd->lock = snand_probe_lock;
		cmd->flash_erase = snand_erase;
		cmd->flash_write = snand_write;
		cmd->flash_read  = snand_read;
	} else if ((flen = snor_init()) > 0) {
		cmd->kind = FLASH_NOR;
		cmd->flash_unprotect = snor_unprotect;
		cmd->flash_erase = snor_erase;
		cmd->flash_write = snor_write;
		cmd->flash_read  = snor_read;
	} else
		printf("\nFlash not found!!!!\n\n");

	return warn(flen);
}

void support_flash_list(void)
{
	support_snand_list();
	printf("\n");
	support_snor_list();
}
/* End of [flashcmd.c] package */

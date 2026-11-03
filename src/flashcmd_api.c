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

const char *flash_chip_name;

long flash_cmd_init(struct flash_cmd *cmd)
{
	long flen = -1;

	if (programmer && programmer->flash_init) {
		if ((flen = programmer->flash_init(cmd)) <= 0)
			printf("\nFlash not found!!!!\n\n");
		return flen;
	}

	if ((flen = snand_init()) > 0) {
		cmd->flash_erase = snand_erase;
		cmd->flash_write = snand_write;
		cmd->flash_read  = snand_read;
	} else if ((flen = snor_init()) > 0) {
		cmd->flash_unprotect = snor_unprotect;
		cmd->flash_erase = snor_erase;
		cmd->flash_write = snor_write;
		cmd->flash_read  = snor_read;
	} else
		printf("\nFlash not found!!!!\n\n");

	return flen;
}

void support_flash_list(void)
{
	support_snand_list();
	printf("\n");
	support_snor_list();
}
/* End of [flashcmd.c] package */

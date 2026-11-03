/*
 * voltage.h — the supply voltage of the chip found, from its name.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef __MS_VOLTAGE_H__
#define __MS_VOLTAGE_H__

#include <stddef.h>

enum {
	MS_VOLTAGE_33,		/* 3.3 V, or not known to be another one */
	MS_VOLTAGE_18,		/* 1.8 V: needs a 1.8 V adapter */
	MS_VOLTAGE_WIDE,	/* 1.65-3.6 V: works at 3.3 V */
};

int ms_voltage(const char *chip);
const char *ms_voltage_name(int voltage);	/* "3.3", "1.8", "1.65-3.6" */

/* what to tell before working with the chip: a 1.8 V chip, the 5 V lines
   of the black CH341A board; 1 if buf has something */
int ms_voltage_warning(const char *programmer, const char *chip, char *buf, size_t n);

#endif /* __MS_VOLTAGE_H__ */

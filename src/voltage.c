/*
 * voltage.c — the supply voltage of the chip found, from its name.
 *
 * The programmer does not measure the voltage on its lines, so the chip
 * says it: the series below are 1.8 V parts (1.65-1.95 V or 1.65-2.0 V)
 * or take 1.65-3.6 V, by their datasheets. A chip not listed here is taken
 * for a 3.3 V one.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include <fnmatch.h>
#include <stdio.h>
#include <string.h>
#include "voltage.h"

struct series {
	const char *glob;		/* the chip name; a SPI NAND name without its maker */
	int voltage;
};

static const struct series series[] = {
	/* SPI NOR */
	{ "GD25L*",		MS_VOLTAGE_18 },	/* GigaDevice GD25LQ, GD25LB, GD25LE */
	{ "GD25W*",		MS_VOLTAGE_WIDE },	/* GD25WD, GD25WQ */
	{ "MX25U*",		MS_VOLTAGE_18 },	/* Macronix */
	{ "W25Q*[BDEFJN]W*",	MS_VOLTAGE_18 },	/* Winbond W25Q..BW, DW, EW, FW, JW, NW */
	{ "N25Q*A11",		MS_VOLTAGE_18 },	/* Micron; N25Q..A13 is 3 V */
	{ "MT25QU*",		MS_VOLTAGE_18 },	/* Micron; MT25QL is 3 V */
	{ "XM25QU*",		MS_VOLTAGE_18 },	/* XMC */
	{ "XM25LU*",		MS_VOLTAGE_18 },
	{ "XM25QW*",		MS_VOLTAGE_WIDE },
	{ "ZB25LQ*",		MS_VOLTAGE_18 },	/* Zbit */
	{ "BY25Q*L",		MS_VOLTAGE_18 },	/* Boya */
	{ "IS25WP*",		MS_VOLTAGE_18 },	/* ISSI */
	{ "FM25W*",		MS_VOLTAGE_WIDE },	/* Fudan */
	{ "FM25M*",		MS_VOLTAGE_18 },	/* Fudan FM25M4AA, FM25M64C */
	{ "EN25S*",		MS_VOLTAGE_18 },	/* Eon EN25S, EN25SE */
	{ "IS25WQ*",		MS_VOLTAGE_18 },	/* ISSI */
	{ "AT25DL*",		MS_VOLTAGE_18 },	/* Adesto */
	{ "AT25DF021A",		MS_VOLTAGE_WIDE },
	{ "SST25WF*",		MS_VOLTAGE_18 },	/* Microchip SST25WF */
	{ "MX25R*",		MS_VOLTAGE_WIDE },	/* Macronix MX25R (low power) */
	{ "DS25M*",		MS_VOLTAGE_18 },	/* Dosilicon */
	{ "XT25Q*",		MS_VOLTAGE_18 },	/* XTX; XT25F is 3.3 V */
	{ "XT25W*",		MS_VOLTAGE_WIDE },
	{ "PY25F*L*",		MS_VOLTAGE_18 },	/* Puya; PY25Q..H is 3.3 V */
	/* SPI NAND */
	{ "GD5F*[QM][0-9]R*",	MS_VOLTAGE_18 },	/* GigaDevice: ..U.. is 3.3 V, ..R.. 1.8 V */
	{ "W25N*W",		MS_VOLTAGE_18 },	/* Winbond W25N..GW, KW */
	{ "MX35UF*",		MS_VOLTAGE_18 },	/* Macronix; MX35LF is 3.3 V */
	{ "MT29F*ABB",		MS_VOLTAGE_18 },	/* Micron; ..ABA is 3.3 V */
	{ "F50D*",		MS_VOLTAGE_18 },	/* ESMT; F50L is 3.3 V */
	{ "DS35M*",		MS_VOLTAGE_18 },	/* Dosilicon; DS35Q is 3.3 V */
	{ "TC58CY*",		MS_VOLTAGE_18 },	/* Kioxia (Toshiba); TC58CV is 3.3 V */
	{ "TH58CY*",		MS_VOLTAGE_18 },
	{ "MT29F*ADB*",		MS_VOLTAGE_18 },	/* Micron 8 Gbit; ..ADA.. is 3.3 V */
	{ "EM78*",		MS_VOLTAGE_18 },	/* Etron; EM73 is 3.3 V */
	{ "XT26Q*",		MS_VOLTAGE_18 },	/* XTX; XT26G is 3.3 V */
	{ "FM25LG*",		MS_VOLTAGE_18 },	/* Fudan; FM25G is 3.3 V */
};

int ms_voltage(const char *chip)
{
	const char *s;

	if (!chip)
		return MS_VOLTAGE_33;
	if ((s = strrchr(chip, ' ')))		/* "WINBOND W25N01GW" */
		chip = s + 1;
	for (size_t i = 0; i < sizeof(series) / sizeof(series[0]); i++)
		if (!fnmatch(series[i].glob, chip, 0))
			return series[i].voltage;
	return MS_VOLTAGE_33;
}

const char *ms_voltage_name(int voltage)
{
	return voltage == MS_VOLTAGE_18 ? "1.8" : voltage == MS_VOLTAGE_WIDE ? "1.65-3.6" : "3.3";
}

int ms_voltage_warning(const char *programmer, const char *chip, char *buf, size_t n)
{
	size_t k = 0;

	buf[0] = 0;
	if (chip && chip[0] && ms_voltage(chip) == MS_VOLTAGE_18)
		k += snprintf(buf, n, "%s is a 1.8 V chip: connect it through a 1.8 V adapter, "
			      "at 3.3 V the reads are not reliable and the chip can be damaged.", chip);
	if (programmer && !strcmp(programmer, "CH341A") && k < n)
		k += snprintf(buf + k, n - k, "%sThe black CH341A board puts 5 V on the chip lines "
			      "unless it is modified for 3.3 V.", k ? " " : "");
	return buf[0] != 0;
}

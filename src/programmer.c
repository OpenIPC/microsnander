/*
 * programmer.c — the USB programmers microsnander drives and the choice of one
 * (-P ch341a|ch347|auto). spi_controller.c talks to the chosen one.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <libusb-1.0/libusb.h>
#include "programmer.h"
#include "ch341a_spi.h"
#include "ch347_spi.h"
#include "ezp.h"


static const struct programmer programmers[] = {
	{ "ch341a", "CH341A", 0x1a86, { 0x5512 }, ch341a_spi_init, ch341a_spi_shutdown, ch341a_spi_send_command, ch341a_cs, false },
	{ "ch347",  "CH347",  0x1a86, { 0x55db, 0x55de }, ch347_spi_init, ch347_spi_shutdown, ch347_spi_send_command, ch347_cs, true },
	{ "ezp",    "EZP",    0x1fc8, { 0x310b, 0x310c, 0x310d }, ezp_init, ezp_shutdown, NULL, NULL, false, ezp_flash_init },
};
#define NPROG (sizeof(programmers) / sizeof(programmers[0]))

const struct programmer *programmer;
static const struct programmer *chosen;	/* -P; NULL: auto */

int programmer_select(const char *name)
{
	if (!strcasecmp(name, "auto")) {
		chosen = NULL;
		return 0;
	}
	for (unsigned i = 0; i < NPROG; i++)
		if (!strcasecmp(name, programmers[i].key)) {
			chosen = &programmers[i];
			return 0;
		}
	return -1;
}

const char *programmer_names(void) { return "ch341a, ch347, ezp, auto"; }

/* How many of each supported programmer are plugged in (count[i] for programmers[i]) */
static void programmers_present(unsigned int count[NPROG])
{
	libusb_device **list;
	ssize_t n;

	memset(count, 0, NPROG * sizeof(count[0]));

	if (libusb_init(NULL) < 0)
		return;
	n = libusb_get_device_list(NULL, &list);
	for (ssize_t d = 0; d < n; d++) {
		struct libusb_device_descriptor desc;

		if (libusb_get_device_descriptor(list[d], &desc))
			continue;
		for (unsigned i = 0; i < NPROG; i++)
			for (unsigned p = 0; p < sizeof(programmers[i].pid) / sizeof(programmers[i].pid[0]); p++)
				if (programmers[i].pid[p] && desc.idVendor == programmers[i].vid && desc.idProduct == programmers[i].pid[p])
					count[i]++;
	}
	if (n >= 0)
		libusb_free_device_list(list, 1);
	libusb_exit(NULL);
}

int programmer_init(bool need_clock)
{
	unsigned int count[NPROG], kinds = 0;

	programmers_present(count);
	for (unsigned i = 0; i < NPROG; i++)
		kinds += count[i] != 0;
	programmer = chosen;
	if (!programmer) {
		if (!kinds)
			return -1;
		if (kinds > 1) {
			printf("Several programmers are connected:");
			for (unsigned i = 0; i < NPROG; i++)
				if (count[i])
					printf(" %s", programmers[i].name);
			printf("; choose one with -P.\n\n");
			return -2;
		}
		for (unsigned i = 0; i < NPROG; i++)
			if (count[i])
				programmer = &programmers[i];
	}
	/* the drivers open the first device with a known USB ID: with two of the
	   same kind it is not known which one, so do not guess */
	if (count[programmer - programmers] > 1) {
		printf("%u %s programmers are connected; connect only one.\n\n", count[programmer - programmers], programmer->name);
		programmer = NULL;
		return -2;
	}
	/* before the device is opened: it may be in use by another run */
	if (need_clock && !programmer->clock) {
		printf("-S sets the SPI clock of the CH347 only; the %s has a fixed one.\n\n", programmer->name);
		programmer = NULL;
		return -2;
	}
	if (programmer->init() < 0) {
		programmer = NULL;
		return -1;
	}
	return 0;
}

int programmer_shutdown(void)
{
	int ret = programmer ? programmer->shutdown() : -1;

	programmer = NULL;
	return ret;
}

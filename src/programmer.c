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

static int ch341a_cs(bool low) { return enable_pins(low); }

static const struct programmer programmers[] = {
	{ "ch341a", "CH341A", 0x1a86, { 0x5512 }, ch341a_spi_init, ch341a_spi_shutdown, ch341a_spi_send_command, ch341a_cs },
	{ "ch347",  "CH347",  0x1a86, { 0x55db, 0x55de }, ch347_spi_init, ch347_spi_shutdown, ch347_spi_send_command, ch347_cs },
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

const char *programmer_names(void) { return "ch341a, ch347, auto"; }

/* Which supported programmers are plugged in: a bit per entry of programmers[] */
static unsigned int programmers_present(void)
{
	libusb_device **list;
	unsigned int found = 0;
	ssize_t n;

	if (libusb_init(NULL) < 0)
		return 0;
	n = libusb_get_device_list(NULL, &list);
	for (ssize_t d = 0; d < n; d++) {
		struct libusb_device_descriptor desc;

		if (libusb_get_device_descriptor(list[d], &desc))
			continue;
		for (unsigned i = 0; i < NPROG; i++)
			for (unsigned p = 0; p < sizeof(programmers[i].pid) / sizeof(programmers[i].pid[0]); p++)
				if (programmers[i].pid[p] && desc.idVendor == programmers[i].vid && desc.idProduct == programmers[i].pid[p])
					found |= 1u << i;
	}
	if (n >= 0)
		libusb_free_device_list(list, 1);
	libusb_exit(NULL);
	return found;
}

int programmer_init(void)
{
	programmer = chosen;
	if (!programmer) {
		unsigned int found = programmers_present();

		if (!found)
			return -1;
		if (found & (found - 1)) {
			printf("Several programmers are connected:");
			for (unsigned i = 0; i < NPROG; i++)
				if (found & (1u << i))
					printf(" %s", programmers[i].name);
			printf("; choose one with -P.\n\n");
			return -2;
		}
		for (unsigned i = 0; i < NPROG; i++)
			if (found & (1u << i))
				programmer = &programmers[i];
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

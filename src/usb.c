/*
 * usb.c — how the programmer drivers open their USB device. On Linux by
 * its IDs, as libusb finds it. In the Android app the USB manager opens
 * the device (the user allows it) and hands its file descriptor over:
 * libusb takes it with libusb_wrap_sys_device() and does not look through
 * /dev/bus/usb, which an app may not do.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "usb.h"

int ms_usb_fd = -1;
uint16_t ms_usb_vid, ms_usb_pid;

int ms_usb_init(void)
{
	if (ms_usb_fd >= 0)
		libusb_set_option(NULL, LIBUSB_OPTION_NO_DEVICE_DISCOVERY);
	return libusb_init(NULL);
}

libusb_device_handle *ms_usb_open(uint16_t vid, uint16_t pid)
{
	libusb_device_handle *h = NULL;

	if (ms_usb_fd < 0)
		return libusb_open_device_with_vid_pid(NULL, vid, pid);
	if (vid != ms_usb_vid || pid != ms_usb_pid)
		return NULL;
	if (libusb_wrap_sys_device(NULL, (intptr_t)ms_usb_fd, &h))
		return NULL;
	return h;
}

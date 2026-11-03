/*
 * usb.h — how the programmer drivers open their USB device: by its IDs on
 * Linux, or a file descriptor that the Android USB manager gave the app.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef __MS_USB_H__
#define __MS_USB_H__

#include <stdint.h>
#include <libusb-1.0/libusb.h>

/* Android: the opened device (-1: none, look for the device as on Linux) */
extern int ms_usb_fd;
extern uint16_t ms_usb_vid, ms_usb_pid;

int ms_usb_init(void);					/* libusb_init() for the drivers */
libusb_device_handle *ms_usb_open(uint16_t vid, uint16_t pid);	/* NULL: not there */

#endif /* __MS_USB_H__ */

/*
 * usbfs.h — the part of the libusb-1.0 API the drivers use, on the Linux
 * usbfs (/dev/bus/usb, ioctl) without libusb: the same names, types and
 * error codes, so the drivers do not change. One context, no hotplug, bulk
 * and control transfers only.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef __MS_USBFS_H__
#define __MS_USBFS_H__

#include <stddef.h>
#include <stdint.h>
#include <sys/time.h>
#include <sys/types.h>

#define LIBUSB_API_VERSION	0x01000109
#define LIBUSB_CALL

enum libusb_error {
	LIBUSB_SUCCESS = 0,
	LIBUSB_ERROR_IO = -1,
	LIBUSB_ERROR_INVALID_PARAM = -2,
	LIBUSB_ERROR_ACCESS = -3,
	LIBUSB_ERROR_NO_DEVICE = -4,
	LIBUSB_ERROR_NOT_FOUND = -5,
	LIBUSB_ERROR_BUSY = -6,
	LIBUSB_ERROR_TIMEOUT = -7,
	LIBUSB_ERROR_OVERFLOW = -8,
	LIBUSB_ERROR_PIPE = -9,
	LIBUSB_ERROR_INTERRUPTED = -10,
	LIBUSB_ERROR_NO_MEM = -11,
	LIBUSB_ERROR_NOT_SUPPORTED = -12,
	LIBUSB_ERROR_OTHER = -99,
};

enum libusb_transfer_status {
	LIBUSB_TRANSFER_COMPLETED,
	LIBUSB_TRANSFER_ERROR,
	LIBUSB_TRANSFER_TIMED_OUT,
	LIBUSB_TRANSFER_CANCELLED,
	LIBUSB_TRANSFER_STALL,
	LIBUSB_TRANSFER_NO_DEVICE,
	LIBUSB_TRANSFER_OVERFLOW,
};

enum libusb_option {
	LIBUSB_OPTION_LOG_LEVEL = 0,
	LIBUSB_OPTION_USE_USBDK = 1,
	LIBUSB_OPTION_NO_DEVICE_DISCOVERY = 2,
};

#define LIBUSB_TRANSFER_TYPE_BULK	2
#define LIBUSB_ENDPOINT_IN		0x80
#define LIBUSB_ENDPOINT_OUT		0x00
#define LIBUSB_REQUEST_GET_DESCRIPTOR	0x06
#define LIBUSB_DT_STRING		0x03

typedef struct libusb_context libusb_context;	/* never defined: NULL is the one context */
typedef struct libusb_device libusb_device;
typedef struct libusb_device_handle libusb_device_handle;

struct libusb_device_descriptor {
	uint8_t  bLength;
	uint8_t  bDescriptorType;
	uint16_t bcdUSB;
	uint8_t  bDeviceClass;
	uint8_t  bDeviceSubClass;
	uint8_t  bDeviceProtocol;
	uint8_t  bMaxPacketSize0;
	uint16_t idVendor;
	uint16_t idProduct;
	uint16_t bcdDevice;
	uint8_t  iManufacturer;
	uint8_t  iProduct;
	uint8_t  iSerialNumber;
	uint8_t  bNumConfigurations;
};

struct libusb_transfer;
typedef void (*libusb_transfer_cb_fn)(struct libusb_transfer *transfer);

struct libusb_transfer {
	libusb_device_handle *dev_handle;
	uint8_t flags;
	unsigned char endpoint;
	unsigned char type;
	unsigned int timeout;			/* ms, 0: none */
	enum libusb_transfer_status status;
	int length;
	int actual_length;
	libusb_transfer_cb_fn callback;
	void *user_data;
	unsigned char *buffer;
	int num_iso_packets;
	void *priv;				/* the URB and its deadline */
};

int libusb_init(libusb_context **ctx);
void libusb_exit(libusb_context *ctx);
int libusb_set_option(libusb_context *ctx, enum libusb_option option, ...);
void libusb_set_debug(libusb_context *ctx, int level);
const char *libusb_error_name(int code);

ssize_t libusb_get_device_list(libusb_context *ctx, libusb_device ***list);
void libusb_free_device_list(libusb_device **list, int unref);
int libusb_get_device_descriptor(libusb_device *dev, struct libusb_device_descriptor *desc);
libusb_device_handle *libusb_open_device_with_vid_pid(libusb_context *ctx, uint16_t vid, uint16_t pid);
int libusb_wrap_sys_device(libusb_context *ctx, intptr_t fd, libusb_device_handle **handle);
libusb_device *libusb_get_device(libusb_device_handle *handle);
void libusb_close(libusb_device_handle *handle);

int libusb_claim_interface(libusb_device_handle *handle, int iface);
int libusb_release_interface(libusb_device_handle *handle, int iface);
int libusb_detach_kernel_driver(libusb_device_handle *handle, int iface);
int libusb_attach_kernel_driver(libusb_device_handle *handle, int iface);
int libusb_clear_halt(libusb_device_handle *handle, unsigned char endpoint);

int libusb_bulk_transfer(libusb_device_handle *handle, unsigned char endpoint, unsigned char *data,
			 int length, int *transferred, unsigned int timeout);
int libusb_control_transfer(libusb_device_handle *handle, uint8_t request_type, uint8_t request,
			    uint16_t value, uint16_t index, unsigned char *data, uint16_t length,
			    unsigned int timeout);

struct libusb_transfer *libusb_alloc_transfer(int iso_packets);
void libusb_free_transfer(struct libusb_transfer *transfer);
int libusb_submit_transfer(struct libusb_transfer *transfer);
int libusb_cancel_transfer(struct libusb_transfer *transfer);
int libusb_handle_events_timeout(libusb_context *ctx, struct timeval *tv);

static inline void libusb_fill_bulk_transfer(struct libusb_transfer *t, libusb_device_handle *handle,
					     unsigned char endpoint, unsigned char *buffer, int length,
					     libusb_transfer_cb_fn callback, void *user_data, unsigned int timeout)
{
	t->dev_handle = handle;
	t->endpoint = endpoint;
	t->type = LIBUSB_TRANSFER_TYPE_BULK;
	t->timeout = timeout;
	t->buffer = buffer;
	t->length = length;
	t->user_data = user_data;
	t->callback = callback;
}

static inline int libusb_get_string_descriptor(libusb_device_handle *handle, uint8_t index, uint16_t langid,
					       unsigned char *data, int length)
{
	return libusb_control_transfer(handle, LIBUSB_ENDPOINT_IN, LIBUSB_REQUEST_GET_DESCRIPTOR,
				       (uint16_t)(LIBUSB_DT_STRING << 8 | index), langid, data, (uint16_t)length, 1000);
}

#endif /* __MS_USBFS_H__ */

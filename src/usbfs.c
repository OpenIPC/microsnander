/*
 * usbfs.c — the part of the libusb-1.0 API the drivers use, on the Linux
 * usbfs: the devices are /dev/bus/usb/BBB/DDD, their descriptors are read
 * from those files, the transfers are ioctls (USBDEVFS_BULK, _CONTROL, and
 * URBs for the asynchronous ones), the kernel driver is let go and given
 * back with USBDEVFS_DISCONNECT and _CONNECT. On Android the app gives the
 * file descriptor of the device it opened (libusb_wrap_sys_device).
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/usbdevice_fs.h>
#include "usbfs.h"

#define USBFS_DIR	"/dev/bus/usb"
#define BULK_PART	16384		/* kernels before 3.3 take no more in one bulk transfer or URB */

struct libusb_device {
	char path[32];			/* /dev/bus/usb/001/005 */
	struct libusb_device_descriptor desc;
};

struct libusb_device_handle {
	int fd, own_fd;
	struct libusb_device dev;
	struct libusb_device_handle *next;
};

/* a transfer goes as URBs of BULK_PART at most, as libusb sends them */
struct urb_priv {
	struct usbdevfs_urb *urbs;
	int nurbs, nalloc;
	int pending;			/* URBs not reaped yet */
	int status;			/* the first URB that did not end well: its -errno */
	int stopped;			/* the rest discarded: an error or a short packet */
	long long deadline;		/* ms of now_ms(), 0: never */
	int active, timed_out, cancelled;
	struct libusb_transfer *next;	/* in the list of those in flight */
};

static long long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
}

static libusb_device_handle *handles;	/* the open ones, for the events */
static struct libusb_transfer *active;	/* the asynchronous transfers in flight */

static struct urb_priv *priv_of(struct libusb_transfer *t) { return t->priv; }

static void unlist(struct libusb_transfer *t)
{
	for (struct libusb_transfer **p = &active; *p; p = &priv_of(*p)->next)
		if (*p == t) {
			*p = priv_of(t)->next;
			break;
		}
}


static int error_of(int e)
{
	switch (e) {
	case ENOENT: case ENODEV: case ESHUTDOWN:	return LIBUSB_ERROR_NO_DEVICE;
	case EACCES: case EPERM:			return LIBUSB_ERROR_ACCESS;
	case EBUSY:					return LIBUSB_ERROR_BUSY;
	case ETIMEDOUT:					return LIBUSB_ERROR_TIMEOUT;
	case EPIPE:					return LIBUSB_ERROR_PIPE;
	case EOVERFLOW:					return LIBUSB_ERROR_OVERFLOW;
	case EINTR:					return LIBUSB_ERROR_INTERRUPTED;
	case ENOMEM:					return LIBUSB_ERROR_NO_MEM;
	case EINVAL:					return LIBUSB_ERROR_INVALID_PARAM;
	case ENODATA:					return LIBUSB_ERROR_NOT_FOUND;
	case ENOTTY: case ENOSYS:			return LIBUSB_ERROR_NOT_SUPPORTED;
	default:					return LIBUSB_ERROR_IO;
	}
}

const char *libusb_error_name(int code)
{
	switch (code) {
	/* the drivers also pass a transfer status here, as libusb allows */
	case LIBUSB_TRANSFER_ERROR:		return "LIBUSB_TRANSFER_ERROR";
	case LIBUSB_TRANSFER_TIMED_OUT:		return "LIBUSB_TRANSFER_TIMED_OUT";
	case LIBUSB_TRANSFER_CANCELLED:		return "LIBUSB_TRANSFER_CANCELLED";
	case LIBUSB_TRANSFER_STALL:		return "LIBUSB_TRANSFER_STALL";
	case LIBUSB_TRANSFER_NO_DEVICE:		return "LIBUSB_TRANSFER_NO_DEVICE";
	case LIBUSB_TRANSFER_OVERFLOW:		return "LIBUSB_TRANSFER_OVERFLOW";
	case LIBUSB_SUCCESS:			return "LIBUSB_SUCCESS";
	case LIBUSB_ERROR_IO:			return "LIBUSB_ERROR_IO";
	case LIBUSB_ERROR_INVALID_PARAM:	return "LIBUSB_ERROR_INVALID_PARAM";
	case LIBUSB_ERROR_ACCESS:		return "LIBUSB_ERROR_ACCESS";
	case LIBUSB_ERROR_NO_DEVICE:		return "LIBUSB_ERROR_NO_DEVICE";
	case LIBUSB_ERROR_NOT_FOUND:		return "LIBUSB_ERROR_NOT_FOUND";
	case LIBUSB_ERROR_BUSY:			return "LIBUSB_ERROR_BUSY";
	case LIBUSB_ERROR_TIMEOUT:		return "LIBUSB_ERROR_TIMEOUT";
	case LIBUSB_ERROR_OVERFLOW:		return "LIBUSB_ERROR_OVERFLOW";
	case LIBUSB_ERROR_PIPE:			return "LIBUSB_ERROR_PIPE";
	case LIBUSB_ERROR_INTERRUPTED:		return "LIBUSB_ERROR_INTERRUPTED";
	case LIBUSB_ERROR_NO_MEM:		return "LIBUSB_ERROR_NO_MEM";
	case LIBUSB_ERROR_NOT_SUPPORTED:	return "LIBUSB_ERROR_NOT_SUPPORTED";
	default:				return "LIBUSB_ERROR_OTHER";
	}
}

int libusb_init(libusb_context **ctx) { if (ctx) *ctx = NULL; return 0; }
void libusb_exit(libusb_context *ctx) { (void)ctx; }
int libusb_set_option(libusb_context *ctx, enum libusb_option option, ...) { (void)ctx; (void)option; return 0; }
void libusb_set_debug(libusb_context *ctx, int level) { (void)ctx; (void)level; }

static uint16_t host16(const unsigned char *b)
{
	uint16_t v;

	memcpy(&v, b, 2);
	return v;
}

/* the device descriptor: the first 18 bytes of the device file; usbfs gives
   its 16-bit fields in the byte order of the host already */
static int read_descriptor(int fd, struct libusb_device_descriptor *d)
{
	unsigned char b[18];

	if (pread(fd, b, sizeof(b), 0) != (ssize_t)sizeof(b) || b[1] != 1)
		return -1;
	d->bLength = b[0];
	d->bDescriptorType = b[1];
	d->bcdUSB = host16(b + 2);
	d->bDeviceClass = b[4];
	d->bDeviceSubClass = b[5];
	d->bDeviceProtocol = b[6];
	d->bMaxPacketSize0 = b[7];
	d->idVendor = host16(b + 8);
	d->idProduct = host16(b + 10);
	d->bcdDevice = host16(b + 12);
	d->iManufacturer = b[14];
	d->iProduct = b[15];
	d->iSerialNumber = b[16];
	d->bNumConfigurations = b[17];
	return 0;
}

static long sysfs_value(const char *dir, const char *name, int base)
{
	char path[300], buf[32];
	FILE *f;
	long v = -1;

	snprintf(path, sizeof(path), "%s/%s", dir, name);
	if ((f = fopen(path, "r"))) {
		if (fgets(buf, sizeof(buf), f))
			v = strtol(buf, NULL, base);
		fclose(f);
	}
	return v;
}

/* a device file that cannot be read (its permissions): its IDs from sysfs,
   so the device is still seen and its open then says why it fails */
static int sysfs_descriptor(long busnum, long devnum, struct libusb_device_descriptor *d)
{
	DIR *sys = opendir("/sys/bus/usb/devices");
	struct dirent *e;
	int ret = -1;

	while (sys && ret && (e = readdir(sys))) {
		char dir[300];

		if (e->d_name[0] == '.' || strchr(e->d_name, ':'))
			continue;
		snprintf(dir, sizeof(dir), "/sys/bus/usb/devices/%s", e->d_name);
		if (sysfs_value(dir, "busnum", 10) != busnum || sysfs_value(dir, "devnum", 10) != devnum)
			continue;
		memset(d, 0, sizeof(*d));
		d->bLength = 18;
		d->bDescriptorType = 1;
		d->idVendor = sysfs_value(dir, "idVendor", 16);
		d->idProduct = sysfs_value(dir, "idProduct", 16);
		d->bcdDevice = sysfs_value(dir, "bcdDevice", 16);
		ret = 0;
	}
	if (sys)
		closedir(sys);
	return ret;
}

static int digits(const char *s)
{
	for (; *s; s++)
		if (*s < '0' || *s > '9')
			return 0;
	return 1;
}

ssize_t libusb_get_device_list(libusb_context *ctx, libusb_device ***list)
{
	libusb_device **l = NULL;
	ssize_t n = 0;
	DIR *bus = opendir(USBFS_DIR), *dev;
	struct dirent *b, *d;

	(void)ctx;
	if (!(l = calloc(1, sizeof(*l))))
		goto out;
	while (bus && (b = readdir(bus))) {
		char dir[sizeof(USBFS_DIR) + 16];

		if (!digits(b->d_name) || strlen(b->d_name) > 3)
			continue;
		snprintf(dir, sizeof(dir), "%s/%.3s", USBFS_DIR, b->d_name);
		if (!(dev = opendir(dir)))
			continue;
		while ((d = readdir(dev))) {
			libusb_device *e, **m;
			int fd;

			if (!digits(d->d_name) || strlen(d->d_name) > 3 || !(e = calloc(1, sizeof(*e))))
				continue;
			snprintf(e->path, sizeof(e->path), "%.20s/%.3s", dir, d->d_name);
			fd = open(e->path, O_RDONLY | O_CLOEXEC);
			int bad = fd >= 0 ? read_descriptor(fd, &e->desc) :
				  sysfs_descriptor(strtol(b->d_name, NULL, 10), strtol(d->d_name, NULL, 10), &e->desc);
			if (fd >= 0)
				close(fd);
			if (bad || !(m = realloc(l, (n + 2) * sizeof(*l)))) {
				free(e);
				continue;
			}
			l = m;
			l[n++] = e;
			l[n] = NULL;
		}
		closedir(dev);
	}
out:
	if (bus)
		closedir(bus);
	if (!l)
		return LIBUSB_ERROR_NO_MEM;
	*list = l;
	return n;
}

void libusb_free_device_list(libusb_device **list, int unref)
{
	(void)unref;
	for (libusb_device **p = list; p && *p; p++)
		free(*p);
	free(list);
}

int libusb_get_device_descriptor(libusb_device *dev, struct libusb_device_descriptor *desc)
{
	*desc = dev->desc;
	return 0;
}

static libusb_device_handle *new_handle(int fd, int own, const libusb_device *dev)
{
	libusb_device_handle *h = calloc(1, sizeof(*h));

	if (!h)
		return NULL;
	h->fd = fd;
	h->own_fd = own;
	h->dev = *dev;
	h->next = handles;
	handles = h;
	return h;
}

libusb_device_handle *libusb_open_device_with_vid_pid(libusb_context *ctx, uint16_t vid, uint16_t pid)
{
	libusb_device **list, *found = NULL;
	libusb_device_handle *h = NULL;
	ssize_t n = libusb_get_device_list(ctx, &list);
	int fd;

	for (ssize_t i = 0; i < n && !found; i++)
		if (list[i]->desc.idVendor == vid && list[i]->desc.idProduct == pid)
			found = list[i];
	if (found && (fd = open(found->path, O_RDWR | O_CLOEXEC)) < 0)
		/* as libusb said it: the driver only tells that the open failed */
		printf("%s: %s%s\n", found->path, strerror(errno), errno == EACCES || errno == EPERM ?
		       " (run as root, or allow the device to a group with a udev rule)" : "");
	else if (found && !(h = new_handle(fd, 1, found)))
		close(fd);
	if (n >= 0)
		libusb_free_device_list(list, 1);
	return h;
}

int libusb_wrap_sys_device(libusb_context *ctx, intptr_t fd, libusb_device_handle **handle)
{
	libusb_device dev = { .path = "" };

	(void)ctx;
	if (read_descriptor((int)fd, &dev.desc))
		return LIBUSB_ERROR_IO;
	if (!(*handle = new_handle((int)fd, 0, &dev)))
		return LIBUSB_ERROR_NO_MEM;
	return 0;
}

libusb_device *libusb_get_device(libusb_device_handle *h) { return &h->dev; }

void libusb_close(libusb_device_handle *h)
{
	if (!h)
		return;
	for (libusb_device_handle **p = &handles; *p; p = &(*p)->next)
		if (*p == h) {
			*p = h->next;
			break;
		}
	if (h->own_fd)
		close(h->fd);		/* the app's descriptor stays the app's */
	free(h);
}

int libusb_claim_interface(libusb_device_handle *h, int iface)
{
	unsigned int i = iface;

	return ioctl(h->fd, USBDEVFS_CLAIMINTERFACE, &i) < 0 ? error_of(errno) : 0;
}

int libusb_release_interface(libusb_device_handle *h, int iface)
{
	unsigned int i = iface;

	return ioctl(h->fd, USBDEVFS_RELEASEINTERFACE, &i) < 0 ? error_of(errno) : 0;
}

int libusb_detach_kernel_driver(libusb_device_handle *h, int iface)
{
	struct usbdevfs_getdriver gd = { .interface = iface };
	struct usbdevfs_ioctl cmd = { .ifno = iface, .ioctl_code = USBDEVFS_DISCONNECT };

	if (ioctl(h->fd, USBDEVFS_GETDRIVER, &gd) < 0)
		return error_of(errno);			/* ENODATA: no driver */
	if (!strcmp(gd.driver, "usbfs"))
		return LIBUSB_ERROR_NOT_FOUND;		/* that is us */
	return ioctl(h->fd, USBDEVFS_IOCTL, &cmd) < 0 ? error_of(errno) : 0;
}

int libusb_attach_kernel_driver(libusb_device_handle *h, int iface)
{
	struct usbdevfs_ioctl cmd = { .ifno = iface, .ioctl_code = USBDEVFS_CONNECT };

	return ioctl(h->fd, USBDEVFS_IOCTL, &cmd) < 0 ? error_of(errno) : 0;
}

int libusb_clear_halt(libusb_device_handle *h, unsigned char endpoint)
{
	unsigned int ep = endpoint;

	return ioctl(h->fd, USBDEVFS_CLEAR_HALT, &ep) < 0 ? error_of(errno) : 0;
}

int libusb_bulk_transfer(libusb_device_handle *h, unsigned char endpoint, unsigned char *data,
			 int length, int *transferred, unsigned int timeout)
{
	long long end = timeout ? now_ms() + timeout : 0;	/* one timeout for all the parts */
	int done = 0;

	*transferred = 0;
	while (done < length) {
		long long left = end ? end - now_ms() : 0;

		if (end && left <= 0)
			return LIBUSB_ERROR_TIMEOUT;
		struct usbdevfs_bulktransfer bt = {
			.ep = endpoint, .len = length - done < BULK_PART ? length - done : BULK_PART,
			.timeout = (unsigned int)left, .data = data + done,
		};
		int r = ioctl(h->fd, USBDEVFS_BULK, &bt);

		if (r < 0)
			return error_of(errno);
		done += r;
		*transferred = done;
		if (r < (int)bt.len)			/* a short packet ends it */
			break;
	}
	return 0;
}

int libusb_control_transfer(libusb_device_handle *h, uint8_t request_type, uint8_t request,
			    uint16_t value, uint16_t index, unsigned char *data, uint16_t length,
			    unsigned int timeout)
{
	struct usbdevfs_ctrltransfer ct = {
		.bRequestType = request_type, .bRequest = request, .wValue = value,
		.wIndex = index, .wLength = length, .timeout = timeout, .data = data,
	};
	int r = ioctl(h->fd, USBDEVFS_CONTROL, &ct);

	return r < 0 ? error_of(errno) : r;
}

/* ---- asynchronous transfers ---- */

struct libusb_transfer *libusb_alloc_transfer(int iso_packets)
{
	struct libusb_transfer *t;

	if (iso_packets || !(t = calloc(1, sizeof(*t))))
		return NULL;
	if (!(t->priv = calloc(1, sizeof(struct urb_priv)))) {
		free(t);
		return NULL;
	}
	return t;
}

void libusb_free_transfer(struct libusb_transfer *t)
{
	if (!t)
		return;
	if (priv_of(t)->active)
		unlist(t);
	free(priv_of(t)->urbs);
	free(t->priv);
	free(t);
}

/* the URBs not reaped yet go back: the kernel returns them with -ENOENT */
static void discard_rest(struct libusb_transfer *t)
{
	struct urb_priv *p = t->priv;

	for (int i = 0; i < p->nurbs; i++)
		ioctl(t->dev_handle->fd, USBDEVFS_DISCARDURB, &p->urbs[i]);	/* EINVAL: reaped already */
}

int libusb_submit_transfer(struct libusb_transfer *t)
{
	struct urb_priv *p = t->priv;
	int n = t->length ? (t->length + BULK_PART - 1) / BULK_PART : 1;

	if (p->active)
		return LIBUSB_ERROR_BUSY;
	if (n > p->nalloc) {
		struct usbdevfs_urb *u = realloc(p->urbs, n * sizeof(*u));

		if (!u)
			return LIBUSB_ERROR_NO_MEM;
		p->urbs = u;
		p->nalloc = n;
	}
	memset(p->urbs, 0, n * sizeof(*p->urbs));
	p->nurbs = p->pending = 0;
	p->status = p->stopped = p->timed_out = p->cancelled = 0;
	t->actual_length = 0;
	for (int i = 0; i < n; i++) {
		struct usbdevfs_urb *u = &p->urbs[i];
		int off = i * BULK_PART;

		u->type = USBDEVFS_URB_TYPE_BULK;
		u->endpoint = t->endpoint;
		u->buffer = t->buffer + off;
		u->buffer_length = t->length - off < BULK_PART ? t->length - off : BULK_PART;
		u->usercontext = t;
		/* as libusb: the parts after the first continue one transfer, and in an
		   IN transfer a short packet before the last part ends it (-EREMOTEIO) */
		if (i)
			u->flags |= USBDEVFS_URB_BULK_CONTINUATION;
		if (i < n - 1 && (t->endpoint & LIBUSB_ENDPOINT_IN))
			u->flags |= USBDEVFS_URB_SHORT_NOT_OK;
		if (ioctl(t->dev_handle->fd, USBDEVFS_SUBMITURB, u) < 0) {
			int e = errno;

			if (!i)
				return error_of(e);
			/* some went: take them back, the transfer ends as an error */
			p->status = -e;
			p->stopped = 1;
			discard_rest(t);
			break;
		}
		p->nurbs++;
		p->pending++;
	}
	p->deadline = t->timeout ? now_ms() + t->timeout : 0;
	p->active = 1;
	p->next = active;
	active = t;
	return 0;
}

int libusb_cancel_transfer(struct libusb_transfer *t)
{
	struct urb_priv *p = t->priv;

	if (!p->active)
		return LIBUSB_ERROR_NOT_FOUND;
	p->cancelled = 1;
	p->stopped = 1;
	discard_rest(t);
	return 0;
}

/* the transfer ends: its status, its callback */
static void finish(struct libusb_transfer *t, int gone)
{
	struct urb_priv *p = t->priv;
	int s = p->status;

	unlist(t);
	p->active = 0;
	if (gone)
		t->status = LIBUSB_TRANSFER_NO_DEVICE;
	else if (p->timed_out)
		t->status = LIBUSB_TRANSFER_TIMED_OUT;
	else if (p->cancelled)
		t->status = LIBUSB_TRANSFER_CANCELLED;
	else if (!s)
		t->status = LIBUSB_TRANSFER_COMPLETED;
	else if (s == -EPIPE)
		t->status = LIBUSB_TRANSFER_STALL;
	else if (s == -ENODEV || s == -ESHUTDOWN)
		t->status = LIBUSB_TRANSFER_NO_DEVICE;
	else if (s == -EOVERFLOW)
		t->status = LIBUSB_TRANSFER_OVERFLOW;
	else
		t->status = LIBUSB_TRANSFER_ERROR;
	if (t->callback)
		t->callback(t);
}

/* one URB of a transfer came back */
static void reaped(struct usbdevfs_urb *u)
{
	struct libusb_transfer *t = u->usercontext;
	struct urb_priv *p = t->priv;
	int s = u->status;

	p->pending--;
	if (s == -ENOENT || s == -ECONNRESET || s == -EREMOTEIO) {
		/* discarded by us, or the rest after a short packet: not an error */
	} else if (s) {
		if (!p->status)
			p->status = s;
	}
	if (!p->stopped) {
		t->actual_length += u->actual_length;
		if (s || u->actual_length < u->buffer_length) {	/* an error or a short packet */
			p->stopped = 1;
			discard_rest(t);
		}
	}
	if (!p->pending)
		finish(t, 0);
}

int libusb_handle_events_timeout(libusb_context *ctx, struct timeval *tv)
{
	struct pollfd fds[8];
	nfds_t n = 0;
	long long now = now_ms(), wait = tv ? tv->tv_sec * 1000LL + tv->tv_usec / 1000 : -1;

	(void)ctx;
	/* wake up for the first transfer that times out */
	for (struct libusb_transfer *t = active; t; t = priv_of(t)->next) {
		struct urb_priv *p = t->priv;

		if (p->deadline && !p->timed_out && (wait < 0 || p->deadline - now < wait))
			wait = p->deadline > now ? p->deadline - now : 0;
	}
	for (libusb_device_handle *h = handles; h && n < sizeof(fds) / sizeof(fds[0]); h = h->next)
		fds[n++] = (struct pollfd){ .fd = h->fd, .events = POLLOUT };
	if (poll(fds, n, wait > 0x7fffffff ? 0x7fffffff : (int)wait) < 0 && errno != EINTR)
		return error_of(errno);

	/* the deadlines first, as libusb does: a URB that came back after its
	   transfer's deadline ends it as TIMED_OUT, not as done */
	now = now_ms();
	for (struct libusb_transfer *t = active; t; t = priv_of(t)->next) {
		struct urb_priv *p = t->priv;

		if (p->deadline && !p->timed_out && p->deadline <= now) {
			p->timed_out = 1;		/* its URBs come back later, as TIMED_OUT */
			p->stopped = 1;
			discard_rest(t);
		}
	}

	for (libusb_device_handle *h = handles; h; h = h->next) {
		struct usbdevfs_urb *u;

		while (ioctl(h->fd, USBDEVFS_REAPURBNDELAY, &u) == 0)
			reaped(u);
		if (errno == ENODEV)			/* unplugged: nothing comes back */
			for (struct libusb_transfer *t = active, *next; t; t = next) {
				next = priv_of(t)->next;
				if (t->dev_handle == h)
					finish(t, 1);
			}
	}

	return 0;
}

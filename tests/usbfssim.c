/* tests/usbfssim.c — src/usbfs.c against a fake usbfs: ioctl, poll and the
   clock are replaced, so the URBs it submits can be looked at, completed,
   cut short or held past their deadline. */
#include <errno.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/ioctl.h>
#include <linux/usbdevice_fs.h>

static int fake_ioctl(int fd, unsigned long req, ...);
static int fake_poll(struct pollfd *f, nfds_t n, int ms) { (void)f; (void)n; (void)ms; return 1; }
static long long clock_ms;
static int fake_clock(clockid_t id, struct timespec *t) { (void)id; t->tv_sec = clock_ms / 1000; t->tv_nsec = clock_ms % 1000 * 1000000; return 0; }
#define ioctl fake_ioctl
#define poll fake_poll
#define clock_gettime fake_clock
#include "../src/usbfs.c"
#undef ioctl
#undef poll
#undef clock_gettime

static int fail;
static void check(int ok, const char *what) { printf("%s %s\n", ok ? "ok  " : "FAIL", what); if (!ok) fail = 1; }

static struct usbdevfs_urb *submitted[16], *done[16];
static int nsub, ndone, nreaped, bulk_calls, unplugged;
static unsigned int bulk_timeouts[8];

/* the URB i comes back with this length and status */
static void complete_urb(int i, int len, int status)
{
	submitted[i]->actual_length = len;
	submitted[i]->status = status;
	done[ndone++] = submitted[i];
}

static int fake_ioctl(int fd, unsigned long req, ...)
{
	va_list ap;
	void *arg;

	(void)fd;
	va_start(ap, req);
	arg = va_arg(ap, void *);
	va_end(ap);
	if (req == USBDEVFS_SUBMITURB) {
		submitted[nsub++] = arg;
		return 0;
	}
	if (req == USBDEVFS_DISCARDURB) {		/* in flight: back as -ENOENT; done: EINVAL */
		for (int i = 0; i < ndone; i++)
			if (done[i] == arg) { errno = EINVAL; return -1; }
		struct usbdevfs_urb *u = arg;
		u->status = -ENOENT;
		u->actual_length = 0;
		done[ndone++] = u;
		return 0;
	}
	if (req == USBDEVFS_REAPURBNDELAY) {
		if (nreaped >= ndone) { errno = unplugged ? ENODEV : EAGAIN; return -1; }
		*(struct usbdevfs_urb **)arg = done[nreaped++];
		return 0;
	}
	if (req == USBDEVFS_BULK) {
		struct usbdevfs_bulktransfer *bt = arg;
		bulk_timeouts[bulk_calls++] = bt->timeout;
		clock_ms += 2000;			/* every part takes 2 s */
		return bt->len;
	}
	errno = ENOTTY;
	return -1;
}

static int calls;
static void cb(struct libusb_transfer *t) { (void)t; calls++; }

static void reset(void) { nsub = ndone = nreaped = calls = 0; }	/* the URBs of one transfer at a time */

int main(void)
{
	static unsigned char buf[60000];
	struct libusb_device_handle h = { .fd = 3 };
	struct libusb_transfer *t = libusb_alloc_transfer(0);
	struct timeval tv = { 0, 0 };
	int n;

	handles = &h;

	/* OUT of 40000 bytes: three URBs, the later ones continue the first */
	reset();
	libusb_fill_bulk_transfer(t, &h, 0x02, buf, 40000, cb, NULL, 1000);
	check(!libusb_submit_transfer(t) && nsub == 3 && submitted[0]->buffer_length == 16384 &&
	      submitted[2]->buffer_length == 40000 - 2 * 16384 && !(submitted[0]->flags & USBDEVFS_URB_BULK_CONTINUATION) &&
	      (submitted[1]->flags & USBDEVFS_URB_BULK_CONTINUATION) && (submitted[2]->flags & USBDEVFS_URB_BULK_CONTINUATION),
	      "a transfer of 40000 bytes goes as URBs of 16384, 16384 and 7232, continued");
	for (int i = 0; i < 3; i++)
		complete_urb(i, submitted[i]->buffer_length, 0);
	libusb_handle_events_timeout(NULL, &tv);
	check(calls == 1 && t->status == LIBUSB_TRANSFER_COMPLETED && t->actual_length == 40000,
	      "  all three back: one callback, completed, 40000 bytes");

	/* IN of 40000: a short first part ends it, the rest is taken back */
	reset();
	libusb_fill_bulk_transfer(t, &h, 0x82, buf, 40000, cb, NULL, 1000);
	libusb_submit_transfer(t);
	check((submitted[0]->flags & USBDEVFS_URB_SHORT_NOT_OK) && (submitted[1]->flags & USBDEVFS_URB_SHORT_NOT_OK) &&
	      !(submitted[2]->flags & USBDEVFS_URB_SHORT_NOT_OK), "an IN transfer: a short packet before the last part stops it");
	complete_urb(0, 100, -EREMOTEIO);
	libusb_handle_events_timeout(NULL, &tv);
	check(nsub == 3 && submitted[1]->status == -ENOENT && submitted[2]->status == -ENOENT,
	      "  the short part back: the other two are taken back");
	check(calls == 1 && t->status == LIBUSB_TRANSFER_COMPLETED && t->actual_length == 100,
	      "  one callback when they are back: completed with the 100 bytes there are");

	/* a URB back after the deadline of its transfer: TIMED_OUT, not done */
	reset();
	libusb_fill_bulk_transfer(t, &h, 0x02, buf, 64, cb, NULL, 1000);
	libusb_submit_transfer(t);
	clock_ms += 1500;
	complete_urb(0, 64, 0);
	libusb_handle_events_timeout(NULL, &tv);
	check(calls == 1 && t->status == LIBUSB_TRANSFER_TIMED_OUT, "a URB that comes back after the deadline: timed out");

	/* cancel */
	reset();
	libusb_fill_bulk_transfer(t, &h, 0x82, buf, 64, cb, NULL, 0);
	libusb_submit_transfer(t);
	check(!libusb_cancel_transfer(t), "cancel a transfer in flight");
	libusb_handle_events_timeout(NULL, &tv);
	check(calls == 1 && t->status == LIBUSB_TRANSFER_CANCELLED, "  it ends cancelled");
	/* unplugged with a transfer in flight: it ends once, NO_DEVICE */
	reset();
	libusb_fill_bulk_transfer(t, &h, 0x82, buf, 40000, cb, NULL, 0);
	libusb_submit_transfer(t);
	unplugged = 1;
	libusb_handle_events_timeout(NULL, &tv);
	libusb_handle_events_timeout(NULL, &tv);
	check(calls == 1 && t->status == LIBUSB_TRANSFER_NO_DEVICE && !active && !priv_of(t)->pending,
	      "unplugged during a transfer: one callback, no device, nothing left in flight");
	unplugged = 0;
	libusb_free_transfer(t);

	/* a synchronous bulk transfer: one timeout for all its parts */
	clock_ms = 100000;
	int ret = libusb_bulk_transfer(&h, 0x02, buf, 60000, &n, 5000);
	check(ret == LIBUSB_ERROR_TIMEOUT && bulk_calls == 3 && bulk_timeouts[0] == 5000 && bulk_timeouts[1] == 3000 &&
	      bulk_timeouts[2] == 1000 && n == 3 * 16384,
	      "a bulk transfer of 60000 bytes, 2 s a part: the parts get what is left of one 5 s timeout, the 4th none");
	return fail;
}

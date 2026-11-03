/* tests/ch341sim.c — src/ch341a_spi.c against a fake libusb that plays a CH341A:
   the OUT stream is clocked out at ~60 KB/s, a transfer that would take longer
   than its timeout fails as on the real device, and every byte clocked in is a
   known pattern, so the order of the data read back can be checked. */
#include <stdlib.h>
#include "../src/ch341a_spi.c"

#define RATE_BYTES_PER_MS 60		/* ~60 KB/s of SPI stream */

static struct libusb_transfer *pending[1 + USB_IN_TRANSFERS];
static int npending;
static unsigned char miso[1 << 20];	/* bytes clocked in, waiting for IN transfers */
static unsigned int miso_head, miso_tail;
static unsigned int clocked;		/* bytes clocked since CS low: the pattern index */
static unsigned int max_out, out_fail;

static unsigned char pattern(unsigned int i) { return (unsigned char)(i * 13 + (i >> 8)); }

struct libusb_transfer *libusb_alloc_transfer(int iso) { (void)iso; return calloc(1, sizeof(struct libusb_transfer)); }
void libusb_free_transfer(struct libusb_transfer *t) { free(t); }
int libusb_submit_transfer(struct libusb_transfer *t) { pending[npending++] = t; return 0; }
int libusb_cancel_transfer(struct libusb_transfer *t) { (void)t; return 0; }
const char *libusb_error_name(int code) { (void)code; return "LIBUSB_ERROR"; }

static void out_done(struct libusb_transfer *t)
{
	if ((unsigned int)t->length > max_out)
		max_out = t->length;
	if ((unsigned int)t->length / RATE_BYTES_PER_MS > t->timeout) {
		t->status = LIBUSB_TRANSFER_TIMED_OUT;
		out_fail++;
		return;
	}
	/* 32-byte packets: a stream packet is 0xA8 and up to 31 bytes to clock out */
	for (int p = 0; p < t->length; p += CH341_PACKET_LENGTH) {
		int n = t->length - p < CH341_PACKET_LENGTH ? t->length - p : CH341_PACKET_LENGTH;
		if (t->buffer[p] != CH341A_CMD_SPI_STREAM)
			continue;
		for (int i = 1; i < n; i++)
			miso[miso_tail++ % sizeof(miso)] = swap_byte(pattern(clocked++));
	}
	t->status = LIBUSB_TRANSFER_COMPLETED;
	t->actual_length = t->length;
}

int libusb_handle_events_timeout(libusb_context *ctx, struct timeval *tv)
{
	(void)ctx; (void)tv;
	for (int i = 0; i < npending; i++)
		if (pending[i]->endpoint == WRITE_EP)
			out_done(pending[i]);
	for (int i = 0; i < npending; i++) {
		struct libusb_transfer *t = pending[i];
		if (t->endpoint != READ_EP)
			continue;
		unsigned int n = 0;
		while (n < (unsigned int)t->length && miso_head != miso_tail)
			t->buffer[n++] = miso[miso_head++ % sizeof(miso)];
		t->status = LIBUSB_TRANSFER_COMPLETED;
		t->actual_length = n;
	}
	for (int i = 0; i < npending; i++)
		pending[i]->callback(pending[i]);
	npending = 0;
	return 0;
}

/* the rest of the libusb API the driver links against; not used here */
int libusb_init(libusb_context **c) { (void)c; return 0; }
void libusb_exit(libusb_context *c) { (void)c; }
int libusb_set_option(libusb_context *c, enum libusb_option o, ...) { (void)c; (void)o; return 0; }
void libusb_set_debug(libusb_context *c, int l) { (void)c; (void)l; }
libusb_device_handle *libusb_open_device_with_vid_pid(libusb_context *c, uint16_t v, uint16_t p) { (void)c; (void)v; (void)p; return NULL; }
void libusb_close(libusb_device_handle *h) { (void)h; }
int libusb_detach_kernel_driver(libusb_device_handle *h, int i) { (void)h; (void)i; return 0; }
int libusb_claim_interface(libusb_device_handle *h, int i) { (void)h; (void)i; return 0; }
int libusb_release_interface(libusb_device_handle *h, int i) { (void)h; (void)i; return 0; }
libusb_device *libusb_get_device(libusb_device_handle *h) { (void)h; return NULL; }
int libusb_get_device_descriptor(libusb_device *d, struct libusb_device_descriptor *s) { (void)d; (void)s; return 0; }

static int fail;
static void check(int ok, const char *what) { printf("%s %s\n", ok ? "ok  " : "FAIL", what); if (!ok) fail = 1; }

static int read_ok(unsigned int wlen, unsigned int rlen)
{
	static unsigned char w[1 << 17], r[1 << 17];
	memset(w, 0x5a, wlen);
	clocked = 0; miso_head = miso_tail = 0;
	if (ch341a_spi_send_command(wlen, rlen, w, r))
		return 0;
	for (unsigned int i = 0; i < rlen; i++)
		if (r[i] != pattern(wlen + i))
			return 0;
	return clocked == wlen + rlen;
}

int main(void)
{
	handle = (libusb_device_handle *)1;
	transfer_out = libusb_alloc_transfer(0);
	libusb_fill_bulk_transfer(transfer_out, handle, WRITE_EP, NULL, 0, cb_out, NULL, USB_TIMEOUT);
	for (int i = 0; i < USB_IN_TRANSFERS; i++) {
		transfer_ins[i] = libusb_alloc_transfer(0);
		libusb_fill_bulk_transfer(transfer_ins[i], handle, READ_EP, NULL, 0, cb_in, NULL, USB_TIMEOUT);
	}

	check(read_ok(4, 256), "short command and read");
	check(read_ok(0, 4096), "4 KB read");
	max_out = 0;
	check(read_ok(0, 65536), "64 KB read (a whole NOR sector) does not time out");
	check(max_out <= CH341_PACKET_LENGTH + (CH341_MAX_STREAM + 30) / 31 * 32 && out_fail == 0,
	      "  every USB transfer stays inside the stream limit");
	check(read_ok(5, 100000), "command bytes then a 100000-byte read: data in order");
	check(read_ok(40000, 0), "40000-byte write in parts");
	check(read_ok(20000, 20000), "20000 bytes written then 20000 read");
	check(usb_timeout(CH341_MAX_STREAM * 2) > (CH341_MAX_STREAM * 2) / RATE_BYTES_PER_MS,
	      "the timeout grows with the transfer size");

	if (!fail) printf("all ch341sim checks passed\n");
	return fail;
}

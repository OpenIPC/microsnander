/* tests/ch341sim.c — src/ch341a_spi.c against a fake libusb that plays a CH341A:
   the OUT stream is clocked out at ~60 KB/s, a transfer that would take longer
   than its timeout fails as on the real device, and every byte clocked in is a
   known pattern, so the order of the data read back can be checked. The UIO
   stream moves pins: behind them a SPI mode 0 slave that takes MOSI and gives
   the same pattern bit by bit (-S, SPI by the pins, #108). */
#include <stdlib.h>
#include "../src/ch341a_spi.c"
#include "../src/usb.c"

#define RATE_BYTES_PER_MS 60		/* ~60 KB/s of SPI stream */

static struct libusb_transfer *pending[1 + USB_IN_TRANSFERS];
static int npending;
static unsigned char miso[1 << 20];	/* bytes clocked in, waiting for IN transfers */
static unsigned int miso_head, miso_tail;
static unsigned int clocked;		/* bytes clocked since CS low: the pattern index */
static unsigned int max_out, out_fail;
static int short_reply;			/* the next IN answer one byte short */

static unsigned char pattern(unsigned int i) { return (unsigned char)(i * 13 + (i >> 8)); }

/* the pins of the UIO stream and the slave behind them */
static unsigned char pins = 0x37;	/* CS high, SCK low */
static unsigned int pbits;		/* bits clocked since CS low */
static int miso_bit;
static unsigned char mosi[1 << 18];	/* the bytes the slave took */
static unsigned int us_since_edge = 1000, min_half = 1000000, max_us_cmd, edges_cs_high, uio_bad;

static void uio_packet(const unsigned char *b, int n)
{
	for (int i = 1; i < n; i++) {
		unsigned char c = b[i];
		if (c == CH341A_CMD_UIO_STM_END)
			return;
		if ((c & 0xC0) == CH341A_CMD_UIO_STM_OUT) {
			unsigned char v = c & 0x3F;
			if ((pins ^ v) & 0x08) {		/* an SCK edge */
				if (us_since_edge < min_half)
					min_half = us_since_edge;
				us_since_edge = 0;
				if (v & 0x01)
					edges_cs_high++;
				else if (v & 0x08) {		/* rising, CS low: one bit each way */
					unsigned int byte = pbits / 8, bit = 7 - pbits % 8;
					if (byte < sizeof(mosi))
						mosi[byte] = (mosi[byte] & ~(1 << bit)) | ((v >> 5 & 1) << bit);
					/* JEDEC ID (0x9F): the ID of a W25Q64 after the command byte;
					   anything else: the pattern */
					if (byte >= 1 && byte <= 3 && mosi[0] == 0x9F)
						miso_bit = (unsigned char[]){ 0xef, 0x40, 0x17 }[byte - 1] >> bit & 1;
					else
						miso_bit = pattern(byte) >> bit & 1;
					pbits++;
				}
			}
			if ((pins & 0x01) && !(v & 0x01))	/* CS low: a new command */
				pbits = 0;
			pins = v;
		} else if ((c & 0xC0) == CH341A_CMD_UIO_STM_US) {
			us_since_edge += c & 0x3F;
			if ((c & 0x3Fu) > max_us_cmd)
				max_us_cmd = c & 0x3F;
			if (!(c & 0x3F))
				uio_bad++;
		} else if ((c & 0xC0) == CH341A_CMD_UIO_STM_DIR) {
		} else if (c == CH341A_CMD_UIO_STM_IN) {
			miso[miso_tail++ % sizeof(miso)] = (miso_bit ? 0x80 : 0) | (pins & 0x3F);
		} else
			uio_bad++;
	}
	uio_bad++;			/* no END */
}

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
		if (t->buffer[p] == CH341A_CMD_UIO_STREAM) {
			uio_packet(t->buffer + p, n);
			continue;
		}
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
		unsigned int n = 0, len = t->length;
		if (short_reply && len > 1) {		/* one answer cut short, its bytes lost */
			len--;
			short_reply = 0;
			miso_head++;
		}
		while (n < len && miso_head != miso_tail)
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
int libusb_wrap_sys_device(libusb_context *c, intptr_t fd, libusb_device_handle **h) { (void)c; (void)fd; *h = NULL; return -1; }
libusb_device_handle *libusb_open_device_with_vid_pid(libusb_context *c, uint16_t v, uint16_t p) { (void)c; (void)v; (void)p; return NULL; }
void libusb_close(libusb_device_handle *h) { (void)h; }
int libusb_detach_kernel_driver(libusb_device_handle *h, int i) { (void)h; (void)i; return 0; }
int libusb_attach_kernel_driver(libusb_device_handle *h, int i) { (void)h; (void)i; return 0; }
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

/* by the pins: CS low (cs: as ch341a_cs(true) sends it), wlen bytes of 0x5a
   out, rlen read; the slave saw them all and gave the pattern */
static int pins_ok(unsigned int wlen, unsigned int rlen, int cs)
{
	static unsigned char w[1 << 17], r[1 << 17];
	for (unsigned int i = 0; i < wlen; i++)
		w[i] = (unsigned char)(0x5a + i);
	miso_head = miso_tail = 0; min_half = 1000000; us_since_edge = 1000;
	if (cs) {
		ch341a_cs(true);
		edges_cs_high = 0;
	} else
		pbits = 0;		/* the slave counts from here */
	if (ch341a_spi_send_command(wlen, rlen, w, r))
		return 0;
	if (pbits != (wlen + rlen) * 8 || miso_head != miso_tail)
		return 0;
	for (unsigned int i = 0; i < wlen; i++)
		if (mosi[i] != w[i])
			return 0;
	for (unsigned int i = 0; i < rlen; i++)
		if (mosi[wlen + i] != 0xFF || r[i] != pattern(wlen + i))
			return 0;
	return 1;
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

	/* -S: SPI by the pins */
	check(ch341a_set_speed("15M") < 0 && ch341a_set_speed("100k") == 0 && pin_half_us == 5,
	      "-S takes the CH341A clocks only (any case): 100K is 5 us a half period");
	{
		unsigned char cmd = 0x9F, id[3] = { 0 };
		miso_head = miso_tail = 0;
		ch341a_cs(true);
		check(ch341a_spi_send_command(1, 3, &cmd, id) == 0 && id[0] == 0xef && id[1] == 0x40 && id[2] == 0x17,
		      "by the pins at 100K: JEDEC ID (9F) reads ef 40 17, as Detect does");
	}
	check(pins_ok(4, 256, 1), "  command, then a read, data in order");
	check(min_half >= 5 && uio_bad == 0, "  every half period of SCK at least 5 us; well-formed UIO packets");
	check(!(pins & 0x08) && !(pins & 0x01), "  SCK low at the end, CS still low");
	check(pins_ok(5, 3000, 1), "  3000-byte read in several transfers");
	max_out = 0;
	check(pins_ok(1000, 0, 0) && max_out <= (PIN_MAX_PACKETS + 2) * CH341_PACKET_LENGTH && out_fail == 0,
	      "  1000-byte write without CS pending: in transfers inside the limit");
	check(ch341a_set_speed("1K") == 0 && pins_ok(4, 16, 1) && min_half >= 500 && max_us_cmd <= 63 && uio_bad == 0,
	      "by the pins at 1K: at least 500 us a half period, each delay command at most 63 us");
	pin_half_us = 0;
	check(read_ok(4, 256), "back on the stream: as before");

	{
		static unsigned char r[100];
		clocked = 0; miso_head = miso_tail = 0; short_reply = 1;
		check(ch341a_spi_send_command(0, 100, NULL, r) != 0, "a short USB answer is an error, not data at the wrong offset");
	}
	short_reply = 0;
	check(read_ok(4, 256), "  and the next transfer works");

	if (!fail) printf("all ch341sim checks passed\n");
	return fail;
}

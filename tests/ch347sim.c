/* tests/ch347sim.c — src/ch347_spi.c against a fake libusb that plays a CH347:
   it parses the command packets (SET_CFG, CS_CTRL, SPI_OUT, SPI_IN), keeps what
   the SPI bus would see, and answers like the chip, a read with a known pattern. */
#include <stdlib.h>
#include "../src/ch347_spi.c"
#include "../src/usb.c"

static unsigned short sim_pid = 0x55db;
static int claimed_iface = -1, detached_iface = -1, attached_iface = -1;
static unsigned char cfg15 = 0xff;
static int cs_low;
static unsigned char mosi[200000];
static unsigned int nmosi, clocked;
static unsigned char resp[CH347_PACKET_SIZE];
static int nresp;
static unsigned int pending_in;			/* SPI_IN bytes still to send */
static unsigned int max_out, max_in, nout;
static int cs_while_write_ok = 1;
static int nout_in, ncmd;
static int bad = 0;	/* 1: write status 1, 2: read answer with another command, 3: empty read answer, 4: length header too big */

static unsigned char pattern(unsigned int i) { return (unsigned char)(i * 29 + (i >> 9)); }

int libusb_init(libusb_context **c) { (void)c; return 0; }
void libusb_exit(libusb_context *c) { (void)c; }
const char *libusb_error_name(int e) { (void)e; return "LIBUSB_ERROR"; }
static int no_discovery, wrapped_fd = -1;
int libusb_set_option(libusb_context *c, enum libusb_option o, ...) { (void)c; if (o == LIBUSB_OPTION_NO_DEVICE_DISCOVERY) no_discovery = 1; return 0; }
int libusb_wrap_sys_device(libusb_context *c, intptr_t fd, libusb_device_handle **h) { (void)c; wrapped_fd = (int)fd; *h = (libusb_device_handle *)1; return 0; }
libusb_device_handle *libusb_open_device_with_vid_pid(libusb_context *c, uint16_t v, uint16_t p)
{ (void)c; return v == 0x1a86 && p == sim_pid ? (libusb_device_handle *)1 : NULL; }
void libusb_close(libusb_device_handle *h) { (void)h; }
int libusb_detach_kernel_driver(libusb_device_handle *h, int i) { (void)h; detached_iface = i; return 0; }
int libusb_attach_kernel_driver(libusb_device_handle *h, int i) { (void)h; attached_iface = i; return 0; }
int libusb_claim_interface(libusb_device_handle *h, int i) { (void)h; claimed_iface = i; return 0; }
int libusb_release_interface(libusb_device_handle *h, int i) { (void)h; (void)i; return 0; }

int libusb_bulk_transfer(libusb_device_handle *h, unsigned char ep, unsigned char *d, int len, int *done, unsigned int timeout)
{
	(void)h; (void)timeout;
	if (ep == CH347_WRITE_EP) {		/* one or more commands: "cmd, length, data" each */
		if ((unsigned int)len > max_out) max_out = len;
		nout++;
		for (int off = 0; off < len; ) {
		unsigned char *c = d + off;
		unsigned int n;
		if (len - off < 3) return LIBUSB_ERROR_IO;
		n = c[1] | (c[2] << 8);
		if (off + 3 + (int)n > len) return LIBUSB_ERROR_IO;
		off += 3 + n;
		ncmd++;
		switch (c[0]) {
		case CH347_CMD_SPI_SET_CFG: cfg15 = c[15]; resp[0] = c[0]; resp[1] = 1; resp[2] = 0; resp[3] = 0; nresp = 4; break;
		case CH347_CMD_SPI_CS_CTRL:
			if (c[3] & CH347_CS_CHANGE) cs_low = !(c[3] & CH347_CS_DEASSERT);
			break;
		case CH347_CMD_SPI_OUT:
			if (!cs_low) cs_while_write_ok = 0;
			memcpy(mosi + nmosi, c + 3, n); nmosi += n; clocked += n;
			resp[0] = c[0]; resp[1] = 1; resp[2] = 0; resp[3] = bad == 1; nresp = 4;
			break;
		case CH347_CMD_SPI_IN:
			pending_in = c[3] | (c[4] << 8) | (c[5] << 16) | ((unsigned int)c[6] << 24);
			break;
		case CH347_CMD_SPI_OUT_IN:		/* sent bytes go out, the pattern comes in */
			if (!cs_low) cs_while_write_ok = 0;
			memcpy(mosi + nmosi, c + 3, n); nmosi += n;
			resp[0] = c[0]; resp[1] = n & 0xff; resp[2] = n >> 8;
			for (unsigned int i = 0; i < n; i++) resp[3 + i] = pattern(clocked++);
			nresp = n + 3; nout_in++;
			break;
		default: return LIBUSB_ERROR_IO;
		}
		}
		if (done) *done = len;
		return 0;
	}
	if (nresp) {				/* answer to SET_CFG or SPI_OUT */
		memcpy(d, resp, nresp);
		if (done) *done = nresp;
		nresp = 0;
		return 0;
	}
	if (pending_in) {			/* SPI_IN data: up to 507 bytes a packet */
		unsigned int n = pending_in < CH347_MAX_DATA ? pending_in : CH347_MAX_DATA;
		if (bad == 3) n = 0;
		d[0] = bad == 2 ? CH347_CMD_SPI_OUT : CH347_CMD_SPI_IN; d[1] = (n + (bad == 4)) & 0xff; d[2] = (n + (bad == 4)) >> 8;
		for (unsigned int i = 0; i < n; i++) d[3 + i] = pattern(clocked++);
		pending_in -= n;
		if (n + 3 > max_in) max_in = n + 3;
		if (done) *done = n + 3;
		return 0;
	}
	return LIBUSB_ERROR_TIMEOUT;
}

static int fail;
static void check(int ok, const char *what) { printf("%s %s\n", ok ? "ok  " : "FAIL", what); if (!ok) fail = 1; }

int main(void)
{
	static unsigned char w[3000], r[100000];

	check(ch347_spi_init() == 0 && claimed_iface == 2 && detached_iface == 2, "CH347T: interface 2 claimed");
	check(cfg15 == (1 << 3), "default SPI clock 30 MHz (divisor 1)");
	nout = 0;
	check(ch347_cs(true) == 0 && ch347_cs(false) == 0 && nout == 0 && !cs_low, "CS low and high with nothing between: nothing sent");
	check(ch347_cs(true) == 0 && !cs_low && nout == 0, "CS low waits for the command it goes with");
	for (unsigned i = 0; i < sizeof(w); i++) w[i] = (unsigned char)(i * 3);
	nmosi = 0; clocked = 0; max_out = 0;
	check(ch347_spi_send_command(sizeof(w), 0, w, NULL) == 0 && nmosi == sizeof(w) && !memcmp(mosi, w, sizeof(w)),
	      "3000-byte write arrives in order");
	check(max_out <= CH347_PACKET_SIZE, "  in packets of at most 510 bytes");
	check(cs_low && cs_while_write_ok, "  CS low before the first byte");
	clocked = 4;
	check(ch347_spi_send_command(4, sizeof(r), w, r) == 0, "4 command bytes, then a 100000-byte read");
	int same = 1;
	for (unsigned i = 0; i < sizeof(r); i++) if (r[i] != pattern(8 + i)) { same = 0; break; }
	check(same && max_in <= CH347_PACKET_SIZE, "  read data in order, packets of at most 510 bytes");
	check(cs_while_write_ok, "  CS stays low while the data goes out");
	nmosi = 0; clocked = 0; nout_in = 0;
	unsigned char st[2];
	w[0] = 0x05;
	check(ch347_cs(false) == 0 && !cs_low && ch347_cs(true) == 0, "CS high, then low again for the next command");
	nout = 0; ncmd = 0;
	check(ch347_spi_send_command(1, 2, w, st) == 0 && nout_in == 1 && nout == 1 && ncmd == 2 && cs_low && nmosi == 3 && mosi[0] == 0x05 && mosi[1] == 0xff
	      && st[0] == pattern(1) && st[1] == pattern(2), "a short command and its answer in one full-duplex transfer, CS low in the same USB transfer");
	check(ch347_cs(false) == 0 && !cs_low, "CS high");
	bad = 1;
	check(ch347_spi_send_command(4, 0, w, NULL) < 0, "a write answered with a nonzero status fails");
	bad = 2;
	check(ch347_spi_send_command(0, 16, NULL, r) < 0, "a read answered with another command fails");
	pending_in = 0; bad = 3;
	check(ch347_spi_send_command(0, 16, NULL, r) < 0, "an empty read answer fails instead of looping");
	pending_in = 0; bad = 4;
	check(ch347_spi_send_command(0, 16, NULL, r) < 0, "a read answer shorter than its length header fails");
	pending_in = 0; bad = 0;
	check(ch347_spi_shutdown() == 0 && attached_iface == 2, "shutdown attaches the kernel driver back");

	sim_pid = 0x55de; claimed_iface = -1;
	check(ch347_set_speed("60M") == 0 && ch347_spi_init() == 0 && claimed_iface == 4 && cfg15 == 0,
	      "CH347F: interface 4; -S 60M: divisor 0");
	ch347_spi_shutdown();
	check(ch347_set_speed("1.875M") == 0 && ch347_set_speed("100M") < 0, "-S accepts the listed speeds only");
	sim_pid = 0;
	check(ch347_spi_init() < 0, "no CH347: init fails");

	/* Android: the device the USB manager opened, by its file descriptor */
	ms_usb_fd = 42; ms_usb_vid = 0x1a86; ms_usb_pid = 0x55de; claimed_iface = -1;
	check(ch347_spi_init() == 0 && wrapped_fd == 42 && no_discovery && claimed_iface == 4,
	      "Android: the CH347F from its file descriptor, no device discovery, interface 4");
	ch347_spi_shutdown();
	ms_usb_pid = 0x55db; wrapped_fd = -1;
	check(ch347_spi_init() == 0 && wrapped_fd == 42 && claimed_iface == 2, "  a CH347T: interface 2");
	ch347_spi_shutdown();
	ms_usb_vid = 0x1fc8; ms_usb_pid = 0x310b; wrapped_fd = -1;
	check(ch347_spi_init() < 0 && wrapped_fd == -1, "  the opened device is not a CH347: init fails");
	ms_usb_fd = -1;

	if (!fail) printf("all ch347sim checks passed\n");
	return fail;
}

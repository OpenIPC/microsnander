/* tests/ezpsim.c — src/ezp.c against a fake libusb that plays an EZP
   programmer: command packets on 0x02, answers and read data on 0x82,
   program data on 0x01, a 1 MB chip behind it. */
#include <stdlib.h>
#include "../src/ezp.c"

#define SIM_SIZE (1 << 20)
static unsigned char mem[SIM_SIZE];
static unsigned char answer[EZP_PKT];
static int answer_ready, streaming, busy, resets, erases, claimed = -1;
static unsigned long addr;

unsigned int bsize;

const char *snor_lookup(const unsigned char id[3], unsigned long *size, unsigned long *sector, int *addr4b)
{
	if (id[0] != 0xef || id[1] != 0x40 || id[2] != 0x14)
		return NULL;
	*size = SIM_SIZE; *sector = 65536; *addr4b = 0;
	return "W25Q80";
}

int libusb_init(libusb_context **c) { (void)c; return 0; }
void libusb_exit(libusb_context *c) { (void)c; }
const char *libusb_error_name(int e) { (void)e; return "LIBUSB_ERROR"; }
libusb_device_handle *libusb_open_device_with_vid_pid(libusb_context *c, uint16_t v, uint16_t p)
{ (void)c; return v == EZP_VID && p == 0x310b ? (libusb_device_handle *)1 : NULL; }
void libusb_close(libusb_device_handle *h) { (void)h; }
int libusb_detach_kernel_driver(libusb_device_handle *h, int i) { (void)h; (void)i; return LIBUSB_ERROR_NOT_FOUND; }
int libusb_attach_kernel_driver(libusb_device_handle *h, int i) { (void)h; (void)i; return 0; }
int libusb_claim_interface(libusb_device_handle *h, int i) { (void)h; claimed = i; return 0; }
int libusb_release_interface(libusb_device_handle *h, int i) { (void)h; (void)i; claimed = -1; return 0; }
int libusb_clear_halt(libusb_device_handle *h, unsigned char ep) { (void)h; (void)ep; return 0; }
static int strings_read;
int libusb_control_transfer(libusb_device_handle *h, uint8_t rt, uint8_t rq, uint16_t v, uint16_t i, unsigned char *d, uint16_t n, unsigned int t)
{ (void)h; (void)rt; (void)i; (void)d; (void)n; (void)t; if (rq == LIBUSB_REQUEST_GET_DESCRIPTOR && (v >> 8) == LIBUSB_DT_STRING) strings_read++; return 0; }

static unsigned long be(const unsigned char *p, int n) { unsigned long v = 0; while (n--) v = v << 8 | *p++; return v; }

int libusb_bulk_transfer(libusb_device_handle *h, unsigned char ep, unsigned char *d, int len, int *done, unsigned int timeout)
{
	(void)h; (void)timeout;
	*done = 0;
	if (ep == EZP_EP_CMD) {
		if (len != EZP_PKT) return LIBUSB_ERROR_IO;
		memset(answer, 0, sizeof(answer));
		switch (d[1]) {
		case EZP_OP_DETECT: answer[1] = 0xef; answer[2] = 0x40; answer[3] = 0x14; answer_ready = 1; streaming = 0; break;
		case EZP_OP_SETUP: answer_ready = 1; break;
		case EZP_OP_START:	/* as the real one: only a multiple of 256 starts right */
			addr = be(d + 8, 4); if (addr % 256) addr = addr + 0x1000;
			streaming = 1; answer_ready = 1; break;
		case EZP_OP_STATUS: answer[0] = busy > 0; if (busy) busy--; answer_ready = 1; break;
		case EZP_OP_CHIP_ERASE: memset(mem, 0xff, sizeof(mem)); erases++; busy = 3; streaming = 0; break;
		case EZP_OP_RESET: resets++; streaming = 0; break;
		default: return LIBUSB_ERROR_IO;
		}
		*done = len;
		return 0;
	}
	if (ep == EZP_EP_DATA_OUT) {
		if (!streaming) return LIBUSB_ERROR_IO;
		for (int i = 0; i < len; i++) mem[(addr + i) % SIM_SIZE] &= d[i];
		addr += len; busy = 2;
		answer_ready = 0;		/* a program stream: the start answer is dropped */
		*done = len;
		return 0;
	}
	if (ep == EZP_EP_IN) {
		if (answer_ready) {
			if (len < EZP_PKT) return LIBUSB_ERROR_OVERFLOW;
			memcpy(d, answer, EZP_PKT); answer_ready = 0; *done = EZP_PKT;
			return 0;
		}
		if (streaming) {
			if (len % EZP_STREAM_UNIT) return LIBUSB_ERROR_OVERFLOW;	/* the units are whole */
			for (int i = 0; i < len; i++) d[i] = mem[(addr + i) % SIM_SIZE];
			addr += len; *done = len;
			return 0;
		}
	}
	return LIBUSB_ERROR_TIMEOUT;
}

static int fail;
static void check(int ok, const char *what) { printf("%s %s\n", ok ? "ok  " : "FAIL", what); if (!ok) fail = 1; }

int main(void)
{
	static unsigned char w[5000], r[100000];
	struct flash_cmd cmd = { 0 };

	memset(mem, 0x5a, sizeof(mem));
	check(ezp_init() == 0 && claimed == 0 && strings_read == 2, "EZP2019: its strings read, interface 0 claimed");
	check(ezp_flash_init(&cmd) == SIM_SIZE && bsize == 65536 && !cmd.flash_unprotect, "the chip from the NOR table by its ID, no unprotect");

	check(cmd.flash_erase(0, 65536) < 0 && erases == 0 && mem[0] == 0x5a, "a part of the chip is not erased (only whole-chip erase)");
	check(cmd.flash_write(w, 0, 16) < 0 && mem[0] == 0x5a, "a write over data that is not erased is refused");
	check(cmd.flash_erase(0, SIM_SIZE) == 0 && erases == 1 && mem[0] == 0xff && mem[SIM_SIZE - 1] == 0xff, "chip erase");

	for (unsigned i = 0; i < sizeof(w); i++) w[i] = (unsigned char)(i * 7 + 3);
	check(cmd.flash_write(w, 0x1001, 4097) == 4097, "write of 4097 bytes at 0x1001");
	check(!memcmp(mem + 0x1001, w, 4097) && mem[0x1000] == 0xff && mem[0x1001 + 4097] == 0xff, "  lands in place, the bytes around stay erased");
	memset(r, 0, sizeof(r));
	check(cmd.flash_read(r, 0x1001, 4097) == 4097 && !memcmp(r, w, 4097), "  reads back");
	check(cmd.flash_write(w, 0x1001, 16) < 0 && !memcmp(mem + 0x1001, w, 16), "a second write to the same range is refused");
	check(cmd.flash_write(w, 0x2002, 100) == 100 && !memcmp(mem + 0x2002, w, 100) && !memcmp(mem + 0x1001, w, 4097),
	      "a write next to it, in the same page, keeps the data before it");

	for (unsigned i = 0; i < SIM_SIZE; i++) mem[i] = (unsigned char)(i * 13 + (i >> 8));
	check(cmd.flash_read(r, 12345, sizeof(r)) == (int)sizeof(r), "read of 100000 bytes at an odd address");
	int same = 1;
	for (unsigned i = 0; i < sizeof(r); i++) if (r[i] != mem[12345 + i]) { same = 0; break; }
	check(same, "  the data in order");

	check(ezp_shutdown() == 0 && resets == 1 && claimed == -1, "shutdown resets the programmer and releases it");

	if (!fail) printf("all ezpsim checks passed\n");
	return fail;
}

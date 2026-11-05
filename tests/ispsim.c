/* tests/ispsim.c — src/sigmastar.c against a fake I2C master with a SigmaStar
   ISP bridge on it: 0x10 + bytes go out on the SPI bus, 0x11 makes the next
   read clock bytes in, 0x12 ends the command (CS high); behind it a SPI NOR
   that answers READ ID (9F) and READ (03) from a pattern. The debug port 0x59
   does not answer a probe, so the attach goes through its unlock sequence. */
#include <stdio.h>
#include <stdlib.h>
#include "../src/sigmastar.c"

static int fail;
static void check(int ok, const char *what) { printf("%s %s\n", ok ? "ok  " : "FAIL", what); if (!ok) fail = 1; }

static unsigned char pattern(unsigned int i) { return (unsigned char)(i * 13 + 7); }

static unsigned char cmd[600];			/* the SPI bytes of the command under CS */
static unsigned int ncmd, clocked;		/* bytes clocked in since the command */
static int reading, ends, max_part, writes59, mstar, bad;
static unsigned char log59[64];
static unsigned int nlog59;
static int answer59 = 1;			/* the register read at 0x59 after the UART setup */

static int refuse_mstar, refuse_end, answers59;

static int f_speed(unsigned int s) { return s > 3 ? -1 : 0; }

static int f_write(unsigned char a, const unsigned char *b, unsigned int n)
{
	if ((int)n > max_part) max_part = n;
	if (n > 24) bad = 1;			/* past the I2C limit given below */
	if (a == 0x59) {
		writes59++;
		for (unsigned int i = 0; i < n && nlog59 < sizeof(log59); i++) log59[nlog59++] = b[i];
		if (n == 6) answer59 = 0;		/* the second UART write: the register is clear */
		return 0;
	}
	if (a != 0x49)
		return -1;
	if (n == 5 && !memcmp(b, "MSTAR", 5)) { if (refuse_mstar) return -1; mstar++; return 0; }
	switch (b[0]) {
	case 0x10: if (reading) bad = 2; memcpy(cmd + ncmd, b + 1, n - 1); ncmd += n - 1; return 0;
	case 0x11: if (n != 1) bad = 3; reading = 1; return 0;
	case 0x12: if (refuse_end) return -1; if (n != 1) bad = 4; ends++; ncmd = clocked = 0; reading = 0; return 0;
	default: bad = 5; return -1;
	}
}

static int f_read(unsigned char a, unsigned char *b, unsigned int n)
{
	if (a == 0x59) {
		if (!n) return answers59 ? 0 : -1;	/* the probe: no answer before the unlock */
		b[0] = answer59;
		return 0;
	}
	if (a != 0x49) return -1;
	if (!n) return 0;
	if (!reading) { bad = 6; return -1; }
	if (n > 24) bad = 1;
	for (unsigned int i = 0; i < n; i++, clocked++)
		if (ncmd == 1 && cmd[0] == 0x9f)
			b[i] = clocked < 3 ? (unsigned char[]){ 0xef, 0x40, 0x18 }[clocked] : 0;
		else if (ncmd == 4 && cmd[0] == 0x03)
			b[i] = pattern(((cmd[1] << 16) | (cmd[2] << 8) | cmd[3]) + clocked);
		else
			b[i] = 0xff;
	return 0;
}

static const struct programmer fake = {
	.key = "ch341a", .name = "CH341A", .send = (void *)1, .cs = (void *)1,
	.i2c_speed = f_speed, .i2c_write = f_write, .i2c_read = f_read, .i2c_max = 24,
};
static const struct programmer noi2c = { .key = "ezp", .name = "EZP" };

int main(void)
{
	const struct programmer *p;
	unsigned char id[3], buf[200], wr[300];

	check(sigmastar_mode("mstar") < 0 && sigmastar_mode("SigmaStar") == 0 && sigmastar_on(), "-m sigmastar only");
	check(sigmastar_set_speed("400k") == 0 && sigmastar_set_speed("15M") < 0, "-S takes the I2C clocks");
	check(!sigmastar_attach(&noi2c), "a programmer without I2C is refused");

	p = sigmastar_attach(&fake);
	check(p && p->send != fake.send && p->cs != fake.cs && !p->clock && mstar == 1, "attach: send and cs go to the bridge, MSTAR sent once");
	check(nlog59 >= 5 && !memcmp(log59, "SERDB", 5) && writes59 == 12, "  the silent debug port 0x59 got its unlock sequence");

	/* READ ID: one byte out, three in, then CS high */
	check(!p->cs(true) && !p->send(1, 3, (const unsigned char[]){ 0x9f }, id) && !p->cs(false) &&
	      id[0] == 0xef && id[1] == 0x40 && id[2] == 0x18 && ends == 1, "READ ID through the bridge: ef 40 18, then 0x12");

	/* READ of 200 bytes at 0x1234: the read goes in parts of the I2C limit, each after a 0x11 */
	int same = 1;
	p->cs(true);
	check(!p->send(4, sizeof(buf), (const unsigned char[]){ 0x03, 0x00, 0x12, 0x34 }, buf) && !p->cs(false), "READ of 200 bytes");
	for (unsigned int i = 0; i < sizeof(buf); i++) same &= buf[i] == pattern(0x1234 + i);
	check(same && max_part <= 24 && !bad, "  the data comes back in order, no I2C transaction over 24 bytes");

	/* a page program: 4 bytes of command and 256 of data out in parts of 23, all under one CS */
	for (unsigned int i = 0; i < sizeof(wr); i++) wr[i] = (unsigned char)i;
	int before = ends;
	ncmd = 0;
	p->cs(true);
	int ok = !p->send(260, 0, wr, NULL);
	check(ok && ncmd == 260 && !memcmp(cmd, wr, 260) && ends == before, "a write of 260 bytes goes out whole, before the CS goes high");
	p->cs(false);
	check(ends == before + 1 && !bad, "  then one 0x12");

	/* a refused MSTAR: the end of a command, and on; refused too: no bridge */
	refuse_mstar = 1;
	before = ends;
	check(sigmastar_attach(&fake) && ends == before + 1, "a refused MSTAR: 0x12, and the bridge goes on");
	refuse_end = 1;
	check(!sigmastar_attach(&fake), "  the end refused too: no bridge");
	refuse_mstar = refuse_end = 0;

	/* a debug port that answers gets no unlock */
	answers59 = 1;
	before = writes59;
	check((p = sigmastar_attach(&fake)) && writes59 == before && p->on_board, "a debug port that answers is left alone");

	/* a failed end of a command stops what comes next, whoever looked at it */
	refuse_end = 1;
	check(p->cs(false) && p->send(1, 3, (const unsigned char[]){ 0x9f }, id), "a failed 0x12: the next command fails");
	refuse_end = 0;
	check(sigmastar_attach(&fake) && !p->send(1, 3, (const unsigned char[]){ 0x9f }, id), "  a new attach starts clean");

	return fail;
}

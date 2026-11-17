package org.openipc.microsnander;

/**
 * The operations of microsnander (src/ops.c) on the USB device the app
 * opened. One at a time (synchronized): the native code has one programmer
 * and one chip.
 */
final class Native {
    static {
        System.loadLibrary("microsnander");
    }

    private Native() {
    }

    /** What an operation found and how it ended. */
    static final class Result {
        final boolean ok;
        final String message;
        final String programmer;
        final String chip;
        final long size;
        /** SR1 << 16 | SR2 << 8 | SR3, bit 24: Macronix, only SR1 counts; -1 when they cannot be read */
        final int sr;
        /** for a read: the dump name, flash-YYYYMMDDHHMM-CHIP.bin; for a self-test: the block
         *  that did not come back, selftest-CHIP-0x0.bin, or "" */
        final String name;
        /** the chip's supply: "3.3", "1.8", "1.65-3.6"; "" without a chip */
        final String voltage;
        /** a 1.8 V chip, the 5 V lines of the CH341A board; "" */
        final String warning;

        Result(boolean ok, String message, String programmer, String chip, long size, int sr, String name,
               String voltage, String warning) {
            this.ok = ok;
            this.message = message;
            this.programmer = programmer;
            this.chip = chip;
            this.size = size;
            this.sr = sr;
            this.name = name;
            this.voltage = voltage;
            this.warning = warning;
        }
    }

    /** Called from the operation's thread. */
    interface Progress {
        void progress(String phase, long done, long total);
    }

    static synchronized native Result detect(int fd, int vid, int pid);

    static synchronized native Result read(int fd, int vid, int pid, String path, Progress progress);

    /** the chip the next operation must find, or it does nothing (Erase and Write) */
    static synchronized native void expect(String chip, long size);

    static synchronized native Result erase(int fd, int vid, int pid, Progress progress);

    static synchronized native Result write(int fd, int vid, int pid, String path, Progress progress);

    static synchronized native Result registers(int fd, int vid, int pid);

    static synchronized native Result unprotect(int fd, int vid, int pid);

    /** the bits mask of status register reg (0-2: SR1-SR3) set to those of value on every die, each
     *  die keeps its other bits; for the chip of expect(); WIP and WEL stay the chip's, LB1-LB3 are
     *  never set */
    static synchronized native Result writeRegister(int fd, int vid, int pid, int reg, int mask, int value);

    /** -t on the first block (ms_expect first); a block that does not come back is saved in dir */
    static synchronized native Result selftest(int fd, int vid, int pid, String dir);

    // ---- the web mode (remote access, RemoteService); not synchronized: webServe blocks ----

    /** the page on listen ("[address:]port") until webStop: 0, or 1 when it cannot listen */
    static native int webServe(String listen, byte[] password, String dir);

    static native void webStop();

    /** the state of the last operation, the app's or the page's: the JSON of /api/state */
    static native String state();

    /** the state of the camex tunnel, for the page ("" none) */
    static native void setTunnel(String text);

    /** the camex server's host: the page takes Host names under it without a password ("" none) */
    static native void webTunnelHost(String host);

    /** a dump the app kept in the dump directory: the page shows it as the last one */
    static native void dumpKept(String name);
}

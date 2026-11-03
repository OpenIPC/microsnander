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
        /** SR1 << 16 | SR2 << 8 | SR3, or -1 when they cannot be read */
        final int sr;
        /** for a read: the dump name, flash-YYYYMMDDHHMM-CHIP.bin */
        final String name;

        Result(boolean ok, String message, String programmer, String chip, long size, int sr, String name) {
            this.ok = ok;
            this.message = message;
            this.programmer = programmer;
            this.chip = chip;
            this.size = size;
            this.sr = sr;
            this.name = name;
        }
    }

    /** Called from the operation's thread. */
    interface Progress {
        void progress(String phase, long done, long total);
    }

    static synchronized native Result detect(int fd, int vid, int pid);

    static synchronized native Result read(int fd, int vid, int pid, String path, Progress progress);

    static synchronized native Result erase(int fd, int vid, int pid, Progress progress);

    static synchronized native Result write(int fd, int vid, int pid, String path, Progress progress);

    static synchronized native Result registers(int fd, int vid, int pid);

    static synchronized native Result unprotect(int fd, int vid, int pid);
}

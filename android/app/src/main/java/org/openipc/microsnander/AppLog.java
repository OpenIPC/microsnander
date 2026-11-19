package org.openipc.microsnander;

import android.os.Handler;
import android.os.Looper;
import android.text.TextUtils;
import java.text.SimpleDateFormat;
import java.util.ArrayDeque;
import java.util.Date;
import java.util.Locale;

/**
 * The log of events under the panel (Show log): the app, the operations,
 * remote access and its port, the tunnel. "HH:mm:ss what: text", the last
 * LINES of them, for as long as the process lives; from any thread.
 */
final class AppLog {
    static final int LINES = 200;
    private static final ArrayDeque<String> lines = new ArrayDeque<>();
    /** how many lines left the top since the start: the window keeps its place by it */
    private static long removed;   // guarded by lines
    private static final Handler MAIN = new Handler(Looper.getMainLooper());
    /** the screen that shows the log, told on the main thread; null if none */
    private static volatile Runnable listener;

    private AppLog() {
    }

    static void add(String what, String text) {
        String l = new SimpleDateFormat("HH:mm:ss", Locale.ROOT).format(new Date()) + " " + what + ": " + text;
        synchronized (lines) {
            if (lines.size() == LINES) {
                lines.removeFirst();
                removed++;
            }
            lines.addLast(l);
        }
        Runnable r = listener;
        if (r != null)
            MAIN.post(r);
    }

    /** the log, oldest first, and how many lines had left its top by then (one snapshot) */
    static final class Snapshot {
        final String text;
        final long removed;

        Snapshot(String text, long removed) {
            this.text = text;
            this.removed = removed;
        }
    }

    static Snapshot snapshot() {
        synchronized (lines) {
            return new Snapshot(TextUtils.join("\n", lines), removed);
        }
    }

    static synchronized void listen(Runnable r) {
        listener = r;
    }

    /** r stops listening, if it still is (a new screen may have taken its place) */
    static synchronized void unlisten(Runnable r) {
        if (listener == r)
            listener = null;
    }
}

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
    private static final Handler MAIN = new Handler(Looper.getMainLooper());
    /** the screen that shows the log, told on the main thread; null if none */
    private static volatile Runnable listener;

    private AppLog() {
    }

    static void add(String what, String text) {
        String l = new SimpleDateFormat("HH:mm:ss", Locale.ROOT).format(new Date()) + " " + what + ": " + text;
        synchronized (lines) {
            if (lines.size() == LINES)
                lines.removeFirst();
            lines.addLast(l);
        }
        Runnable r = listener;
        if (r != null)
            MAIN.post(r);
    }

    /** the log, oldest first */
    static String text() {
        synchronized (lines) {
            return TextUtils.join("\n", lines);
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

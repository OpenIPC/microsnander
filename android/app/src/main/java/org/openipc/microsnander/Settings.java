package org.openipc.microsnander;

import android.content.Context;
import android.content.SharedPreferences;

/**
 * The settings of remote access: on or off, the port and the password of
 * the web page. Remote access runs only when it is on and has a password.
 */
final class Settings {
    static final int DEFAULT_PORT = 8080;
    static final int MIN_PASSWORD = 4;

    private Settings() {
    }

    private static SharedPreferences prefs(Context c) {
        return c.getApplicationContext().getSharedPreferences("remote", Context.MODE_PRIVATE);
    }

    static boolean enabled(Context c) {
        return prefs(c).getBoolean("enabled", false);
    }

    static int port(Context c) {
        return prefs(c).getInt("port", DEFAULT_PORT);
    }

    static String password(Context c) {
        return prefs(c).getString("password", "");
    }

    /** on, with a password: the service serves the page */
    static boolean active(Context c) {
        return enabled(c) && password(c).length() >= MIN_PASSWORD;
    }

    static void save(Context c, boolean enabled, int port, String password) {
        prefs(c).edit().putBoolean("enabled", enabled).putInt("port", port).putString("password", password).apply();
    }
}

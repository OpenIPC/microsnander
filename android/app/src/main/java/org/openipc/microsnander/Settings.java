package org.openipc.microsnander;

import android.content.Context;
import android.content.SharedPreferences;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import org.openipc.camex.tunnel.CamexTunnel;
import org.openipc.camex.tunnel.Field;

/**
 * The settings of remote access: on or off, the port and the password of
 * the web page. Remote access runs when it is on; the password is optional:
 * without one the page is open to anyone who reaches the port (a lab tool on
 * a trusted network: it guards against accidents, not attacks).
 * The camex tunnel (the device joins a camex server, the operator reaches the
 * page through it) runs only with remote access, when it is on too.
 */
final class Settings {
    /** the same as vectis and decoder: camex's nginx finds the page by it */
    static final int DEFAULT_PORT = 8085;

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

    /** on: the service serves the page */
    static boolean active(Context c) {
        return enabled(c);
    }

    static void save(Context c, boolean enabled, int port, String password) {
        prefs(c).edit().putBoolean("enabled", enabled).putInt("port", port).putString("password", password).apply();
    }

    /** how many ports from the settings' one the page tries: 8085 taken (another of our
     *  apps), 8086, and so on (ANDROID.md, section 12) */
    static final int PORTS = 16;

    /** the port the page listens on now: the settings' one or the next free; 0 not listening */
    static volatile int webPort;

    /**
     * Start on boot: remote access starts after a reboot. Off by default; an install from
     * before the check keeps what it did, which was to start when remote access was on.
     */
    static boolean startOnBoot(Context c) {
        SharedPreferences p = prefs(c);
        if (!p.contains("boot"))
            p.edit().putBoolean("boot", enabled(c)).apply();
        return p.getBoolean("boot", enabled(c));
    }

    static void setStartOnBoot(Context c, boolean on) {
        prefs(c).edit().putBoolean("boot", on).apply();
    }

    /** the log of events under the panel; off by default */
    static boolean showLog(Context c) {
        return prefs(c).getBoolean("show_log", false);
    }

    static void setShowLog(Context c, boolean on) {
        prefs(c).edit().putBoolean("show_log", on).apply();
    }

    // ---- the camex tunnel ----

    /** the keys of CamexTunnel kept here: every field of the library, so a new one needs no
     *  change here; but the web port, which is the page's port */
    static List<String> tunnelKeys() {
        List<String> k = new ArrayList<>();
        for (Field f : CamexTunnel.fields())
            if (!CamexTunnel.WEB_PORT.equals(f.key))
                k.add(f.key);
        return k;
    }

    static boolean tunnelEnabled(Context c) {
        return prefs(c).getBoolean("camex", false);
    }

    /** remote access runs and the tunnel is on */
    static boolean tunnelActive(Context c) {
        return active(c) && tunnelEnabled(c);
    }

    static Map<String, String> tunnel(Context c) {
        Map<String, String> v = new HashMap<>();
        for (String k : tunnelKeys())
            v.put(k, prefs(c).getString("camex." + k, CamexTunnel.TRANSPORT.equals(k) ? "UDP" : ""));
        // the server's nginx opens the page by the client's name on this port (camex 3.19.0+),
        // and the tunnel lets new connections through to it only (camex 4.1.0+): the port the
        // page listens on, which is the next free one when the settings' one is taken
        v.put(CamexTunnel.WEB_PORT, String.valueOf(webPort > 0 ? webPort : port(c)));
        return v;
    }

    static void saveTunnel(Context c, boolean on, Map<String, String> v) {
        SharedPreferences.Editor e = prefs(c).edit().putBoolean("camex", on);
        for (String k : tunnelKeys())
            e.putString("camex." + k, v.containsKey(k) ? v.get(k) : "");
        e.apply();
    }
}

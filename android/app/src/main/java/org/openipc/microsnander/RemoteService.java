package org.openipc.microsnander;

import android.annotation.SuppressLint;
import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.hardware.usb.UsbDevice;
import android.hardware.usb.UsbDeviceConnection;
import android.hardware.usb.UsbManager;
import android.os.Build;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.os.PowerManager;
import android.text.TextUtils;
import android.util.Log;
import java.io.File;
import java.net.Inet4Address;
import java.net.InetAddress;
import java.net.NetworkInterface;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.Map;
import org.openipc.camex.tunnel.CamexTunnel;

/**
 * Remote access: the web mode of microsnander (src/web.c) in a foreground
 * service, on every interface of the device, with the password of the
 * settings, or open without one. An operator anywhere the device is reachable from (the local
 * network, a tunnel) opens the page and drives the programmer; nobody has
 * to be at the device.
 *
 * Each operation of the page opens the programmer here (openDevice, from the
 * native thread of the operation) and closes it after (closeDevice), as the
 * app's own buttons do, so it can be replugged between operations. Access to
 * it comes from "use by default" when it was plugged in (UsbAttachActivity);
 * without it the operation fails and says so.
 *
 * With the camex tunnel on too, the device joins the camex server of the
 * settings (org.openipc.camex.tunnel, the camex sources in bundles/camex) and
 * the page is reached through it as well: the tunnel runs while the page
 * does, reconnects by itself, and shares this service's notification.
 * Without the VPN consent (a dialog once, or adb appops ACTIVATE_VPN allow)
 * it says so on the screen, in the notification and on the page, and tries
 * again every minute.
 */
public class RemoteService extends Service {
    static final String ACTION_START = "org.openipc.microsnander.REMOTE_START";
    static final String ACTION_STOP = "org.openipc.microsnander.REMOTE_STOP";
    /** the settings changed: serve again with the new ones */
    static final String ACTION_RESTART = "org.openipc.microsnander.REMOTE_RESTART";
    private static final String TAG = "microsnander";
    private static final String CHANNEL = "remote";
    private static final int NOTIFY_ID = 1;
    private static final int RETRY_MS = 5000;
    /** a tunnel without the consent (or refused) is started again this often */
    private static final int TUNNEL_RETRY_MS = 60000;

    /** the running service, for openDevice and the screen; null if none */
    static volatile RemoteService instance;
    /** what the screen shows about remote access */
    static volatile String status = "";
    /** what the screen shows about the tunnel; "" when it is off */
    static volatile String tunnelStatus = "";

    private final Handler main = new Handler(Looper.getMainLooper());
    private final Object lock = new Object();
    private Thread server;              // guarded by lock: the thread in Native.webServe
    private boolean wanted;             // guarded by lock: keep serving
    private int generation;             // guarded by lock: which server thread is the current one
    private Thread stopping;            // guarded by lock: a server thread stopped, still ending
    private boolean startAfterStop;     // guarded by lock: serve again once it has ended
    private boolean finishAfterStop;    // guarded by lock: the service ends once it has ended
    private PowerManager.WakeLock wakeLock;

    // the programmer of the page's operation; one at a time (web.c runs one)
    private static final Object DEVICE = new Object();
    private static UsbDeviceConnection conn;    // guarded by DEVICE

    /** the network came or went: the addresses on the screen and in the notification */
    private final BroadcastReceiver netEvents = new BroadcastReceiver() {
        @Override
        public void onReceive(Context c, Intent i) {
            main.postDelayed(() -> {
                synchronized (lock) {
                    if (wanted && server != null)
                        setStatus(listening(Settings.port(RemoteService.this)));
                }
            }, 1500);
        }
    };

    static void send(Context c, String action) {
        Intent i = new Intent(c, RemoteService.class).setAction(action);
        if (!action.equals(ACTION_STOP) && Build.VERSION.SDK_INT >= 26)
            c.startForegroundService(i);
        else
            c.startService(i);
    }

    @Override
    @SuppressWarnings("deprecation")   // CONNECTIVITY_ACTION: the one broadcast from Android 5 on
    public void onCreate() {
        super.onCreate();
        instance = this;
        PowerManager pm = (PowerManager) getSystemService(POWER_SERVICE);
        wakeLock = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "microsnander:remote");
        wakeLock.setReferenceCounted(false);
        registerReceiver(netEvents, new IntentFilter(android.net.ConnectivityManager.CONNECTIVITY_ACTION));
        // the tunnel's foreground service posts this service's notification: one for both
        CamexTunnel.setNotification(NOTIFY_ID, (ctx, st, addr, err) -> notification(ctx));
        CamexTunnel.addListener(tunnelState);
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        String action = intent != null && intent.getAction() != null ? intent.getAction() : ACTION_START;
        foreground(getString(R.string.remote_starting));
        if (action.equals(ACTION_STOP) || !Settings.active(this)) {
            synchronized (lock) {
                startAfterStop = false;
                finishAfterStop = true;		// once a page's operation that runs has ended
            }
            stopServing();
            return START_NOT_STICKY;
        }
        synchronized (lock) {
            finishAfterStop = false;
        }
        if (action.equals(ACTION_RESTART))
            stopServing();
        startServing();
        return START_STICKY;
    }

    @Override
    public void onDestroy() {
        unregisterReceiver(netEvents);
        CamexTunnel.removeListener(tunnelState);
        stopServing();
        instance = null;
        super.onDestroy();
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }

    // ---- the server ----

    /** the wake lock is held while it serves: until Stop, as the screen may be off for days */
    @SuppressLint("WakelockTimeout")
    private void startServing() {
        synchronized (lock) {
            if (stopping != null) {		// the old server still ends: after it, not over it
                startAfterStop = true;
                return;
            }
            if (server != null) {		// serving: the tunnel may have been turned on or changed
                main.post(this::startTunnel);
                return;
            }
            wanted = true;
            int gen = ++generation;
            int port = Settings.port(this);
            String password = Settings.password(this);
            String dir = new File(getFilesDir(), "dumps").getPath();	// the app's dumps, the same as its own
            wakeLock.acquire();
            server = new Thread(() -> serve(gen, port, password, dir), "microsnander-web");
            server.start();
            setStatus(listening(port));
        }
        startTunnel();
    }

    /** Native.webServe until webStop; a port it cannot take is tried again */
    private void serve(int gen, int port, String password, String dir) {
        while (true) {
            // UTF-8 as a browser sends it (JNI strings are modified UTF-8: an emoji would differ)
            int ret = Native.webServe("0.0.0.0:" + port, password.getBytes(StandardCharsets.UTF_8), dir);
            synchronized (lock) {
                if (!wanted || gen != generation)
                    return;
                if (ret == 0)		// a stop meant for the server before this one: serve again
                    continue;
                setStatus(getString(R.string.remote_cannot_listen, port));
            }
            Log.w(TAG, "web mode: cannot listen on port " + port + ", again in " + RETRY_MS / 1000 + " s");
            try {
                Thread.sleep(RETRY_MS);
            } catch (InterruptedException e) {
                return;
            }
            synchronized (lock) {
                if (!wanted || gen != generation)
                    return;
                setStatus(listening(port));
            }
        }
    }

    private void stopServing() {
        main.removeCallbacks(retryTunnel);
        if (CamexTunnel.running() || CamexTunnel.state() != CamexTunnel.State.STOPPED)
            CamexTunnel.stop();
        Thread t;
        synchronized (lock) {
            wanted = false;
            generation++;
            t = server;
            server = null;
            if (t == null) {
                if (stopping == null)
                    main.post(this::stopped);
                return;
            }
            stopping = t;
        }
        setStatus(getString(R.string.remote_stopping));
        // webServe returns once a page's operation that runs has ended (a write may take
        // minutes): waited for off the main thread, with the wake lock still held
        new Thread(() -> {
            Native.webStop();
            t.interrupt();
            boolean done = false;
            while (!done) {
                try {
                    t.join();
                    done = true;
                } catch (InterruptedException ignored) {
                    // keep waiting: the native server still uses the settings
                }
            }
            synchronized (lock) {
                stopping = null;
            }
            main.post(this::stopped);
        }, "microsnander-stop").start();
    }

    /** the old server has ended: serve again, or let the wake lock and the service go */
    private void stopped() {
        boolean again, finish;
        synchronized (lock) {
            if (stopping != null || server != null)
                return;
            again = startAfterStop && Settings.active(this);
            finish = finishAfterStop;
            startAfterStop = false;
        }
        if (again) {
            startServing();
            return;
        }
        if (wakeLock.isHeld())
            wakeLock.release();
        if (finish) {
            setStatus("");
            if (Build.VERSION.SDK_INT >= 24)
                stopForeground(STOP_FOREGROUND_REMOVE);
            else
                stopForeground(true);
            stopSelf();
        }
    }

    /** "Listening: http://…", and that the page is open when it has no password */
    private String listening(int port) {
        return getString(Settings.password(this).isEmpty() ? R.string.remote_on_open : R.string.remote_on,
                addresses(port));
    }

    /** http://ADDRESS:PORT of each IPv4 address of the device (Ethernet, Wi-Fi, a tunnel) */
    static String addresses(int port) {
        List<String> l = new ArrayList<>();
        try {
            for (NetworkInterface n : Collections.list(NetworkInterface.getNetworkInterfaces())) {
                if (!n.isUp() || n.isLoopback())
                    continue;
                for (InetAddress a : Collections.list(n.getInetAddresses()))
                    if (a instanceof Inet4Address)
                        l.add("http://" + a.getHostAddress() + ":" + port);
            }
        } catch (Exception e) {
            // no list: the port alone
        }
        return l.isEmpty() ? ":" + port : TextUtils.join("  ", l);
    }

    private void setStatus(String s) {
        status = s;
        main.post(() -> {
            if (!s.isEmpty())
                foreground(s);
            MainActivity.remoteChanged();
        });
    }

    // ---- the camex tunnel ----

    /** the tunnel of the settings, when it is on; a start refused now is tried again later */
    private void startTunnel() {
        main.removeCallbacks(retryTunnel);
        if (!Settings.tunnelActive(this)) {
            if (CamexTunnel.state() != CamexTunnel.State.STOPPED)
                CamexTunnel.stop();
            showTunnel(CamexTunnel.State.STOPPED, "", "");
            Native.webTunnelHost("");
            return;
        }
        Map<String, String> v = Settings.tunnel(this);
        // the server's nginx opens the page as client.server: those names pass the Host check
        // without a password too
        String server = v.get(CamexTunnel.SERVER) == null ? "" : v.get(CamexTunnel.SERVER).trim();
        int colon = server.lastIndexOf(':');
        Native.webTunnelHost(colon > 0 ? server.substring(0, colon) : server);
        if (CamexTunnel.running() && v.equals(tunnelValues))
            return;		// up with these values: a plug-in of the programmer does not cut it
        tunnelValues = v;
        if (!CamexTunnel.start(this, v))
            main.postDelayed(retryTunnel, TUNNEL_RETRY_MS);	// no consent yet: adb may give it meanwhile
    }

    /** the values the tunnel was last started with (main thread) */
    private Map<String, String> tunnelValues;

    private final Runnable retryTunnel = () -> {
        synchronized (lock) {
            if (!wanted)
                return;
        }
        CamexTunnel.State s = CamexTunnel.state();
        if (s == CamexTunnel.State.NEEDS_CONSENT || s == CamexTunnel.State.FAILED || s == CamexTunnel.State.STOPPED)
            startTunnel();
    };

    private final CamexTunnel.Listener tunnelState = (st, addr, err) -> {
        showTunnel(st, addr, err);
        if (st == CamexTunnel.State.NEEDS_CONSENT && Settings.tunnelActive(this)) {
            main.removeCallbacks(retryTunnel);	// consent revoked while it ran
            main.postDelayed(retryTunnel, TUNNEL_RETRY_MS);
        }
    };

    private void showTunnel(CamexTunnel.State st, String addr, String err) {
        String t;
        if (!Settings.tunnelActive(this))
            t = "";
        else if (st == CamexTunnel.State.CONNECTED)
            t = getString(R.string.tunnel_connected, "http://" + addr + ":" + Settings.port(this));
        else if (st == CamexTunnel.State.CONNECTING)
            t = getString(R.string.tunnel_connecting, err.isEmpty() ? "" : " (" + err + ")");
        else if (st == CamexTunnel.State.NEEDS_CONSENT)
            t = getString(R.string.tunnel_needs_consent);
        else if (st == CamexTunnel.State.FAILED)
            t = getString(R.string.tunnel_failed, err);
        else
            t = getString(R.string.tunnel_stopped);
        tunnelStatus = t;
        Native.setTunnel(t);
        main.post(() -> {
            if (instance != null)
                foreground(status);
            MainActivity.remoteChanged();
        });
    }

    // ---- the notification ----

    private void foreground(String text) {
        if (!text.isEmpty())
            status = text;
        startForeground(NOTIFY_ID, notification(this));
    }

    /** the one notification of remote access and its tunnel: the page's addresses, the tunnel's state */
    @SuppressWarnings("deprecation")   // Notification.Builder(Context): the one before Android 8
    static Notification notification(Context c) {
        NotificationManager nm = (NotificationManager) c.getSystemService(NOTIFICATION_SERVICE);
        Notification.Builder b;
        if (Build.VERSION.SDK_INT >= 26) {
            nm.createNotificationChannel(new NotificationChannel(CHANNEL, c.getString(R.string.remote_access),
                    NotificationManager.IMPORTANCE_LOW));
            b = new Notification.Builder(c, CHANNEL);
        } else
            b = new Notification.Builder(c);
        int f = Build.VERSION.SDK_INT >= 23 ? PendingIntent.FLAG_IMMUTABLE : 0;
        String text = status.isEmpty() ? c.getString(R.string.remote_starting) : status;
        if (!tunnelStatus.isEmpty())
            text += "\n" + tunnelStatus;
        b.setSmallIcon(R.drawable.ic_notify)
         .setContentTitle(c.getString(R.string.remote_access))
         .setContentText(text)
         .setStyle(new Notification.BigTextStyle().bigText(text))
         .setOngoing(true)
         .setContentIntent(PendingIntent.getActivity(c, 0, new Intent(c, MainActivity.class), f));
        return b.build();
    }

    // ---- the programmer of the page's operations (from the native thread) ----

    private static final int[][] PROGRAMMERS = MainActivity.PROGRAMMERS;

    /** the programmer opened for one operation: fd << 32 | vid << 16 | pid, or -1 (none, no access) */
    @SuppressLint("MissingPermission")
    static long openDevice() {
        RemoteService s = instance;
        if (s == null)
            return -1;
        UsbManager usb = (UsbManager) s.getSystemService(Context.USB_SERVICE);
        UsbDevice d = null;
        for (UsbDevice x : usb.getDeviceList().values())
            for (int[] p : PROGRAMMERS)
                if (x.getVendorId() == p[0] && x.getProductId() == p[1])
                    d = x;
        if (d == null || !usb.hasPermission(d)) {
            if (d != null)
                Log.w(TAG, "web mode: no access to " + d.getDeviceName() + ": plug it in again and choose the app with Always");
            return -1;
        }
        synchronized (DEVICE) {
            if (conn != null)
                conn.close();
            conn = usb.openDevice(d);
            if (conn == null)
                return -1;
            return ((long) conn.getFileDescriptor() << 32) | ((long) d.getVendorId() << 16) | d.getProductId();
        }
    }

    static void closeDevice() {
        synchronized (DEVICE) {
            if (conn != null)
                conn.close();
            conn = null;
        }
    }
}

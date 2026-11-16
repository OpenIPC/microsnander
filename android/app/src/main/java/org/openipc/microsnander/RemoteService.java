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
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

/**
 * Remote access: the web mode of microsnander (src/web.c) in a foreground
 * service, on every interface of the device, with the password of the
 * settings. An operator anywhere the device is reachable from (the local
 * network, a tunnel) opens the page and drives the programmer; nobody has
 * to be at the device.
 *
 * Each operation of the page opens the programmer here (openDevice, from the
 * native thread of the operation) and closes it after (closeDevice), as the
 * app's own buttons do, so it can be replugged between operations. Access to
 * it comes from "use by default" when it was plugged in (UsbAttachActivity);
 * without it the operation fails and says so.
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

    /** the running service, for openDevice and the screen; null if none */
    static volatile RemoteService instance;
    /** what the screen shows about remote access */
    static volatile String status = "";

    private final Handler main = new Handler(Looper.getMainLooper());
    private final Object lock = new Object();
    private Thread server;              // guarded by lock: the thread in Native.webServe
    private boolean wanted;             // guarded by lock: keep serving
    private int generation;             // guarded by lock: which server thread is the current one
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
                        setStatus(getString(R.string.remote_on, addresses(Settings.port(RemoteService.this))));
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
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        String action = intent != null && intent.getAction() != null ? intent.getAction() : ACTION_START;
        foreground(getString(R.string.remote_starting));
        if (action.equals(ACTION_STOP) || !Settings.active(this)) {
            stopServing();
            setStatus("");
            if (Build.VERSION.SDK_INT >= 24)
                stopForeground(STOP_FOREGROUND_REMOVE);
            else
                stopForeground(true);
            stopSelf();
            return START_NOT_STICKY;
        }
        if (action.equals(ACTION_RESTART))
            stopServing();
        startServing();
        return START_STICKY;
    }

    @Override
    public void onDestroy() {
        unregisterReceiver(netEvents);
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
            if (server != null)
                return;
            wanted = true;
            int gen = ++generation;
            int port = Settings.port(this);
            String password = Settings.password(this);
            String dir = new File(getFilesDir(), "dumps").getPath();	// the app's dumps, the same as its own
            wakeLock.acquire();
            server = new Thread(() -> serve(gen, port, password, dir), "microsnander-web");
            server.start();
            setStatus(getString(R.string.remote_on, addresses(port)));
        }
    }

    /** Native.webServe until webStop; a port it cannot take is tried again */
    private void serve(int gen, int port, String password, String dir) {
        while (true) {
            int ret = Native.webServe("0.0.0.0:" + port, password, dir);
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
                setStatus(getString(R.string.remote_on, addresses(port)));
            }
        }
    }

    private void stopServing() {
        Thread t;
        synchronized (lock) {
            wanted = false;
            generation++;
            t = server;
            server = null;
        }
        if (t == null)
            return;
        Native.webStop();
        t.interrupt();
        try {
            t.join(3000);
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        }
        if (wakeLock.isHeld())
            wakeLock.release();
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

    // ---- the notification ----

    @SuppressWarnings("deprecation")   // Notification.Builder(Context): the one before Android 8
    private void foreground(String text) {
        NotificationManager nm = (NotificationManager) getSystemService(NOTIFICATION_SERVICE);
        Notification.Builder b;
        if (Build.VERSION.SDK_INT >= 26) {
            nm.createNotificationChannel(new NotificationChannel(CHANNEL, getString(R.string.remote_access),
                    NotificationManager.IMPORTANCE_LOW));
            b = new Notification.Builder(this, CHANNEL);
        } else
            b = new Notification.Builder(this);
        int f = Build.VERSION.SDK_INT >= 23 ? PendingIntent.FLAG_IMMUTABLE : 0;
        b.setSmallIcon(R.drawable.ic_notify)
         .setContentTitle(getString(R.string.remote_access))
         .setContentText(text)
         .setOngoing(true)
         .setContentIntent(PendingIntent.getActivity(this, 0, new Intent(this, MainActivity.class), f));
        startForeground(NOTIFY_ID, b.build());
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

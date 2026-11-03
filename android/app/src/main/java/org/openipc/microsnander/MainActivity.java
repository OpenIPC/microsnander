package org.openipc.microsnander;

import android.Manifest;
import android.annotation.SuppressLint;
import android.app.Activity;
import android.app.AlertDialog;
import android.app.PendingIntent;
import android.content.ActivityNotFoundException;
import android.content.BroadcastReceiver;
import android.content.ContentUris;
import android.content.ContentValues;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.PackageManager;
import android.database.Cursor;
import android.hardware.usb.UsbDevice;
import android.hardware.usb.UsbDeviceConnection;
import android.hardware.usb.UsbManager;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.os.Handler;
import android.os.Looper;
import android.provider.MediaStore;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.GridLayout;
import android.widget.ProgressBar;
import android.widget.TextView;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.lang.ref.WeakReference;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/**
 * Detect, Read, Erase, Write and Registers for a CH341A, CH347 or EZP
 * programmer on the USB host port, as the web mode of microsnander does.
 *
 * One operation at a time for the whole process: the native code has one
 * programmer and one chip. The worker and the state of the operation live
 * as long as the process, so a screen that is created again sees the
 * operation running and gets its result.
 */
public class MainActivity extends Activity {
    private static final String ACTION_USB_PERMISSION = "org.openipc.microsnander.USB_PERMISSION";
    /** only senders with our signature: our own PendingIntent */
    private static final String RESULT_PERMISSION = "org.openipc.microsnander.permission.USB_RESULT";
    private static final int PICK_WRITE = 1, SAVE_AS = 2, ASK_STORAGE = 3, ASK_READ = 4;

    /** vendor, product of the programmers microsnander drives */
    private static final int[][] PROGRAMMERS = {
        {0x1a86, 0x5512}, {0x1a86, 0x55db}, {0x1a86, 0x55de},
        {0x1fc8, 0x310b}, {0x1fc8, 0x310c}, {0x1fc8, 0x310d},
    };
    private static final String[][] BITS = {
        {"SRP0", "SEC", "TB", "BP2", "BP1", "BP0", "WEL", "BUSY"},
        {"SUS", "CMP", "LB3", "LB2", "LB1", "R", "QE", "SRP1"},
        {"HOLD", "DRV1", "DRV0", "R", "R", "WPS", "R", "R"},
    };
    private static final String PROTECTION = " SRP0 TB BP2 BP1 BP0 CMP SRP1 WPS ";

    // ---- the process-wide state of the operations (UI thread) ----
    private static final ExecutorService WORKER = Executors.newSingleThreadExecutor();
    private static final Handler MAIN = new Handler(Looper.getMainLooper());
    private static boolean busy;
    private static String busyText = "";
    private static int busyPct = -1;
    /** the screen that shows the results; weak, a closed screen goes */
    private static WeakReference<MainActivity> current = new WeakReference<>(null);
    private static Native.Result lastResult;
    private static String lastSaved;
    private static int lastSr = -1;

    private UsbManager usb;
    private TextView state, meta, regsTitle;
    private ProgressBar bar;
    private GridLayout regs;
    private View regsPanel;
    private Button[] buttons;
    private Button saveAs, share;
    private Runnable afterPermission;

    @Override
    protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        setContentView(R.layout.activity_main);
        usb = (UsbManager) getSystemService(Context.USB_SERVICE);
        state = findViewById(R.id.state);
        meta = findViewById(R.id.meta);
        bar = findViewById(R.id.bar);
        regs = findViewById(R.id.regs);
        regsPanel = findViewById(R.id.regs_panel);
        regsTitle = findViewById(R.id.regs_title);
        saveAs = findViewById(R.id.b_save_as);
        share = findViewById(R.id.b_share);
        ((TextView) findViewById(R.id.version)).setText(getString(R.string.version, BuildConfig.VERSION_NAME));
        buttons = new Button[] {
            findViewById(R.id.b_detect), findViewById(R.id.b_read), findViewById(R.id.b_erase),
            findViewById(R.id.b_write), findViewById(R.id.b_registers), findViewById(R.id.b_unprotect),
        };
        buttons[0].setOnClickListener(v -> run(R.string.detecting, true, (fd, vid, pid, p) -> Native.detect(fd, vid, pid)));
        buttons[1].setOnClickListener(v -> read());
        buttons[2].setOnClickListener(v -> new AlertDialog.Builder(this)
                .setMessage(R.string.erase_confirm)
                .setPositiveButton(R.string.erase, (d, w) -> run(R.string.erasing, false, Native::erase))
                .setNegativeButton(android.R.string.cancel, null).show());
        buttons[3].setOnClickListener(v -> pickWrite());
        buttons[4].setOnClickListener(v -> run(R.string.reading_registers, true, (fd, vid, pid, p) -> Native.registers(fd, vid, pid)));
        buttons[5].setOnClickListener(v -> run(R.string.unprotecting, true, (fd, vid, pid, p) -> Native.unprotect(fd, vid, pid)));
        saveAs.setOnClickListener(v -> {
            File dump = lastDump();
            if (dump == null) return;
            Intent i = new Intent(Intent.ACTION_CREATE_DOCUMENT).addCategory(Intent.CATEGORY_OPENABLE)
                    .setType("application/octet-stream").putExtra(Intent.EXTRA_TITLE, dump.getName());
            try {
                startActivityForResult(i, SAVE_AS);
            } catch (ActivityNotFoundException e) {
                meta.append("\n" + getString(R.string.no_picker_save));
            }
        });
        share.setOnClickListener(v -> {
            File dump = lastDump();
            if (dump == null) return;
            Intent i = new Intent(Intent.ACTION_SEND).setType("application/octet-stream")
                    .putExtra(Intent.EXTRA_STREAM, DumpProvider.uri(dump))
                    .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
            startActivity(Intent.createChooser(i, getString(R.string.share)));
        });
        listenForPermission();
        current = new WeakReference<>(this);
        if (busy)
            showBusy();
        else if (lastResult != null)
            showResult(lastResult, lastSaved);
        else
            showIdle(getString(R.string.press_detect));
        showRegisters(lastSr);
    }

    @Override
    protected void onDestroy() {
        unregisterReceiver(permission);
        if (current.get() == this)
            current = new WeakReference<>(null);
        if (afterPermission != null) {		// a request this screen was waiting for
            afterPermission = null;
            busy = false;
        }
        super.onDestroy();
    }

    /** the newest dump the app keeps (files/dumps), or null */
    private File lastDump() {
        File[] l = new File(getFilesDir(), "dumps").listFiles((d, n) -> n.startsWith("flash-") && n.endsWith(".bin"));
        File last = null;
        if (l != null)
            for (File f : l)
                if (last == null || f.lastModified() > last.lastModified()) last = f;
        return last;
    }

    // ---- USB ----

    private UsbDevice findProgrammer() {
        for (UsbDevice d : usb.getDeviceList().values())
            for (int[] p : PROGRAMMERS)
                if (d.getVendorId() == p[0] && d.getProductId() == p[1])
                    return d;
        return null;
    }

    /**
     * The answer to our own permission request. Only a sender that holds our
     * signature permission gets through (our PendingIntent does), so another
     * app cannot answer it; before Android 13 there is no
     * RECEIVER_NOT_EXPORTED, the permission is what keeps others out there.
     */
    @SuppressLint("UnspecifiedRegisterReceiverFlag")
    private void listenForPermission() {
        IntentFilter f = new IntentFilter(ACTION_USB_PERMISSION);
        if (Build.VERSION.SDK_INT >= 33)
            registerReceiver(permission, f, RESULT_PERMISSION, null, Context.RECEIVER_NOT_EXPORTED);
        else
            registerReceiver(permission, f, RESULT_PERMISSION, null);
    }

    private final BroadcastReceiver permission = new BroadcastReceiver() {
        @Override
        public void onReceive(Context c, Intent i) {
            Runnable r = afterPermission;
            afterPermission = null;
            if (r == null)
                return;
            UsbDevice d = findProgrammer();
            if (d != null && usb.hasPermission(d)) {
                r.run();
            } else {
                busy = false;
                showFailed(getString(R.string.no_permission));
            }
        }
    };

    /** the programmer, with the user's permission; then r. Busy from now on. */
    private void withDevice(int phase, Runnable r) {
        if (busy) return;
        UsbDevice d = findProgrammer();
        if (d == null) {
            showFailed(getString(R.string.no_programmer));
            return;
        }
        busy = true;
        busyText = getString(phase);
        busyPct = -1;
        showBusy();
        if (usb.hasPermission(d)) {
            r.run();
            return;
        }
        afterPermission = r;
        int flags = Build.VERSION.SDK_INT >= 31 ? PendingIntent.FLAG_MUTABLE : 0;
        Intent i = new Intent(ACTION_USB_PERMISSION).setPackage(getPackageName());
        usb.requestPermission(d, PendingIntent.getBroadcast(this, 0, i, flags));
    }

    interface Op { Native.Result run(int fd, int vid, int pid, Native.Progress p); }

    /** what runs in the worker after a successful operation: a line for the screen */
    interface After { String run(Native.Result r); }

    /** open the device for this operation only, as the web mode does */
    private static Native.Result onDevice(UsbManager usb, UsbDevice d, Op op) {
        UsbDeviceConnection c = d == null ? null : usb.openDevice(d);
        if (c == null)
            return null;
        try {
            return op.run(c.getFileDescriptor(), d.getVendorId(), d.getProductId(), MainActivity::progress);
        } finally {
            c.close();
        }
    }

    private void run(int phase, boolean registersOp, Op op) {
        runThen(phase, registersOp, op, null);
    }

    private void runThen(int phase, boolean registersOp, Op op, After after) {
        withDevice(phase, () -> {
            UsbManager u = usb;
            UsbDevice d = findProgrammer();
            WORKER.execute(() -> {
                Native.Result r = onDevice(u, d, op);
                String saved = after != null && r != null && r.ok ? after.run(r) : null;
                MAIN.post(() -> {
                    busy = false;
                    lastResult = r;
                    lastSaved = saved;
                    if (r != null && (registersOp || r.sr >= 0))
                        lastSr = r.sr;		// Read, Erase and Write leave the last registers shown
                    MainActivity a = current.get();
                    if (a != null) {
                        a.showResult(r, saved);
                        a.showRegisters(lastSr);
                    }
                });
            });
        });
    }

    private static void progress(String phase, long done, long total) {
        MAIN.post(() -> {
            MainActivity a = current.get();
            if (a == null) return;
            busyText = a.getString("verify".equals(phase) ? R.string.verifying : "write".equals(phase) ? R.string.writing
                    : "erase".equals(phase) ? R.string.erasing : R.string.reading);
            busyPct = total > 0 ? (int) (100 * done / total) : -1;
            a.showBusy();
        });
    }

    // ---- read ----

    private void read() {
        if (busy) return;
        if (Build.VERSION.SDK_INT >= 23 && Build.VERSION.SDK_INT < 29
                && checkSelfPermission(Manifest.permission.WRITE_EXTERNAL_STORAGE) != PackageManager.PERMISSION_GRANTED) {
            requestPermissions(new String[] {Manifest.permission.WRITE_EXTERNAL_STORAGE}, ASK_STORAGE);
            return;
        }
        Context app = getApplicationContext();
        File tmp = new File(getCacheDir(), "read.tmp");
        File dumps = new File(getFilesDir(), "dumps");
        runThen(R.string.reading, false, (fd, vid, pid, p) -> Native.read(fd, vid, pid, tmp.getPath(), p), r -> {
            //noinspection ResultOfMethodCallIgnored
            dumps.mkdirs();
            File dump = unique(dumps, r.name);
            if (dump == null || !(tmp.renameTo(dump) || copyFile(tmp, dump))) {
                //noinspection ResultOfMethodCallIgnored
                tmp.delete();
                return app.getString(R.string.cannot_keep);
            }
            String where = toDownloads(app, dump);
            return where != null ? app.getString(R.string.dump_is, where) : app.getString(R.string.dump_kept);
        });
    }

    @Override
    public void onRequestPermissionsResult(int code, String[] perms, int[] results) {
        boolean granted = results.length > 0 && results[0] == PackageManager.PERMISSION_GRANTED;
        if (code == ASK_STORAGE && granted)
            read();
        else if (code == ASK_STORAGE)
            showFailed(getString(R.string.no_storage));
        else if (code == ASK_READ && granted)
            pickWrite();
        else if (code == ASK_READ)
            showFailed(getString(R.string.no_files));
    }

    /** a second read in the same minute does not replace the first: -2, -3, ...; null when all are taken */
    private static File unique(File dir, String name) {
        File f = new File(dir, name);
        String base = name.endsWith(".bin") ? name.substring(0, name.length() - 4) : name;
        for (int k = 2; f.exists(); k++) {
            if (k > 999) return null;
            f = new File(dir, base + "-" + k + ".bin");
        }
        return f;
    }

    private static boolean copyFile(File from, File to) {
        try (OutputStream o = new FileOutputStream(to)) {
            copy(from, o);
            return true;
        } catch (IOException e) {
            //noinspection ResultOfMethodCallIgnored
            to.delete();
            return false;
        }
    }

    /** a copy in Downloads; its place, or null, with nothing half-written left there */
    private static String toDownloads(Context app, File dump) {
        if (Build.VERSION.SDK_INT >= 29) {
            ContentValues v = new ContentValues();
            v.put(MediaStore.Downloads.DISPLAY_NAME, dump.getName());
            v.put(MediaStore.Downloads.MIME_TYPE, "application/octet-stream");
            v.put(MediaStore.Downloads.IS_PENDING, 1);
            Uri u;
            try {
                u = app.getContentResolver().insert(MediaStore.Downloads.EXTERNAL_CONTENT_URI, v);
            } catch (IllegalArgumentException | SecurityException e) {
                return null;
            }
            if (u == null) return null;
            try (OutputStream o = app.getContentResolver().openOutputStream(u)) {
                if (o == null) throw new IOException();
                copy(dump, o);
            } catch (IOException | SecurityException e) {
                app.getContentResolver().delete(u, null, null);
                return null;
            }
            v.clear();
            v.put(MediaStore.Downloads.IS_PENDING, 0);
            app.getContentResolver().update(u, v, null, null);
            return "Download/" + dump.getName();
        }
        File dir = Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS);
        //noinspection ResultOfMethodCallIgnored
        dir.mkdirs();
        File out = unique(dir, dump.getName());
        if (out == null || !copyFile(dump, out))
            return null;
        return "Download/" + out.getName();
    }

    private static void copy(File from, OutputStream to) throws IOException {
        try (InputStream in = new FileInputStream(from)) {
            copy(in, to);
        }
    }

    private static void copy(InputStream in, OutputStream to) throws IOException {
        byte[] b = new byte[65536];
        for (int n; (n = in.read(b)) > 0; )
            to.write(b, 0, n);
    }

    // ---- save as, write ----

    @Override
    protected void onActivityResult(int code, int result, Intent data) {
        if (result != RESULT_OK || data == null || data.getData() == null) return;
        Uri u = data.getData();
        if (code == SAVE_AS)
            saveAs(u);
        else if (code == PICK_WRITE)
            confirmWrite(u);
    }

    /** the copy runs in the worker, so a large dump does not stop the screen */
    private void saveAs(Uri u) {
        File dump = lastDump();
        if (dump == null || busy) return;
        busy = true;
        busyText = getString(R.string.saving);
        busyPct = -1;
        showBusy();
        Context app = getApplicationContext();
        WORKER.execute(() -> {
            boolean ok;
            try (OutputStream o = app.getContentResolver().openOutputStream(u)) {
                if (o == null) throw new IOException();
                copy(dump, o);
                ok = true;
            } catch (IOException | SecurityException e) {
                ok = false;
            }
            boolean saved = ok;
            MAIN.post(() -> {
                busy = false;
                MainActivity a = current.get();
                if (a == null) return;
                a.showBusyOff();
                a.meta.append("\n" + a.getString(saved ? R.string.saved_as : R.string.cannot_save));
            });
        });
    }

    /** the file picker; a TV box without one gets a list of Downloads and the app's dumps */
    private void pickWrite() {
        if (busy) return;
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT).addCategory(Intent.CATEGORY_OPENABLE).setType("*/*");
        try {
            startActivityForResult(i, PICK_WRITE);
            return;
        } catch (ActivityNotFoundException e) {
            // no picker on this device
        }
        if (Build.VERSION.SDK_INT >= 23 && Build.VERSION.SDK_INT <= 32
                && checkSelfPermission(Manifest.permission.READ_EXTERNAL_STORAGE) != PackageManager.PERMISSION_GRANTED) {
            requestPermissions(new String[] {Manifest.permission.READ_EXTERNAL_STORAGE}, ASK_READ);
            return;
        }
        List<String> names = new ArrayList<>();
        List<Uri> uris = new ArrayList<>();
        listDownloads(names, uris);
        File[] own = new File(getFilesDir(), "dumps").listFiles((d, n) -> n.endsWith(".bin"));
        if (own != null)
            for (File f : own) {
                names.add(f.getName());
                uris.add(Uri.fromFile(f));
            }
        if (names.isEmpty()) {
            showFailed(getString(R.string.no_files));
            return;
        }
        new AlertDialog.Builder(this).setTitle(R.string.pick_file)
                .setItems(names.toArray(new String[0]), (d, w) -> confirmWrite(uris.get(w)))
                .setNegativeButton(android.R.string.cancel, null).show();
    }

    /**
     * The .bin files of Downloads. From Android 10 through the MediaStore
     * (from Android 11 that is the files this app saved there; other files
     * need the file picker), before that the folder itself.
     */
    private void listDownloads(List<String> names, List<Uri> uris) {
        if (Build.VERSION.SDK_INT >= 29) {
            String[] cols = {MediaStore.Downloads._ID, MediaStore.Downloads.DISPLAY_NAME};
            try (Cursor c = getContentResolver().query(MediaStore.Downloads.EXTERNAL_CONTENT_URI, cols,
                    MediaStore.Downloads.DISPLAY_NAME + " LIKE ?", new String[] {"%.bin"}, null)) {
                while (c != null && c.moveToNext()) {
                    names.add(c.getString(1));
                    uris.add(ContentUris.withAppendedId(MediaStore.Downloads.EXTERNAL_CONTENT_URI, c.getLong(0)));
                }
            } catch (IllegalArgumentException | SecurityException e) {
                // nothing listed
            }
            return;
        }
        File[] l = Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS)
                .listFiles((d, n) -> n.endsWith(".bin"));
        if (l != null)
            for (File f : l) {
                names.add(f.getName());
                uris.add(Uri.fromFile(f));
            }
    }

    private void confirmWrite(Uri u) {
        new AlertDialog.Builder(this)
                .setMessage(R.string.write_confirm)
                .setPositiveButton(R.string.write, (d, w) -> write(u))
                .setNegativeButton(android.R.string.cancel, null).show();
    }

    /** the file is staged and written in the worker, the screen busy all along */
    private void write(Uri u) {
        Context app = getApplicationContext();
        File tmp = new File(getCacheDir(), "write.tmp");
        run(R.string.writing, false, (fd, vid, pid, p) -> {
            try (InputStream in = app.getContentResolver().openInputStream(u); OutputStream o = new FileOutputStream(tmp)) {
                if (in == null) throw new IOException();
                copy(in, o);
            } catch (IOException | SecurityException e) {
                //noinspection ResultOfMethodCallIgnored
                tmp.delete();
                return new Native.Result(false, app.getString(R.string.cannot_open), "", "", 0, -1, "");
            }
            Native.Result r = Native.write(fd, vid, pid, tmp.getPath(), p);
            //noinspection ResultOfMethodCallIgnored
            tmp.delete();
            return r;
        });
    }

    // ---- the screen ----

    @SuppressWarnings("deprecation")
    private int col(int id) {
        return Build.VERSION.SDK_INT >= 23 ? getColor(id) : getResources().getColor(id);
    }

    private void enable(boolean on) {
        for (Button x : buttons) x.setEnabled(on);
        boolean dump = on && lastDump() != null;
        saveAs.setEnabled(dump);
        share.setEnabled(dump);
    }

    private void showBusy() {
        enable(false);
        bar.setVisibility(View.VISIBLE);
        bar.setIndeterminate(busyPct < 0);
        if (busyPct >= 0) bar.setProgress(busyPct);
        state.setTextColor(col(R.color.amber));
        state.setText(busyPct >= 0 ? String.format(Locale.ROOT, "%s %d%%", busyText, busyPct) : busyText);
    }

    private void showBusyOff() {
        enable(true);
        bar.setVisibility(View.INVISIBLE);
    }

    private void showIdle(String m) {
        showBusyOff();
        state.setTextColor(col(R.color.dim));
        state.setText(R.string.stand_by);
        meta.setText(m);
    }

    private void showFailed(String m) {
        showBusyOff();
        state.setTextColor(col(R.color.red));
        state.setText(R.string.failed);
        meta.setText(m);
    }

    private void showResult(Native.Result r, String saved) {
        showBusyOff();
        if (r == null) {
            showFailed(getString(R.string.no_programmer));
            return;
        }
        StringBuilder m = new StringBuilder();
        if (!r.programmer.isEmpty()) m.append(getString(R.string.programmer_is, r.programmer)).append('\n');
        if (!r.chip.isEmpty())
            m.append(getString(R.string.chip_is, r.chip, r.size >= 1 << 20 ? (r.size >> 20) + " MB" : (r.size >> 10) + " KB")).append('\n');
        if (saved != null) m.append(saved).append('\n');
        m.append(r.message);
        meta.setText(m);
        state.setTextColor(col(r.ok ? R.color.amber : R.color.red));
        state.setText(r.ok ? R.string.done : R.string.failed);
    }

    private void showRegisters(int sr) {
        if (sr < 0) {
            regsPanel.setVisibility(View.GONE);
            return;
        }
        regsPanel.setVisibility(View.VISIBLE);
        regsTitle.setText(getString(R.string.registers_hex, (sr >> 16) & 0xff, (sr >> 8) & 0xff, sr & 0xff));
        regs.removeAllViews();
        for (int r = 0; r < 3; r++) {
            int v = (sr >> (16 - 8 * r)) & 0xff;
            TextView name = new TextView(this);
            name.setText(getString(R.string.sr_n, r + 1));
            name.setTextColor(col(R.color.amber));
            regs.addView(name);
            for (int k = 0; k < 8; k++) {
                TextView b = new TextView(this);
                boolean on = ((v >> (7 - k)) & 1) != 0;
                String n = BITS[r][k];
                b.setText(n);
                b.setGravity(Gravity.CENTER);
                b.setTextSize(11);
                b.setPadding(6, 6, 6, 6);
                b.setTextColor(col(on ? R.color.ink_dark : R.color.dim));
                b.setBackgroundResource(on ? (PROTECTION.contains(" " + n + " ") ? R.drawable.bit_on : R.drawable.bit_ok) : R.drawable.bit_off);
                GridLayout.LayoutParams lp = new GridLayout.LayoutParams();
                lp.setMargins(3, 3, 3, 3);
                regs.addView(b, lp);
            }
        }
    }
}

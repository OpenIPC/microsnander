package org.openipc.microsnander;

import android.Manifest;
import android.annotation.SuppressLint;
import android.app.Activity;
import android.app.AlertDialog;
import android.app.PendingIntent;
import android.content.ActivityNotFoundException;
import android.content.BroadcastReceiver;
import android.content.ContentValues;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.PackageManager;
import android.hardware.usb.UsbDevice;
import android.hardware.usb.UsbDeviceConnection;
import android.hardware.usb.UsbManager;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
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
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.Locale;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/**
 * Detect, Read, Erase, Write and Registers for a CH341A, CH347 or EZP
 * programmer on the USB host port, as the web mode of microsnander does.
 */
public class MainActivity extends Activity {
    private static final String ACTION_USB_PERMISSION = "org.openipc.microsnander.USB_PERMISSION";
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

    private final ExecutorService worker = Executors.newSingleThreadExecutor();
    private UsbManager usb;
    private TextView state, meta, regsTitle;
    private ProgressBar bar;
    private GridLayout regs;
    private View regsPanel;
    private Button[] buttons;
    private Button saveAs, share;
    private File lastDump;
    private boolean busy, registersOp;
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
        buttons[0].setOnClickListener(v -> run(R.string.detecting, Native::detect, true));
        buttons[1].setOnClickListener(v -> read());
        buttons[2].setOnClickListener(v -> new AlertDialog.Builder(this)
                .setMessage(R.string.erase_confirm)
                .setPositiveButton(R.string.erase, (d, w) -> runProgress(R.string.erasing,
                        (fd, vid, pid, p) -> Native.erase(fd, vid, pid, p)))
                .setNegativeButton(android.R.string.cancel, null).show());
        buttons[3].setOnClickListener(v -> pickWrite());
        buttons[4].setOnClickListener(v -> run(R.string.reading_registers, Native::registers, true));
        buttons[5].setOnClickListener(v -> run(R.string.unprotecting, Native::unprotect, true));
        saveAs.setOnClickListener(v -> {
            if (lastDump == null) return;
            Intent i = new Intent(Intent.ACTION_CREATE_DOCUMENT).addCategory(Intent.CATEGORY_OPENABLE)
                    .setType("application/octet-stream").putExtra(Intent.EXTRA_TITLE, lastDump.getName());
            try {
                startActivityForResult(i, SAVE_AS);
            } catch (ActivityNotFoundException e) {
                meta.append("\n" + getString(R.string.no_picker_save));
            }
        });
        share.setOnClickListener(v -> {
            if (lastDump == null) return;
            Intent i = new Intent(Intent.ACTION_SEND).setType("application/octet-stream")
                    .putExtra(Intent.EXTRA_STREAM, DumpProvider.uri(lastDump))
                    .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
            startActivity(Intent.createChooser(i, getString(R.string.share)));
        });
        listenForPermission();
        showIdle(getString(R.string.press_detect));
    }

    @Override
    protected void onDestroy() {
        unregisterReceiver(permission);
        worker.shutdown();
        super.onDestroy();
    }

    // ---- USB ----

    /** the answer to our own permission request; the flag exists from Android 13 */
    @SuppressLint("UnspecifiedRegisterReceiverFlag")
    private void listenForPermission() {
        IntentFilter f = new IntentFilter(ACTION_USB_PERMISSION);
        if (Build.VERSION.SDK_INT >= 33)
            registerReceiver(permission, f, Context.RECEIVER_NOT_EXPORTED);
        else
            registerReceiver(permission, f);
    }

    private UsbDevice findProgrammer() {
        for (UsbDevice d : usb.getDeviceList().values())
            for (int[] p : PROGRAMMERS)
                if (d.getVendorId() == p[0] && d.getProductId() == p[1])
                    return d;
        return null;
    }

    private final BroadcastReceiver permission = new BroadcastReceiver() {
        @Override
        public void onReceive(Context c, Intent i) {
            Runnable r = afterPermission;
            afterPermission = null;
            if (i.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false) && r != null)
                r.run();
            else
                showFailed(getString(R.string.no_permission));
        }
    };

    /** the programmer, with the user's permission; then r */
    private void withDevice(Runnable r) {
        UsbDevice d = findProgrammer();
        if (d == null) {
            showFailed(getString(R.string.no_programmer));
            return;
        }
        if (usb.hasPermission(d)) {
            r.run();
            return;
        }
        afterPermission = r;
        int flags = Build.VERSION.SDK_INT >= 31 ? PendingIntent.FLAG_MUTABLE : 0;
        Intent i = new Intent(ACTION_USB_PERMISSION).setPackage(getPackageName());
        usb.requestPermission(d, PendingIntent.getBroadcast(this, 0, i, flags));
    }

    interface Op { Native.Result run(int fd, int vid, int pid); }

    interface ProgressOp { Native.Result run(int fd, int vid, int pid, Native.Progress p); }

    /** open the device for this operation only, as the web mode does */
    private Native.Result onDevice(ProgressOp op, Native.Progress p) {
        UsbDevice d = findProgrammer();
        if (d == null)
            return null;
        UsbDeviceConnection c = usb.openDevice(d);
        if (c == null)
            return null;
        try {
            return op.run(c.getFileDescriptor(), d.getVendorId(), d.getProductId(), p);
        } finally {
            c.close();
        }
    }

    private void run(int phase, Op op, boolean regs) {
        runProgress(phase, (fd, vid, pid, p) -> op.run(fd, vid, pid), regs);
    }

    private void runProgress(int phase, ProgressOp op) {
        runProgress(phase, op, false);
    }

    private void runProgress(int phase, ProgressOp op, boolean regs) {
        if (busy) return;
        registersOp = regs;
        withDevice(() -> {
            setBusy(true, getString(phase), -1);
            worker.execute(() -> {
                Native.Result r = onDevice(op, this::progress);
                runOnUiThread(() -> finished(r, null));
            });
        });
    }

    private void progress(String phase, long done, long total) {
        int pct = total > 0 ? (int) (100 * done / total) : -1;
        String word = getString("verify".equals(phase) ? R.string.verifying : "write".equals(phase) ? R.string.writing
                : "erase".equals(phase) ? R.string.erasing : R.string.reading);
        runOnUiThread(() -> setBusy(true, word, pct));
    }

    // ---- read and write ----

    private void read() {
        if (busy) return;
        registersOp = false;
        if (Build.VERSION.SDK_INT >= 23 && Build.VERSION.SDK_INT < 29
                && checkSelfPermission(Manifest.permission.WRITE_EXTERNAL_STORAGE) != PackageManager.PERMISSION_GRANTED) {
            requestPermissions(new String[] {Manifest.permission.WRITE_EXTERNAL_STORAGE}, ASK_STORAGE);
            return;
        }
        withDevice(() -> {
            setBusy(true, getString(R.string.reading), 0);
            File tmp = new File(getCacheDir(), "read.tmp");
            worker.execute(() -> {
                Native.Result r = onDevice((fd, vid, pid, p) -> Native.read(fd, vid, pid, tmp.getPath(), p), this::progress);
                String saved = null;
                if (r != null && r.ok) {
                    File dumps = new File(getFilesDir(), "dumps");
                    //noinspection ResultOfMethodCallIgnored
                    dumps.mkdirs();
                    File dump = unique(dumps, r.name);
                    if (tmp.renameTo(dump)) {
                        lastDump = dump;
                        saved = toDownloads(dump);
                    }
                }
                String where = saved;
                runOnUiThread(() -> finished(r, where));
            });
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

    /** a second read in the same minute does not replace the first: -2, -3, ... */
    private static File unique(File dir, String name) {
        File f = new File(dir, name);
        String base = name.endsWith(".bin") ? name.substring(0, name.length() - 4) : name;
        for (int k = 2; f.exists() && k < 100; k++)
            f = new File(dir, base + "-" + k + ".bin");
        return f;
    }

    /** a copy in Downloads; its place, or null */
    private String toDownloads(File dump) {
        try {
            if (Build.VERSION.SDK_INT >= 29) {
                ContentValues v = new ContentValues();
                v.put(MediaStore.Downloads.DISPLAY_NAME, dump.getName());
                v.put(MediaStore.Downloads.MIME_TYPE, "application/octet-stream");
                Uri u = getContentResolver().insert(MediaStore.Downloads.EXTERNAL_CONTENT_URI, v);
                if (u == null) return null;
                try (OutputStream o = getContentResolver().openOutputStream(u)) {
                    copy(dump, o);
                }
                return "Download/" + dump.getName();
            }
            File dir = Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS);
            //noinspection ResultOfMethodCallIgnored
            dir.mkdirs();
            File out = unique(dir, dump.getName());
            try (OutputStream o = new FileOutputStream(out)) {
                copy(dump, o);
            }
            return "Download/" + out.getName();
        } catch (IOException | SecurityException e) {
            return null;
        }
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

    @Override
    protected void onActivityResult(int code, int result, Intent data) {
        if (result != RESULT_OK || data == null || data.getData() == null) return;
        Uri u = data.getData();
        if (code == SAVE_AS && lastDump != null) {
            try (OutputStream o = getContentResolver().openOutputStream(u)) {
                copy(lastDump, o);
                meta.append("\n" + getString(R.string.saved_as));
            } catch (IOException | SecurityException e) {
                showFailed(getString(R.string.cannot_save));
            }
        } else if (code == PICK_WRITE) {
            confirmWrite(u);
        }
    }

    /** the file picker; a TV box without one gets the .bin files of Downloads and the app's dumps */
    private void pickWrite() {
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
        List<File> files = new ArrayList<>();
        for (File dir : new File[] {Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS),
                new File(getFilesDir(), "dumps")}) {
            File[] l = dir.listFiles((d, n) -> n.endsWith(".bin"));
            if (l != null) files.addAll(Arrays.asList(l));
        }
        if (files.isEmpty()) {
            showFailed(getString(R.string.no_files));
            return;
        }
        String[] names = new String[files.size()];
        for (int k = 0; k < names.length; k++) names[k] = files.get(k).getName();
        new AlertDialog.Builder(this).setTitle(R.string.pick_file)
                .setItems(names, (d, w) -> confirmWrite(Uri.fromFile(files.get(w))))
                .setNegativeButton(android.R.string.cancel, null).show();
    }

    private void confirmWrite(Uri u) {
        new AlertDialog.Builder(this)
                .setMessage(R.string.write_confirm)
                .setPositiveButton(R.string.write, (d, w) -> write(u))
                .setNegativeButton(android.R.string.cancel, null).show();
    }

    private void write(Uri u) {
        File tmp = new File(getCacheDir(), "write.tmp");
        try (InputStream in = getContentResolver().openInputStream(u); OutputStream o = new FileOutputStream(tmp)) {
            if (in == null) throw new IOException();
            copy(in, o);
        } catch (IOException | SecurityException e) {
            showFailed(getString(R.string.cannot_open));
            return;
        }
        runProgress(R.string.writing, (fd, vid, pid, p) -> {
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

    private void setBusy(boolean b, String what, int pct) {
        busy = b;
        for (Button x : buttons) x.setEnabled(!b);
        saveAs.setEnabled(!b && lastDump != null);
        share.setEnabled(!b && lastDump != null);
        bar.setVisibility(b ? View.VISIBLE : View.INVISIBLE);
        bar.setIndeterminate(pct < 0);
        if (pct >= 0) bar.setProgress(pct);
        if (b) {
            state.setTextColor(col(R.color.amber));
            state.setText(pct >= 0 ? String.format(Locale.ROOT, "%s %d%%", what, pct) : what);
        }
    }

    private void showIdle(String m) {
        setBusy(false, "", 0);
        state.setTextColor(col(R.color.dim));
        state.setText(R.string.stand_by);
        meta.setText(m);
    }

    private void showFailed(String m) {
        setBusy(false, "", 0);
        state.setTextColor(col(R.color.red));
        state.setText(R.string.failed);
        meta.setText(m);
    }

    private void finished(Native.Result r, String saved) {
        setBusy(false, "", 0);
        if (r == null) {
            showFailed(getString(R.string.no_programmer));
            return;
        }
        StringBuilder m = new StringBuilder();
        if (!r.programmer.isEmpty()) m.append(getString(R.string.programmer_is, r.programmer)).append('\n');
        if (!r.chip.isEmpty())
            m.append(getString(R.string.chip_is, r.chip, r.size >= 1 << 20 ? (r.size >> 20) + " MB" : (r.size >> 10) + " KB")).append('\n');
        if (saved != null) m.append(getString(R.string.dump_is, saved)).append('\n');
        else if (r.ok && !r.name.isEmpty()) m.append(getString(R.string.dump_kept)).append('\n');
        m.append(r.message);
        meta.setText(m);
        state.setTextColor(col(r.ok ? R.color.amber : R.color.red));
        state.setText(r.ok ? R.string.done : R.string.failed);
        if (r.sr >= 0 || registersOp)
            showRegisters(r.sr);	// Read, Erase and Write leave the last registers shown
        saveAs.setEnabled(lastDump != null);
        share.setEnabled(lastDump != null);
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

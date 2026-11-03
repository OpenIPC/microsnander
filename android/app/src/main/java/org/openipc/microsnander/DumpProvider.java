package org.openipc.microsnander;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.database.Cursor;
import android.database.MatrixCursor;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import android.provider.OpenableColumns;

import java.io.File;
import java.io.FileNotFoundException;

/** Hands a dump of files/dumps to the app chosen in Share, read-only. */
public class DumpProvider extends ContentProvider {
    static final String AUTHORITY = "org.openipc.microsnander.dumps";

    static Uri uri(File dump) {
        return Uri.parse("content://" + AUTHORITY + "/" + Uri.encode(dump.getName()));
    }

    /** a dump of files/dumps by its name only, no other file */
    private File file(Uri u) throws FileNotFoundException {
        String name = u.getLastPathSegment();
        if (name == null || name.contains("/") || !MainActivity.isDump(name))
            throw new FileNotFoundException();
        File f = new File(new File(getContext().getFilesDir(), "dumps"), name);
        if (!f.isFile())
            throw new FileNotFoundException();
        return f;
    }

    @Override
    public boolean onCreate() {
        return true;
    }

    @Override
    public ParcelFileDescriptor openFile(Uri u, String mode) throws FileNotFoundException {
        if (!"r".equals(mode))
            throw new FileNotFoundException();
        return ParcelFileDescriptor.open(file(u), ParcelFileDescriptor.MODE_READ_ONLY);
    }

    @Override
    public Cursor query(Uri u, String[] projection, String selection, String[] args, String sort) {
        try {
            File f = file(u);
            MatrixCursor c = new MatrixCursor(new String[] {OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE});
            c.addRow(new Object[] {f.getName(), f.length()});
            return c;
        } catch (FileNotFoundException e) {
            return null;
        }
    }

    @Override
    public String getType(Uri u) {
        return "application/octet-stream";
    }

    @Override
    public Uri insert(Uri u, ContentValues v) {
        return null;
    }

    @Override
    public int delete(Uri u, String s, String[] a) {
        return 0;
    }

    @Override
    public int update(Uri u, ContentValues v, String s, String[] a) {
        return 0;
    }
}

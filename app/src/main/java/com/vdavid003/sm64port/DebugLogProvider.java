package com.vdavid003.sm64port;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.database.Cursor;
import android.net.Uri;
import android.os.ParcelFileDescriptor;

import java.io.File;
import java.io.FileNotFoundException;

/**
 * Minimal read-only provider so the exported debug log can be attached to an
 * ACTION_SEND share sheet without adding an AndroidX/FileProvider dependency.
 * Only files inside the app cache/debug-logs directory are served.
 */
public class DebugLogProvider extends ContentProvider {
    private static final String AUTHORITY_SUFFIX = ".debuglog";

    public static Uri uriFor(android.content.Context context, File file) {
        return new Uri.Builder()
                .scheme("content")
                .authority(context.getPackageName() + AUTHORITY_SUFFIX)
                .appendPath(file.getName())
                .build();
    }

    @Override
    public boolean onCreate() {
        return true;
    }

    @Override
    public ParcelFileDescriptor openFile(Uri uri, String mode) throws FileNotFoundException {
        if (getContext() == null || uri.getLastPathSegment() == null) {
            throw new FileNotFoundException("no debug log path");
        }
        if (!"r".equals(mode)) {
            throw new FileNotFoundException("read-only provider");
        }
        File dir = new File(getContext().getCacheDir(), "debug-logs");
        File target;
        try {
            dir = dir.getCanonicalFile();
            target = new File(dir, uri.getLastPathSegment()).getCanonicalFile();
        } catch (Exception e) {
            throw new FileNotFoundException("bad debug log path");
        }
        if (!dir.equals(target.getParentFile()) || !target.isFile()) {
            throw new FileNotFoundException("debug log not found: " + target);
        }
        return ParcelFileDescriptor.open(target, ParcelFileDescriptor.MODE_READ_ONLY);
    }

    @Override
    public String getType(Uri uri) {
        return "text/plain";
    }

    @Override
    public Cursor query(Uri uri, String[] projection, String selection,
                        String[] selectionArgs, String sortOrder) {
        return null;
    }

    @Override
    public Uri insert(Uri uri, ContentValues values) {
        return null;
    }

    @Override
    public int delete(Uri uri, String selection, String[] selectionArgs) {
        return 0;
    }

    @Override
    public int update(Uri uri, ContentValues values, String selection, String[] selectionArgs) {
        return 0;
    }
}

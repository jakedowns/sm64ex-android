package com.vdavid003.sm64port;

import android.content.Context;
import android.graphics.Bitmap;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.util.Log;
import android.widget.Toast;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.Locale;

/**
 * Feedback outbox.
 *
 * A tap on FEEDBACK writes one self-contained item under
 * {@code <external files>/feedback/<timestamp>/}:
 *
 * <ul>
 *   <li>{@code feedback.txt} - the user's text (empty for a one-tap capture)</li>
 *   <li>{@code shot.png}     - the game frame presented at the tap</li>
 *   <li>{@code state.txt}    - the debug-report header (versions, FPS mode,
 *                              rate, FX flags, stereo, full PTC status line)</li>
 *   <li>{@code log.txt}      - bounded tail of the app log ({@link DebugLog})</li>
 * </ul>
 *
 * Items stay until an agent pulls and deletes them over adb; the in-app CLEAR
 * button only exists for the user. Nothing is written into the repository, into
 * {@code files/debug-logs} or into the debug log's rotation, so the outbox can
 * never evict a crash log.
 *
 * Pull:
 * <pre>
 *   adb shell ls /sdcard/Android/data/com.vdavid003.sm64port/files/feedback
 *   adb pull /sdcard/Android/data/com.vdavid003.sm64port/files/feedback/&lt;item&gt;
 *   adb shell rm -rf /sdcard/Android/data/com.vdavid003.sm64port/files/feedback/&lt;item&gt;
 * </pre>
 */
public final class FeedbackOutbox {
    public static final String TAG = "SM64Feedback";
    /** Directory name under the app's external files dir. */
    public static final String DIR_NAME = "feedback";
    /** Device path prefix used in the docs and in the toast. */
    public static final String DEVICE_ROOT =
            "/sdcard/Android/data/com.vdavid003.sm64port/files/" + DIR_NAME;

    private static final long CAPTURE_TIMEOUT_MS = 5000L;
    private static final int MAX_LOG_TAIL_BYTES = 256 * 1024;
    private static final int SHOT_MAX_W = 1920;
    private static final Object ONE_AT_A_TIME = new Object();
    private static boolean sCapturing;

    private FeedbackOutbox() {}

    /** Outbox root, or null when external storage is unavailable. */
    public static File root(Context context) {
        if (context == null) return null;
        Context app = context.getApplicationContext();
        File base = app.getExternalFilesDir(null);
        if (base == null) {
            // External storage is not mounted; keep the item somewhere the
            // debug report can still find it rather than losing the capture.
            Log.w(TAG, "external files dir unavailable; using internal storage");
            base = app.getFilesDir();
        }
        return new File(base, DIR_NAME);
    }

    /** Number of pending items (one per timestamped directory). */
    public static int count(Context context) {
        File dir = root(context);
        File[] items = dir == null ? null : dir.listFiles();
        if (items == null) return 0;
        int n = 0;
        for (File f : items) {
            if (f.isDirectory()) n++;
        }
        return n;
    }

    /** Delete every pending item. Returns the number of items removed. */
    public static int clear(Context context) {
        File dir = root(context);
        File[] items = dir == null ? null : dir.listFiles();
        if (items == null) return 0;
        int n = 0;
        for (File f : items) {
            if (f.isDirectory() && deleteTree(f)) n++;
        }
        Log.i(TAG, "cleared " + n + " feedback item(s)");
        return n;
    }

    /**
     * Capture one item. Returns immediately; the frame is grabbed on the next
     * rendered frame and the PNG/writes happen on a background thread. The
     * toast and {@code onDone} run on the main thread when the item is on disk.
     *
     * @param text    optional user text; null/empty is the one-tap path
     * @param onDone  optional badge refresh callback
     */
    public static void capture(Context context, String text, Runnable onDone) {
        if (context == null) return;
        final Context app = context.getApplicationContext();
        final String note = text == null ? "" : text;
        synchronized (ONE_AT_A_TIME) {
            if (sCapturing) {
                toast(app, "feedback capture already running");
                return;
            }
            sCapturing = true;
        }
        final long t0 = SystemClock.elapsedRealtime();
        Thread worker = new Thread(new Runnable() {
            @Override
            public void run() {
                File item = null;
                try {
                    item = writeItem(app, note, t0);
                } catch (Throwable t) {
                    Log.w(TAG, "feedback capture failed", t);
                    toast(app, "feedback failed: " + t);
                } finally {
                    synchronized (ONE_AT_A_TIME) {
                        sCapturing = false;
                    }
                }
                if (item != null) {
                    long ms = SystemClock.elapsedRealtime() - t0;
                    Log.i(TAG, "FEEDBACK saved " + item + " (" + ms + " ms, "
                            + count(app) + " pending)");
                    toast(app, "FEEDBACK (" + count(app) + ") saved: " + item.getAbsolutePath());
                }
                if (onDone != null) {
                    new Handler(Looper.getMainLooper()).post(onDone);
                }
            }
        }, "sm64-feedback");
        worker.setDaemon(true);
        worker.start();
    }

    // ---- item writer -------------------------------------------------------

    private static File writeItem(Context app, String text, long t0) throws IOException {
        File root = root(app);
        if (root == null) throw new IOException("no feedback dir");
        if (!root.exists() && !root.mkdirs()) {
            throw new IOException("cannot create " + root);
        }
        String stamp = new SimpleDateFormat("yyyyMMdd-HHmmss", Locale.US).format(new Date());
        File item = new File(root, stamp);
        for (int i = 2; item.exists() && i < 100; i++) {
            item = new File(root, stamp + "-" + i);
        }
        if (!item.mkdirs()) throw new IOException("cannot create " + item);

        // Ask for the frame first: everything below is file I/O that would
        // otherwise delay the grab by a frame or two.
        File raw = new File(item, "shot.rgba");
        nativeCaptureFrame(raw.getAbsolutePath());
        long frameT0 = SystemClock.elapsedRealtime();

        writeText(new File(item, "feedback.txt"), text);

        long frameMs = -1;
        File shot = new File(item, "shot.png");
        if (waitForFrame()) {
            frameMs = SystemClock.elapsedRealtime() - frameT0;
            try {
                rawToPng(raw, shot);
            } catch (Throwable t) {
                Log.w(TAG, "shot decode failed", t);
            }
        }
        // Never leave the intermediate in the item.
        if (raw.exists() && !raw.delete()) Log.w(TAG, "could not delete " + raw);

        StringBuilder head = new StringBuilder(256);
        head.append("========== FEEDBACK ITEM ==========\n");
        head.append("item: ").append(item.getAbsolutePath()).append('\n');
        head.append("pull: adb pull ").append(DEVICE_ROOT).append('/')
                .append(item.getName()).append('\n');
        head.append("textChars: ").append(text.length()).append('\n');
        head.append("frameMs: ").append(frameMs)
                .append("  shot: ").append(shot.isFile() ? shot.length() + " bytes" : "missing")
                .append('\n');
        head.append("totalMs: ").append(SystemClock.elapsedRealtime() - t0).append('\n');
        head.append("shotMaxWidth: ").append(SHOT_MAX_W).append('\n');

        StringBuilder tail = new StringBuilder(256);
        if (DebugLog.writeLogTail(new File(item, "log.txt"), MAX_LOG_TAIL_BYTES)) {
            tail.append("log.txt: last ").append(MAX_LOG_TAIL_BYTES).append(" bytes of the app log\n");
        } else {
            tail.append("log.txt: unavailable (debug log not installed)\n");
        }

        writeText(new File(item, "state.txt"),
                head + "\n" + DebugLog.environmentReport(app) + "\n" + tail);
        return item;
    }

    private static void writeText(File file, String text) throws IOException {
        try (FileOutputStream out = new FileOutputStream(file, false)) {
            out.write(text.getBytes(StandardCharsets.UTF_8));
            out.flush();
        }
    }

    private static boolean waitForFrame() {
        long start = SystemClock.elapsedRealtime();
        while (SystemClock.elapsedRealtime() - start < CAPTURE_TIMEOUT_MS) {
            int st = nativeCaptureState();
            if (st == 2) return true;
            if (st < 0) return false;
            try {
                Thread.sleep(8);
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
                return false;
            }
        }
        Log.w(TAG, "frame capture timed out");
        return false;
    }

    /** Decode "SM64FB1\n w=.. h=..\n" + RGBA rows into a PNG. */
    private static void rawToPng(File raw, File png) throws IOException {
        try (FileInputStream in = new FileInputStream(raw)) {
            String magic = readLine(in);
            String dims = readLine(in);
            if (!"SM64FB1".equals(magic) || dims == null) {
                throw new IOException("bad capture header: " + magic);
            }
            int w = 0, h = 0;
            for (String part : dims.trim().split("\\s+")) {
                int eq = part.indexOf('=');
                if (eq < 0) continue;
                String k = part.substring(0, eq);
                int v = Integer.parseInt(part.substring(eq + 1));
                if (k.equals("w")) w = v;
                else if (k.equals("h")) h = v;
            }
            if (w <= 0 || h <= 0 || w > 8192 || h > 8192) {
                throw new IOException("bad capture size " + w + "x" + h);
            }
            byte[] pixels = new byte[w * h * 4];
            int read = 0;
            while (read < pixels.length) {
                int n = in.read(pixels, read, pixels.length - read);
                if (n <= 0) throw new IOException("short capture: " + read + "/" + pixels.length);
                read += n;
            }
            Bitmap bmp = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888);
            try {
                bmp.copyPixelsFromBuffer(ByteBuffer.wrap(pixels));
                try (FileOutputStream out = new FileOutputStream(png, false)) {
                    if (!bmp.compress(Bitmap.CompressFormat.PNG, 100, out)) {
                        throw new IOException("PNG encode failed");
                    }
                    out.flush();
                }
            } finally {
                bmp.recycle();
            }
        }
    }

    /** ASCII line reader that leaves the stream at the first pixel byte. */
    private static String readLine(FileInputStream in) throws IOException {
        ByteArrayOutputStream line = new ByteArrayOutputStream(64);
        int c;
        while ((c = in.read()) >= 0) {
            if (c == '\n') break;
            if (c != '\r') line.write(c);
        }
        if (c < 0 && line.size() == 0) return null;
        return new String(line.toByteArray(), StandardCharsets.US_ASCII);
    }

    private static boolean deleteTree(File f) {
        File[] kids = f.listFiles();
        if (kids != null) {
            for (File k : kids) deleteTree(k);
        }
        return f.delete();
    }

    private static void toast(Context app, String msg) {
        new Handler(Looper.getMainLooper()).post(() -> {
            try {
                Toast.makeText(app, msg, Toast.LENGTH_LONG).show();
            } catch (Throwable ignored) {
            }
        });
    }

    // ---- native frame grab -------------------------------------------------

    /** Ask the renderer for the frame presented just after this call. */
    private static native void nativeCaptureFrame(String path);

    /** 0 idle, 1 pending, 2 written, negative on error. */
    private static native int nativeCaptureState();
}

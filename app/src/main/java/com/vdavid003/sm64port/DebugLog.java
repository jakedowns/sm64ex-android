package com.vdavid003.sm64port;

import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageInfo;
import android.net.Uri;
import android.os.Build;
import android.util.DisplayMetrics;
import android.util.Log;
import android.view.Display;
import android.view.WindowManager;

import java.io.BufferedReader;
import java.io.BufferedWriter;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStreamReader;
import java.io.OutputStreamWriter;
import java.nio.charset.StandardCharsets;
import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.LinkedHashMap;
import java.util.Locale;
import java.util.Map;

/**
 * On-device debug logging for user bug reports.
 *
 * The app process is small enough that logcat can be mirrored continuously to
 * an app-private file. That matters for boot failures: the user can start the
 * app, hit the black screen, then export the log even after the original
 * process is gone. At export time we also append a fresh logcat snapshot (main
 * + crash buffers) and a full environment header so Android/firmware/CNSDK
 * versions travel with the log.
 */
public final class DebugLog {
    public static final String TAG = "SM64Debug";

    private static final String LOG_DIR = "debug-logs";
    private static final String LOG_NAME = "sm64-debug.log";
    private static final String OLD_LOG_NAME = "sm64-debug.1.log";
    private static final long MAX_LOG_BYTES = 4L * 1024L * 1024L;
    private static final long MAX_SNAPSHOT_BYTES = 2L * 1024L * 1024L;
    /** Fallback if the AAR asset is unavailable. Update with the bundled AAR. */
    private static final String CNSDK_FALLBACK_VERSION = "0.6.147";

    private static final Object LOCK = new Object();
    private static Context appContext;
    private static File logFile;
    private static BufferedWriter writer;
    private static long bytesWritten;
    private static boolean installed;
    private static Thread captureThread;

    private DebugLog() {}

    /** Install once, from Application.onCreate, before the game starts. */
    public static void install(Context context) {
        if (context == null) return;
        synchronized (LOCK) {
            if (installed) return;
            installed = true;
            appContext = context.getApplicationContext();
            File dir = new File(appContext.getFilesDir(), LOG_DIR);
            if (!dir.exists() && !dir.mkdirs()) {
                Log.e(TAG, "cannot create debug log dir: " + dir);
                return;
            }
            logFile = new File(dir, LOG_NAME);

            // Keep the previous session if the current one grew too large.
            File old = new File(dir, OLD_LOG_NAME);
            try {
                if (logFile.length() > MAX_LOG_BYTES) {
                    if (old.exists()) old.delete();
                    logFile.renameTo(old);
                }
            } catch (Throwable t) {
                Log.w(TAG, "could not rotate old debug log", t);
            }

            openWriterLocked();
            writeEnvironmentLocked("BOOT");
        }
        startCapture();
        installCrashHandler();
        Log.i(TAG, "debug log capture installed: " + logFile);
    }

    /** Make sure a fatal Java crash is in the file even if logcat is unavailable. */
    private static void installCrashHandler() {
        try {
            final Thread.UncaughtExceptionHandler previous =
                    Thread.getDefaultUncaughtExceptionHandler();
            Thread.setDefaultUncaughtExceptionHandler(new Thread.UncaughtExceptionHandler() {
                @Override
                public void uncaughtException(Thread thread, Throwable error) {
                    try {
                        note("FATAL uncaught exception in " + thread.getName() + ":\n"
                                + Log.getStackTraceString(error));
                        flush();
                    } catch (Throwable ignored) {
                    }
                    if (previous != null) {
                        previous.uncaughtException(thread, error);
                    }
                }
            });
        } catch (Throwable t) {
            Log.w(TAG, "could not install crash handler", t);
        }
    }

    /**
     * Write a lifecycle marker straight to the file as well as logcat. Used for
     * the Leia weaver boot stages so they survive even if continuous logcat
     * capture is unavailable on a user's build.
     */
    public static void note(String message) {
        Log.i(TAG, message);
        synchronized (LOCK) {
            if (writer == null) return;
            if (bytesWritten > MAX_LOG_BYTES) rotateLocked();
            try {
                String stamp = new SimpleDateFormat("MM-dd HH:mm:ss.SSS", Locale.US)
                        .format(new Date());
                writer.write(stamp + " I/" + TAG + "(" + android.os.Process.myPid()
                        + "): " + message + "\n");
                writer.flush();
                bytesWritten += message.length() + 40L;
            } catch (Throwable t) {
                Log.w(TAG, "debug note write failed", t);
            }
        }
    }

    /** Force buffered lines to disk (called before export and on pause). */
    public static void flush() {
        synchronized (LOCK) {
            flushLocked();
        }
    }

    /** The live rolling log file, or null if logging failed to initialise. */
    public static File getLogFile(Context context) {
        synchronized (LOCK) {
            if (logFile != null) return logFile;
        }
        if (context == null) return null;
        return new File(new File(context.getApplicationContext().getFilesDir(), LOG_DIR), LOG_NAME);
    }

    /** Short one-line summary used in share text and boot markers. */
    public static String oneLineSummary(Context context) {
        if (context == null) return "SM64 debug";
        Context app = context.getApplicationContext();
        String version = "?";
        try {
            PackageInfo pi = app.getPackageManager().getPackageInfo(app.getPackageName(), 0);
            version = pi.versionName + " (" + pi.getLongVersionCode() + ")";
        } catch (Throwable ignored) {}
        return "SM64 Lume Pad 2 fork " + version
                + " | Android " + Build.VERSION.RELEASE + " (SDK " + Build.VERSION.SDK_INT + ")"
                + " | " + Build.MANUFACTURER + " " + Build.MODEL
                + " | build " + Build.DISPLAY
                + " | CNSDK " + cnsdkVersion(app);
    }

    /**
     * Build a self-contained UTF-8 report in cacheDir: environment header,
     * captured boot log (previous + current session), then a fresh logcat
     * snapshot. Returns the file ready to save or share.
     */
    public static File buildExportFile(Context context) throws IOException {
        Context app = context.getApplicationContext();
        File dir = new File(app.getCacheDir(), LOG_DIR);
        if (!dir.exists() && !dir.mkdirs()) {
            throw new IOException("cannot create export dir: " + dir);
        }
        String stamp = new SimpleDateFormat("yyyyMMdd-HHmmss", Locale.US).format(new Date());
        File out = new File(dir, "sm64-debug-" + stamp + ".txt");

        flush();
        try (BufferedWriter w = new BufferedWriter(new OutputStreamWriter(
                new FileOutputStream(out, false), StandardCharsets.UTF_8))) {
            w.write(environmentReport(app));
            w.write("\n");
            w.write("========== CAPTURED APP LOG (boot -> now) ==========\n");
            synchronized (LOCK) {
                File old = logFile == null ? null : new File(logFile.getParentFile(), OLD_LOG_NAME);
                copyInto(old, w);
                copyInto(logFile, w);
            }
            w.write("\n\n========== LOGCAT SNAPSHOT (main buffer) ==========\n");
            copyLogcatInto(w, new String[]{"-d", "-b", "main", "-v", "threadtime",
                    "--pid=" + android.os.Process.myPid()});
            w.write("\n\n========== LOGCAT SNAPSHOT (crash buffer) ==========\n");
            copyLogcatInto(w, new String[]{"-d", "-b", "crash", "-v", "threadtime",
                    "--pid=" + android.os.Process.myPid()});
        }
        Log.i(TAG, "export report written: " + out + " (" + out.length() + " bytes)");
        return out;
    }

    /** Share sheet intent for an exported log file. */
    public static Intent buildShareIntent(Context context, File file) {
        Intent send = new Intent(Intent.ACTION_SEND);
        send.setType("text/plain");
        Uri uri = DebugLogProvider.uriFor(context, file);
        send.putExtra(Intent.EXTRA_STREAM, uri);
        send.putExtra(Intent.EXTRA_SUBJECT, "SM64 Lume Pad 2 debug log");
        send.putExtra(Intent.EXTRA_TEXT,
                oneLineSummary(context)
                        + "\n\nThe attached log covers app boot, Leia weaver/CNSDK setup,"
                        + " Android/firmware versions, and recent runtime logs.");
        send.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
        return Intent.createChooser(send, "Share SM64 debug log");
    }

    /** Human-readable environment block included in logs and the About tab. */
    public static String environmentReport(Context context) {
        Context app = context.getApplicationContext();
        StringBuilder sb = new StringBuilder(4096);
        String stamp = new SimpleDateFormat("yyyy-MM-dd HH:mm:ss Z", Locale.US).format(new Date());
        sb.append("========== SM64 LUME PAD 2 DEBUG REPORT ==========\n");
        sb.append("generated: ").append(stamp).append('\n');

        sb.append("\n--- app ---\n");
        try {
            PackageInfo pi = app.getPackageManager().getPackageInfo(app.getPackageName(), 0);
            sb.append("package: ").append(app.getPackageName()).append('\n');
            sb.append("versionName: ").append(pi.versionName).append('\n');
            sb.append("versionCode: ").append(pi.getLongVersionCode()).append('\n');
            sb.append("buildStamp: ").append(BuildConfig.BUILD_STAMP).append('\n');
        } catch (Throwable t) {
            sb.append("app info error: ").append(t).append('\n');
        }
        sb.append("leiaCNSDK: ").append(cnsdkVersion(app)).append('\n');

        sb.append("\n--- android ---\n");
        sb.append("release: ").append(Build.VERSION.RELEASE).append('\n');
        sb.append("sdkInt: ").append(Build.VERSION.SDK_INT).append('\n');
        sb.append("codename: ").append(Build.VERSION.CODENAME).append('\n');
        sb.append("securityPatch: ").append(Build.VERSION.SECURITY_PATCH).append('\n');
        sb.append("display: ").append(Build.DISPLAY).append('\n');
        sb.append("lumeFirmwareDisplay: ").append(Build.DISPLAY).append('\n');
        sb.append("buildId: ").append(Build.ID).append('\n');
        sb.append("incremental: ").append(Build.VERSION.INCREMENTAL).append('\n');
        sb.append("type: ").append(Build.TYPE).append('\n');
        sb.append("tags: ").append(Build.TAGS).append('\n');
        sb.append("fingerprint: ").append(Build.FINGERPRINT).append('\n');

        sb.append("\n--- device ---\n");
        sb.append("manufacturer: ").append(Build.MANUFACTURER).append('\n');
        sb.append("brand: ").append(Build.BRAND).append('\n');
        sb.append("model: ").append(Build.MODEL).append('\n');
        sb.append("device: ").append(Build.DEVICE).append('\n');
        sb.append("product: ").append(Build.PRODUCT).append('\n');
        sb.append("board: ").append(Build.BOARD).append('\n');
        sb.append("hardware: ").append(Build.HARDWARE).append('\n');
        sb.append("supportedAbis: ").append(join(Build.SUPPORTED_ABIS)).append('\n');
        sb.append("supported64BitAbis: ").append(join(Build.SUPPORTED_64_BIT_ABIS)).append('\n');
        sb.append("supported32BitAbis: ").append(join(Build.SUPPORTED_32_BIT_ABIS)).append('\n');
        sb.append("locale: ").append(Locale.getDefault()).append('\n');
        sb.append("timezone: ").append(java.util.TimeZone.getDefault().getID()).append('\n');

        sb.append("\n--- display ---\n");
        try {
            WindowManager wm = (WindowManager) app.getSystemService(Context.WINDOW_SERVICE);
            Display d = wm == null ? null : wm.getDefaultDisplay();
            if (d != null) {
                DisplayMetrics m = new DisplayMetrics();
                d.getRealMetrics(m);
                sb.append("realSize: ").append(m.widthPixels).append('x').append(m.heightPixels)
                        .append(" @ ").append(m.densityDpi).append("dpi\n");
                sb.append("refreshRate: ").append(d.getRefreshRate()).append("Hz\n");
                Display.Mode[] modes = d.getSupportedModes();
                sb.append("supportedModes: ");
                for (int i = 0; i < modes.length; i++) {
                    if (i > 0) sb.append(", ");
                    sb.append(modes[i].getModeId()).append('=')
                            .append(Math.round(modes[i].getRefreshRate())).append("Hz/")
                            .append(modes[i].getPhysicalWidth()).append('x')
                            .append(modes[i].getPhysicalHeight());
                }
                sb.append('\n');
                sb.append("currentMode: ").append(d.getMode() == null ? "?" : (
                        d.getMode().getModeId() + "=" + Math.round(d.getMode().getRefreshRate())
                                + "Hz/" + d.getMode().getPhysicalWidth() + "x"
                                + d.getMode().getPhysicalHeight())).append('\n');
            }
        } catch (Throwable t) {
            sb.append("display info error: ").append(t).append('\n');
        }

        sb.append("\n--- leia services ---\n");
        String[] packages = {
                "com.leia.headtrackingservice",
                "com.leialoft.display.config",
                "com.leia.leiaplayer"
        };
        for (String pkg : packages) {
            try {
                PackageInfo pi = app.getPackageManager().getPackageInfo(pkg, 0);
                sb.append(pkg).append(": ").append(pi.versionName)
                        .append(" (").append(pi.getLongVersionCode()).append(")\n");
            } catch (Throwable t) {
                sb.append(pkg).append(": not visible/not installed\n");
            }
        }

        sb.append("\n--- system properties ---\n");
        Map<String, String> props = readSystemProperties();
        if (props.isEmpty()) {
            sb.append("(getprop unavailable)\n");
        } else {
            for (Map.Entry<String, String> e : props.entrySet()) {
                sb.append(e.getKey()).append(": ").append(e.getValue()).append('\n');
            }
        }
        sb.append("==================================================\n");
        return sb.toString();
    }

    // ---- logging internals -------------------------------------------------

    private static void startCapture() {
        captureThread = new Thread(new Runnable() {
            @Override
            public void run() {
                Process process = null;
                try {
                    process = new ProcessBuilder("/system/bin/logcat",
                            "-v", "threadtime", "--pid=" + android.os.Process.myPid())
                            .redirectErrorStream(true)
                            .start();
                    BufferedReader reader = new BufferedReader(
                            new InputStreamReader(process.getInputStream(), StandardCharsets.UTF_8));
                    String line;
                    while ((line = reader.readLine()) != null) {
                        appendLineLocked(line);
                    }
                } catch (Throwable t) {
                    Log.w(TAG, "continuous logcat capture unavailable; export will still snapshot logcat", t);
                } finally {
                    if (process != null) {
                        try { process.destroy(); } catch (Throwable ignored) {}
                    }
                }
            }
        }, "sm64-debug-logcat");
        captureThread.setDaemon(true);
        captureThread.start();
    }

    private static void appendLineLocked(String line) {
        synchronized (LOCK) {
            if (writer == null) return;
            if (bytesWritten > MAX_LOG_BYTES) {
                rotateLocked();
            }
            try {
                writer.write(line);
                writer.write('\n');
                writer.flush();
                bytesWritten += line.length() + 1L;
            } catch (Throwable t) {
                Log.w(TAG, "debug log write failed", t);
                closeLocked();
            }
        }
    }

    private static void writeEnvironmentLocked(String reason) {
        if (writer == null) return;
        try {
            writer.write("\n\n########## " + reason + " ##########\n");
            writer.write(environmentReport(appContext));
            writer.flush();
            bytesWritten = logFile == null ? 0 : logFile.length();
        } catch (Throwable t) {
            Log.w(TAG, "debug environment write failed", t);
        }
    }

    private static void rotateLocked() {
        if (logFile == null) return;
        try {
            closeLocked();
            File old = new File(logFile.getParentFile(), OLD_LOG_NAME);
            if (old.exists()) old.delete();
            logFile.renameTo(old);
            openWriterLocked();
            writeEnvironmentLocked("ROTATED SESSION");
        } catch (Throwable t) {
            Log.w(TAG, "debug log rotation failed", t);
        }
    }

    private static void openWriterLocked() {
        closeLocked();
        if (logFile == null) return;
        try {
            writer = new BufferedWriter(new OutputStreamWriter(
                    new FileOutputStream(logFile, true), StandardCharsets.UTF_8));
            bytesWritten = logFile.length();
        } catch (Throwable t) {
            Log.e(TAG, "cannot open debug log: " + logFile, t);
            writer = null;
            bytesWritten = 0;
        }
    }

    private static void closeLocked() {
        if (writer != null) {
            try { writer.flush(); } catch (Throwable ignored) {}
            try { writer.close(); } catch (Throwable ignored) {}
            writer = null;
        }
    }

    private static void flushLocked() {
        if (writer != null) {
            try { writer.flush(); } catch (Throwable ignored) {}
        }
    }

    private static void copyInto(File file, BufferedWriter out) throws IOException {
        if (file == null || !file.isFile()) return;
        try (BufferedReader r = new BufferedReader(new InputStreamReader(
                new FileInputStream(file), StandardCharsets.UTF_8))) {
            char[] buf = new char[8192];
            int n;
            while ((n = r.read(buf)) > 0) {
                out.write(buf, 0, n);
            }
        }
    }

    private static void copyLogcatInto(BufferedWriter out, String[] args) {
        Process process = null;
        try {
            process = new ProcessBuilder(buildLogcatCommand(args))
                    .redirectErrorStream(true)
                    .start();
            BufferedReader reader = new BufferedReader(
                    new InputStreamReader(process.getInputStream(), StandardCharsets.UTF_8));
            String line;
            long written = 0;
            while ((line = reader.readLine()) != null && written < MAX_SNAPSHOT_BYTES) {
                out.write(line);
                out.write('\n');
                written += line.length() + 1L;
            }
            try { process.waitFor(); } catch (Throwable ignored) {}
        } catch (Throwable t) {
            try {
                out.write("(logcat snapshot unavailable: " + t + ")\n");
            } catch (IOException ignored) {
            }
        } finally {
            if (process != null) {
                try { process.destroy(); } catch (Throwable ignored) {}
            }
        }
    }

    private static java.util.List<String> buildLogcatCommand(String[] args) {
        java.util.List<String> cmd = new java.util.ArrayList<>();
        cmd.add("/system/bin/logcat");
        for (String arg : args) cmd.add(arg);
        return cmd;
    }

    private static String cnsdkVersion(Context app) {
        try (java.io.InputStream in = app.getAssets().open("cnsdk.version")) {
            BufferedReader r = new BufferedReader(new InputStreamReader(in, StandardCharsets.UTF_8));
            String line = r.readLine();
            if (line != null && !line.trim().isEmpty()) return line.trim();
        } catch (Throwable ignored) {}
        return CNSDK_FALLBACK_VERSION;
    }

    /**
     * One getprop invocation, filtered to the properties that identify the
     * Android build, Lume firmware, SoC and Leia stack.
     */
    private static Map<String, String> readSystemProperties() {
        Map<String, String> out = new LinkedHashMap<>();
        Process p = null;
        try {
            p = new ProcessBuilder("/system/bin/getprop").redirectErrorStream(true).start();
            BufferedReader r = new BufferedReader(new InputStreamReader(p.getInputStream()));
            String line;
            while ((line = r.readLine()) != null) {
                if (!line.startsWith("[")) continue;
                int end = line.indexOf("]: [");
                if (end < 0) continue;
                String key = line.substring(1, end);
                int valueStart = end + 4;
                int valueEnd = line.lastIndexOf(']');
                if (valueEnd < valueStart) continue;
                String value = line.substring(valueStart, valueEnd);
                if (isInterestingProperty(key)) out.put(key, value);
            }
            try { p.waitFor(); } catch (Throwable ignored) {}
        } catch (Throwable t) {
            // Caller prints "(getprop unavailable)".
        } finally {
            if (p != null) {
                try { p.destroy(); } catch (Throwable ignored) {}
            }
        }
        return out;
    }

    private static boolean isInterestingProperty(String key) {
        String k = key.toLowerCase(Locale.US);
        if (k.startsWith("ro.build.") || k.startsWith("ro.product.")
                || k.startsWith("ro.board.") || k.startsWith("ro.hardware")
                || k.startsWith("ro.soc.") || k.startsWith("ro.config.")) {
            // ro.config.* is broad; only keep Leia-ish/display-ish values below.
            return !k.startsWith("ro.config.") || k.contains("leia") || k.contains("lume")
                    || k.contains("display");
        }
        return k.contains("leia") || k.contains("lume") || k.contains("firmware")
                || k.contains("display.config");
    }

    private static String join(String[] values) {
        if (values == null || values.length == 0) return "(none)";
        StringBuilder sb = new StringBuilder();
        for (int i = 0; i < values.length; i++) {
            if (i > 0) sb.append(',');
            sb.append(values[i]);
        }
        return sb.toString();
    }
}

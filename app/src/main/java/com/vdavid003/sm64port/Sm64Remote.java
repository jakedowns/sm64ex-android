package com.vdavid003.sm64port;

import android.content.Context;
import android.os.SystemClock;
import android.util.Log;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.util.ArrayDeque;
import java.util.Locale;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/**
 * Opt-in telemetry + remote-input worker ("otsm64remote").
 *
 * The tablet is the authority and the browser at
 * {@code https://omitest.jakedowns.com/otsm64remote/} is a viewer: this class
 * mirrors the numeric state that the game thread already produced and applies
 * at most one A press per command from it. It never changes gameplay, input
 * mappings or the render loop.
 *
 * Config, read once at startup from
 * {@code /sdcard/Android/data/com.vdavid003.sm64port/files/sm64remote.cfg}:
 *
 * <pre>
 *   enabled = false
 *   url =
 *   token = change-me
 *   interval_ms = 160
 * </pre>
 *
 * Absent or {@code enabled = false} means nothing is sent and the native
 * hooks stay no-ops. Full protocol and security notes: OTSM64REMOTE.md
 *
 * Threading: one worker thread owns the C ring's read side and the HTTP
 * connection. It samples the ring at ~30 Hz, coalesces into a bounded
 * drop-oldest queue and posts the newest entry at {@code interval_ms};
 * commands are polled at 10 Hz, and a 500 ms watchdog drops any injected input
 * if the relay goes away.
 */
public final class Sm64Remote {
    public static final String TAG = "SM64Remote";
    public static final String CFG_NAME = "sm64remote.cfg";
    public static final String DEVICE_PATH =
            "/sdcard/Android/data/com.vdavid003.sm64port/files/" + CFG_NAME;

    private static final String DEFAULT_TOKEN = "change-me";
    /* 160 ms = 6.25 pushes/s.  The relay answers 429 to every second push at
     * 12.5 Hz, so the default sits just under its ~6/s limit; backpressure
     * still adapts if a different relay is stricter. */
    private static final int DEFAULT_INTERVAL_MS = 160;
    private static final int MIN_INTERVAL_MS = 20;
    private static final int MAX_INTERVAL_MS = 1000;
    private static final int QUEUE_CAP = 8;
    private static final int SAMPLE_PERIOD_MS = 33;     /* ~30 Hz from the ring */
    private static final int POLL_PERIOD_MS = 100;      /* 10 Hz commands */
    private static final int MAX_PUSH_INTERVAL_MS = 500;
    private static final int PUSH_OK = 1;
    private static final int PUSH_FAIL = 0;
    private static final int PUSH_THROTTLED = -1;
    private static final int WATCHDOG_MS = 500;
    private static final int CONNECT_TIMEOUT_MS = 3000;
    private static final int READ_TIMEOUT_MS = 3000;
    private static final int MAX_RESPONSE_BYTES = 1024;
    private static final Pattern INPUT_RE =
            Pattern.compile("\"input\"\\s*:\\s*\"([A-Za-z0-9_]+)\"");

    /** Parsed config. Never logged with the token in it. */
    public static final class Config {
        public boolean enabled;
        public String url = "";
        public String token = DEFAULT_TOKEN;
        public int intervalMs = DEFAULT_INTERVAL_MS;

        public String describe() {
            return "enabled=" + enabled + " url=" + (url.isEmpty() ? "<unset>" : url)
                    + " interval=" + intervalMs + "ms token="
                    + (DEFAULT_TOKEN.equals(token) ? "default" : "set");
        }
    }

    private static Config sCfg = new Config();
    private static volatile boolean sRunning;
    private static volatile Thread sThread;

    /* The queue between sampling and the network: bounded, drop-oldest. */
    private static final ArrayDeque<String> sQueue = new ArrayDeque<>(QUEUE_CAP);

    /* Diagnostics for `cmd dump` / `cmd remote`. */
    private static volatile String sState = "off";
    private static volatile long sPushed;
    private static volatile long sCommands;
    private static volatile long sErrors;
    private static volatile long sLastPushMs;
    private static volatile long sLastPollOkMs;
    private static volatile String sLastError = "";
    /** Push period; raised while the relay answers 429, decayed on success. */
    private static volatile int sPushIntervalMs = DEFAULT_INTERVAL_MS;

    /* Reused by the worker; no per-tick allocation on the Java side either. */
    private static final byte[] POLL_BUF = new byte[512];

    private Sm64Remote() {}

    /* ------------------------------------------------------------------ config */

    /** Path used when the app's external files dir is unavailable. */
    private static File cfgFile(Context context) {
        Context app = context.getApplicationContext();
        File base = app.getExternalFilesDir(null);
        if (base == null) {
            base = app.getFilesDir();
        }
        return new File(base, CFG_NAME);
    }

    /** Reads the config file. Missing file = disabled with defaults. */
    public static Config load(Context context) {
        Config cfg = new Config();
        File f = cfgFile(context);
        if (!f.isFile()) {
            return cfg;
        }
        try {
            String text = readAll(f);
            for (String raw : text.split("\\r?\\n")) {
                String line = raw.trim();
                if (line.isEmpty() || line.startsWith("#") || line.startsWith(";")) {
                    continue;
                }
                int eq = line.indexOf('=');
                if (eq <= 0) {
                    continue;
                }
                String key = line.substring(0, eq).trim().toLowerCase(Locale.ROOT);
                String value = line.substring(eq + 1).trim();
                // Tolerate the trailing comments the examples carry.
                switch (key) {
                    case "enabled":
                        cfg.enabled = isTrue(value);
                        break;
                    case "url":
                        cfg.url = value;
                        break;
                    case "token":
                        cfg.token = value;
                        break;
                    case "interval_ms": {
                        try {
                            int v = Integer.parseInt(value);
                            cfg.intervalMs = Math.max(MIN_INTERVAL_MS,
                                    Math.min(MAX_INTERVAL_MS, v));
                        } catch (NumberFormatException ignored) {
                            cfg.intervalMs = DEFAULT_INTERVAL_MS;
                        }
                        break;
                    }
                    default:
                        break;
                }
            }
        } catch (Throwable t) {
            Log.w(TAG, "config read failed: " + t);
            return new Config();
        }
        return cfg;
    }

    private static boolean isTrue(String v) {
        String s = v.trim().toLowerCase(Locale.ROOT);
        return s.equals("1") || s.equals("true") || s.equals("yes") || s.equals("on");
    }

    private static String readAll(File f) throws IOException {
        try (FileInputStream in = new FileInputStream(f)) {
            return new String(drain(in, 4096), StandardCharsets.UTF_8);
        }
    }

    private static byte[] drain(InputStream in, int max) throws IOException {
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        byte[] buf = new byte[512];
        int n;
        while ((n = in.read(buf)) > 0 && out.size() < max) {
            out.write(buf, 0, Math.min(n, max - out.size()));
        }
        return out.toByteArray();
    }

    /* --------------------------------------------------------------- lifecycle */

    /** Reads the config and starts the worker when it says enabled = true. */
    public static synchronized void start(Context context) {
        stop();
        Config cfg = load(context);
        if (!cfg.enabled || cfg.url.isEmpty()) {
            sCfg = cfg;
            sState = cfg.enabled ? "no-url" : "off";
            /* Exactly one line for the disabled case; never the token. */
            Log.i(TAG, "disabled" + (cfg.enabled ? " (enabled = true but url is empty)" : ""));
            return;
        }
        sCfg = cfg;
        sPushed = 0;
        sCommands = 0;
        sErrors = 0;
        sLastPushMs = 0;
        sLastPollOkMs = SystemClock.uptimeMillis();
        sLastError = "";
        sState = "on";
        sPushIntervalMs = cfg.intervalMs;
        nativeSetRemoteEnabled(1);
        Log.i(TAG, "enabled url=" + cfg.url + " interval=" + cfg.intervalMs
                + "ms token=" + (DEFAULT_TOKEN.equals(cfg.token) ? "default" : "set"));
        sRunning = true;
        Thread t = new Thread(Sm64Remote::worker, "sm64remote");
        t.setDaemon(true);
        sThread = t;
        t.start();
    }

    /** Stops the worker (one "disabled" line, no token). */
    public static synchronized void stop() {
        boolean wasRunning = sRunning;
        sRunning = false;
        Thread t = sThread;
        sThread = null;
        if (t != null) {
            t.interrupt();
            try {
                t.join(500);
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
            }
        }
        nativeSetRemoteEnabled(0);
        synchronized (sQueue) {
            sQueue.clear();
        }
        if (wasRunning) {
            sState = "off";
            Log.i(TAG, "disabled");
        }
    }

    /** `cmd remote reload`: re-read the config and restart if it changed. */
    public static synchronized String reload(Context context) {
        start(context);
        Log.i(TAG, "reload " + sCfg.describe());
        return statusLine();
    }

    /**
     * Test hook: queue exactly the press a relay `{"input":"jump"}` produces,
     * without the network. Used to measure the game-side half of the latency
     * on its own (see OTSM64REMOTE.md).
     */
    static void queueTestJump() {
        if (!sRunning) {
            Log.i(TAG, "test input ignored: bridge is off");
            return;
        }
        nativeRemoteQueueJump();
        sCommands++;
        Log.i(TAG, "cmd jump -> queued (local test hook)");
    }

    /** One line for `cmd dump`. Never contains the token. */
    public static String statusLine() {
        return "remote=" + sState
                + " url=" + (sCfg.url.isEmpty() ? "<unset>" : sCfg.url)
                + " interval=" + sCfg.intervalMs
                + " pushMs=" + sPushIntervalMs
                + " pushed=" + sPushed
                + " cmds=" + sCommands
                + " err=" + sErrors
                + " captured=" + nativeRemoteStat(4)
                + " dropped=" + nativeRemoteStat(5)
                + " applied=" + nativeRemoteStat(2)
                + " expired=" + nativeRemoteStat(3)
                + " depth=" + nativeRemoteStat(8)
                + (sLastError.isEmpty() ? "" : " lastErr=\"" + sLastError + "\"")
                + (sLastPushMs > 0
                        ? " lastPushAgo=" + (SystemClock.uptimeMillis() - sLastPushMs) + "ms"
                        : "");
    }

    /* ------------------------------------------------------------------ worker */

    private static void worker() {
        final int baseInterval = sCfg.intervalMs;
        final String pushUrl = endpoint(sCfg.url, "push");
        final String cmdUrl = endpoint(sCfg.url, "commands");
        long nextSample = SystemClock.uptimeMillis();
        long nextPost = nextSample;
        long nextPoll = nextSample;
        boolean watchdogFired = false;

        sPushIntervalMs = baseInterval;

        while (sRunning) {
            long now = SystemClock.uptimeMillis();

            /* Sample the C ring at the simulation rate; posting is slower than
             * that, so the queue is the decimation point (drop-oldest). */
            if (now >= nextSample) {
                sample();
                nextSample = now + SAMPLE_PERIOD_MS;
            }

            /* Commands first. One HTTP round trip is 60-300 ms on this link,
             * so a push must never sit in front of a due poll: the viewer's
             * jump would queue behind it. */
            if (now >= nextPoll) {
                nextPoll = now + POLL_PERIOD_MS;
                if (pollCommands(cmdUrl)) {
                    sLastPollOkMs = now;
                    watchdogFired = false;
                } else if (!watchdogFired
                        && now - sLastPollOkMs > WATCHDOG_MS) {
                    /* The relay is gone: make sure no queued press can fire
                     * when it comes back. The C side also expires at 500 ms. */
                    nativeClearRemoteInput();
                    watchdogFired = true;
                }
            }

            if (now >= nextPost) {
                String body = takeNewest();
                if (body != null) {
                    int result = post(pushUrl, body);
                    if (result == PUSH_OK) {
                        watchdogFired = false;
                        /* Recover towards the configured cadence. */
                        sPushIntervalMs = Math.max(baseInterval,
                                sPushIntervalMs - Math.max(10, baseInterval / 4));
                    } else if (result == PUSH_THROTTLED) {
                        /* The relay rate-limits pushes (429): back off instead
                         * of spending the thread on requests it will reject,
                         * which is what starves the command poll. */
                        sPushIntervalMs = Math.min(MAX_PUSH_INTERVAL_MS,
                                Math.max(baseInterval, sPushIntervalMs * 2));
                    }
                }
                nextPost = now + sPushIntervalMs;
            }

            long wake = Math.min(nextSample, Math.min(nextPost, nextPoll));
            long sleepMs = Math.min(50L, Math.max(1L, wake - SystemClock.uptimeMillis()));
            try {
                Thread.sleep(sleepMs);
            } catch (InterruptedException e) {
                break;
            }
        }
        sState = "off";
    }

    /** Drains the C ring into the drop-oldest queue; keeps only the newest. */
    private static void sample() {
        String newest = null;
        int n;
        int guard = 0;
        while (guard++ < 64 && (n = nativePollRemoteState(POLL_BUF)) > 0) {
            newest = new String(POLL_BUF, 0, n, StandardCharsets.UTF_8);
        }
        if (newest == null) {
            return; /* nothing simulated since the last pass (paused/title) */
        }
        synchronized (sQueue) {
            if (sQueue.size() >= QUEUE_CAP) {
                sQueue.removeFirst(); /* drop-oldest */
            }
            sQueue.addLast(newest);
        }
    }

    private static String takeNewest() {
        synchronized (sQueue) {
            String newest = null;
            while (!sQueue.isEmpty()) {
                newest = sQueue.pollFirst();
            }
            return newest;
        }
    }

    /* -------------------------------------------------------------------- HTTP */

    static String endpoint(String base, String action) {
        String u = base.trim();
        if (!u.endsWith(".php")) {
            if (!u.endsWith("/")) {
                u = u + "/";
            }
            u = u + "state.php";
        }
        return u + (u.indexOf('?') >= 0 ? "&" : "?") + "action=" + action;
    }

    private static int post(String url, String body) {
        byte[] bytes = body.getBytes(StandardCharsets.UTF_8);
        HttpURLConnection c = null;
        try {
            c = open(url, "POST", bytes.length);
            try (OutputStream out = c.getOutputStream()) {
                out.write(bytes);
            }
            int code = c.getResponseCode();
            InputStream in = code >= 400 ? c.getErrorStream() : c.getInputStream();
            if (in != null) {
                drain(in, MAX_RESPONSE_BYTES);
                in.close();
            }
            if (code == 429) {
                noteError("push HTTP 429");
                return PUSH_THROTTLED;
            }
            if (code < 200 || code >= 300) {
                noteError("push HTTP " + code);
                return PUSH_FAIL;
            }
            sPushed++;
            sLastPushMs = SystemClock.uptimeMillis();
            sState = "on";
            return PUSH_OK;
        } catch (Throwable t) {
            noteError("push " + t.getClass().getSimpleName());
            return PUSH_FAIL;
        }
        /* No disconnect(): reading the response to EOF returns the socket to
         * the JDK's keep-alive pool, which is what makes 12.5 pushes/s plus
         * 10 polls/s cheap. */
    }

    private static boolean pollCommands(String url) {
        HttpURLConnection c = null;
        try {
            c = open(url, "GET", -1);
            int code = c.getResponseCode();
            if (code < 200 || code >= 300) {
                noteError("commands HTTP " + code);
                return false;
            }
            InputStream in = c.getInputStream();
            byte[] body = drain(in, MAX_RESPONSE_BYTES);
            in.close();
            applyCommands(new String(body, StandardCharsets.UTF_8));
            return true;
        } catch (Throwable t) {
            noteError("commands " + t.getClass().getSimpleName());
            return false;
        }
    }

    /** Only "jump" is wired to a button; anything else is ignored. */
    private static void applyCommands(String text) {
        Matcher m = INPUT_RE.matcher(text);
        while (m.find()) {
            String input = m.group(1);
            if ("jump".equalsIgnoreCase(input)) {
                nativeRemoteQueueJump();
                sCommands++;
                /* Evidence line for the latency test (one per command). */
                Log.i(TAG, "cmd jump -> queued");
            } else {
                Log.i(TAG, "cmd ignored: " + input);
            }
        }
    }

    private static HttpURLConnection open(String url, String method, int bodyLen)
            throws IOException {
        URL u = new URL(url);
        HttpURLConnection c = (HttpURLConnection) u.openConnection();
        c.setRequestMethod(method);
        c.setConnectTimeout(CONNECT_TIMEOUT_MS);
        c.setReadTimeout(READ_TIMEOUT_MS);
        c.setUseCaches(false);
        c.setInstanceFollowRedirects(true);
        c.setRequestProperty("Connection", "keep-alive");
        c.setRequestProperty("Accept", "application/json");
        /* The token travels as a header, never in the URL (which is logged,
         * screenshotted and kept by proxies). */
        c.setRequestProperty("X-Sm64-Token", sCfg.token);
        if (bodyLen >= 0) {
            c.setDoOutput(true);
            c.setRequestProperty("Content-Type", "application/json; charset=utf-8");
            c.setFixedLengthStreamingMode(bodyLen);
        }
        return c;
    }

    private static void noteError(String message) {
        sErrors++;
        if (!message.equals(sLastError)) {
            sLastError = message;
            Log.w(TAG, "relay error: " + message);
        }
    }

    /* ------------------------------------------------------------------- JNI */

    static native void nativeSetRemoteEnabled(int on);
    static native int nativePollRemoteState(byte[] buf);
    static native void nativeRemoteQueueJump();
    static native void nativeClearRemoteInput();
    static native int nativeRemoteStat(int which);
}

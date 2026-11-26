package com.vdavid003.sm64port;

import android.content.Context;
import android.content.SharedPreferences;

/**
 * Cam-stick response curve, shared by the on-screen stick (heatmap + input
 * shaping), the curve editor in the CAMERA panel, and the native camera.
 *
 * Representation: five points (x, y) with x strictly increasing 0..1 and y
 * non-decreasing 0..1.  The two end points are fixed at (0,0) and (1,1); the
 * three interior points are draggable.  Interpolation is Fritsch-Carlson
 * monotone cubic Hermite, the same code as native
 * (app/jni/src/src/game/customcam.c) so the graph, the heatmap and the camera
 * never disagree.  Monotonicity is enforced on drag, so the curve cannot fold
 * back and invert the stick.
 *
 * Reference: https://en.wikipedia.org/wiki/Monotone_cubic_interpolation
 * (tangent limiting; see also Heckbert's variant in
 * https://signalsmith-audio.co.uk/writing/2021/monotonic-smooth-interpolation/).
 *
 * Native keys (fx 290-299, gfx_opengl_ssfx_set -> custom_camera_stick_fx_set):
 *   290 curve on     291 deadzone %      292 heatmap on
 *   293/294/295 interior point packed (x<<8)|y
 *   296 spring %     297 damping %       298 R mode (0 toggle, 1 hold, 2 snap)
 *   299 vanilla digital C-stick on
 */
public final class StickCurve {
    public static final int N = 5;
    public static final int KEY_BASE = 290;

    /* Interior points (draggable).  End points are implicit. */
    public static final float[] PX = {0f, 0.25f, 0.50f, 0.75f, 1f};
    public static final float[] PY = {0f, 0.25f, 0.50f, 0.75f, 1f};
    public static int deadzonePct = 10;
    public static boolean enabled = true;
    public static boolean heatmap = true;
    public static int springPct = 100;
    public static int dampingPct = 100;
    public static int rMode = 0;
    public static boolean cstickDigital = true;

    /** Bumped on every change; views compare it against their cached copy. */
    public static int version = 1;

    private static final float[] TAN = new float[N];
    private static boolean tangentsValid;
    private static Context appCtx;

    private StickCurve() {
    }

    /* ---------------------------------------------------------------- curve */

    private static void rebuildTangents() {
        float[] d = new float[N - 1];
        for (int i = 0; i < N - 1; i++) {
            float h = PX[i + 1] - PX[i];
            d[i] = h > 1e-6f ? (PY[i + 1] - PY[i]) / h : 0f;
        }
        TAN[0] = d[0];
        TAN[N - 1] = d[N - 2];
        for (int i = 1; i < N - 1; i++) {
            TAN[i] = (d[i - 1] + d[i]) * 0.5f;
        }
        for (int i = 0; i < N - 1; i++) {
            if (d[i] == 0f) {
                TAN[i] = 0f;
                TAN[i + 1] = 0f;
                continue;
            }
            float a = TAN[i] / d[i];
            float b = TAN[i + 1] / d[i];
            float s = a * a + b * b;
            if (s > 9f) {
                float t = 3f / (float) Math.sqrt(s);
                TAN[i] = t * a * d[i];
                TAN[i + 1] = t * b * d[i];
            }
        }
        tangentsValid = true;
    }

    /** Monotone cubic through the five points, u in 0..1 (post-deadzone). */
    public static float evalRaw(float u) {
        if (!tangentsValid) {
            rebuildTangents();
        }
        if (u <= PX[0]) return PY[0];
        if (u >= PX[N - 1]) return PY[N - 1];
        int i = 0;
        while (i < N - 2 && u > PX[i + 1]) {
            i++;
        }
        float h = PX[i + 1] - PX[i];
        if (h < 1e-6f) return PY[i + 1];
        float t = (u - PX[i]) / h;
        float t2 = t * t;
        float t3 = t2 * t;
        float v = (2f * t3 - 3f * t2 + 1f) * PY[i]
                + (t3 - 2f * t2 + t) * h * TAN[i]
                + (-2f * t3 + 3f * t2) * PY[i + 1]
                + (t3 - t2) * h * TAN[i + 1];
        if (v < PY[i]) v = PY[i];
        if (v > PY[i + 1]) v = PY[i + 1];
        if (v < 0f) v = 0f;
        if (v > 1f) v = 1f;
        return v;
    }

    /** Effective output for a raw deflection of u (0..1 of the stick radius). */
    public static float effective(float u) {
        if (!effectiveEnabled()) {
            return u;
        }
        if (u <= 0f) return 0f;
        if (u > 1f) u = 1f;
        float dz = deadzonePct / 100f;
        if (u <= dz) return 0f;
        return evalRaw((u - dz) / (1f - dz));
    }

    public static boolean effectiveEnabled() {
        return enabled;
    }

    private static void sanitise() {
        PX[0] = 0f;
        PY[0] = 0f;
        PX[N - 1] = 1f;
        PY[N - 1] = 1f;
        for (int i = 1; i < N - 1; i++) {
            if (PX[i] < PX[i - 1] + 0.02f) PX[i] = PX[i - 1] + 0.02f;
            if (PY[i] < PY[i - 1]) PY[i] = PY[i - 1];
            if (PX[i] > 0.98f) PX[i] = 0.98f;
            if (PY[i] > 1f) PY[i] = 1f;
        }
        tangentsValid = false;
    }

    /* ------------------------------------------------------------ mutations */

    /** Drag interior point i (1..N-2); x stays between its neighbours. */
    public static void setPoint(int i, float x, float y) {
        if (i < 1 || i > N - 2) return;
        PX[i] = x;
        PY[i] = y;
        sanitise();
        changed();
    }

    public static void setDeadzone(int pct) {
        deadzonePct = Math.max(0, Math.min(90, pct));
        changed();
    }

    public static void setEnabled(boolean on) {
        enabled = on;
        changed();
    }

    public static void setHeatmap(boolean on) {
        heatmap = on;
        changed();
    }

    public static void setSpring(int pct) {
        springPct = Math.max(10, Math.min(300, pct));
        changed();
    }

    public static void setDamping(int pct) {
        dampingPct = Math.max(40, Math.min(200, pct));
        changed();
    }

    public static void setRMode(int mode) {
        rMode = Math.max(0, Math.min(2, mode));
        changed();
    }

    /** RESET: straight line through the corners (a linear response). */
    public static void resetLinear() {
        for (int i = 0; i < N; i++) {
            PX[i] = i / (float) (N - 1);
            PY[i] = i / (float) (N - 1);
        }
        tangentsValid = false;
        changed();
    }

    /** One entry point for the editor: persist, push to native, redraw. */
    private static void changed() {
        version++;
        pushNative();
        save();
        notifyViews();
    }

    /* ---------------------------------------------------------------- native */

    public static void pushNative() {
        sm64portActivity a = sm64portActivity.instance;
        if (a == null) {
            return;
        }
        a.stickFx(KEY_BASE + 0, enabled ? 1 : 0);
        a.stickFx(KEY_BASE + 1, deadzonePct);
        a.stickFx(KEY_BASE + 2, heatmap ? 1 : 0);
        for (int i = 1; i < N - 1; i++) {
            int px = Math.round(PX[i] * 255f);
            int py = Math.round(PY[i] * 255f);
            a.stickFx(KEY_BASE + 2 + i, (px << 8) | py);
        }
        a.stickFx(KEY_BASE + 6, springPct);
        a.stickFx(KEY_BASE + 7, dampingPct);
        a.stickFx(KEY_BASE + 8, rMode);
        a.stickFx(KEY_BASE + 9, cstickDigital ? 1 : 0);
    }

    /** Vanilla-mode digital C-stick mapping (fx 299) without touching the rest. */
    public static void applyCstickDigital() {
        sm64portActivity a = sm64portActivity.instance;
        if (a != null) {
            a.stickFx(KEY_BASE + 9, cstickDigital ? 1 : 0);
        }
        save();
    }

    private static void notifyViews() {
        sm64portActivity a = sm64portActivity.instance;
        if (a != null) {
            a.refreshTouchOverlay();
            a.refreshCamStickPanel();
        }
    }

    /* ----------------------------------------------------------- persistence */

    private static SharedPreferences prefs() {
        if (appCtx == null) {
            sm64portActivity a = sm64portActivity.instance;
            if (a != null) appCtx = a.getApplicationContext();
        }
        return appCtx == null ? null : appCtx.getSharedPreferences("camstick", 0);
    }

    public static void save() {
        SharedPreferences p = prefs();
        if (p == null) return;
        StringBuilder sb = new StringBuilder();
        for (int i = 1; i < N - 1; i++) {
            if (sb.length() > 0) sb.append(';');
            sb.append(PX[i]).append(',').append(PY[i]);
        }
        p.edit()
                .putString("points", sb.toString())
                .putInt("dz", deadzonePct)
                .putBoolean("on", enabled)
                .putBoolean("heat", heatmap)
                .putInt("spring", springPct)
                .putInt("damp", dampingPct)
                .putInt("rmode", rMode)
                .putBoolean("cdig", cstickDigital)
                .apply();
    }

    public static void load(Context c) {
        appCtx = c.getApplicationContext();
        SharedPreferences p = prefs();
        if (p != null) {
            String s = p.getString("points", "");
            if (s != null && s.length() > 0) {
                String[] parts = s.split(";");
                for (int i = 1; i < N - 1 && (i - 1) < parts.length; i++) {
                    String[] xy = parts[i - 1].split(",");
                    if (xy.length == 2) {
                        try {
                            PX[i] = Float.parseFloat(xy[0]);
                            PY[i] = Float.parseFloat(xy[1]);
                        } catch (NumberFormatException ignored) {
                        }
                    }
                }
            }
            deadzonePct = p.getInt("dz", deadzonePct);
            enabled = p.getBoolean("on", enabled);
            heatmap = p.getBoolean("heat", heatmap);
            springPct = p.getInt("spring", springPct);
            dampingPct = p.getInt("damp", dampingPct);
            rMode = p.getInt("rmode", rMode);
            cstickDigital = p.getBoolean("cdig", cstickDigital);
        }
        sanitise();
        version++;
        pushNative();
    }

    public static String describe() {
        StringBuilder sb = new StringBuilder();
        sb.append("curve=").append(enabled ? 1 : 0)
                .append(" dz=").append(deadzonePct)
                .append(" heat=").append(heatmap ? 1 : 0);
        for (int i = 1; i < N - 1; i++) {
            sb.append(String.format(" p%d=%.2f/%.2f", i, PX[i], PY[i]));
        }
        sb.append(" spring=").append(springPct)
                .append("/").append(dampingPct)
                .append(" rmode=").append(rMode)
                .append(" cdig=").append(cstickDigital ? 1 : 0);
        return sb.toString();
    }
}

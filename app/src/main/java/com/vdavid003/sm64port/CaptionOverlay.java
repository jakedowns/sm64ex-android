package com.vdavid003.sm64port;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Typeface;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.View;

/**
 * Caption text drawn as a crisp Android overlay above the game surface.
 *
 * <p>The strings still come from the game: {@code print_caption()} and
 * {@code print_caption2_small()} in {@code game/print.c} publish what they
 * were asked to show, and the native glyph renderer is switched off while this
 * view owns the text (DEBUG &gt; SYS &gt; CAPTIONS, default JAVA). Nothing about
 * what a caption says, or when it appears, is decided here — this class only
 * polls the JNI getters at ~15 Hz and draws the result.
 *
 * <p>It is a normal Android view in screen space: one draw feeds both eyes of
 * the Leia weave, it sits above the interlaced view and below the debug panel,
 * and it never takes input. Because it is outside the GL surface it is
 * <b>not</b> part of the feedback-outbox frame grab (a GL readback); use
 * GAME FONT when a shot has to carry the caption text.
 */
class CaptionOverlay extends View {
    private static final String TAG = "SM64Leia";
    /** 66 ms ≈ 15 Hz: fast enough for a caption change, cheap enough to idle. */
    private static final long POLL_MS = 66;

    /* Native caption space (see print.c / print_caption callers): x is 0..320,
     * y is bottom-origin in a 240-high frame, so the Java top is
     * (224 - y) / 240 of the view height. */
    private static final float NATIVE_W = 320f;
    private static final float NATIVE_H = 240f;
    private static final float NATIVE_Y_TOP = 224f;

    private final Paint mainPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint smallPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Handler handler = new Handler(Looper.getMainLooper());

    private boolean javaMode = true;
    private int sizePct = 100;
    private int opacityPct = 90;
    private float baseSizePx;

    private String mainText = "";
    private String smallText = "";
    private int mainX = 160, mainY = 160, mainScaleMilli = 500;
    private int smallX = 160, smallY = 160, smallScaleMilli = 250;
    private int modeReasserts;

    private final Runnable poll = new Runnable() {
        @Override
        public void run() {
            pollOnce();
            handler.postDelayed(this, POLL_MS);
        }
    };

    CaptionOverlay(Context context) {
        super(context);
        setWillNotDraw(false);
        setClickable(false);
        setFocusable(false);
        setFocusableInTouchMode(false);
        /* Touches belong to the game / touch overlay underneath. */
        setImportantForAccessibility(IMPORTANT_FOR_ACCESSIBILITY_NO);

        Typeface face = Typeface.create(Typeface.MONOSPACE, Typeface.BOLD);
        for (Paint p : new Paint[]{mainPaint, smallPaint}) {
            p.setTypeface(face);
            p.setTextAlign(Paint.Align.CENTER);
            p.setSubpixelText(true);
            p.setAntiAlias(true);
            /* Subtle dark halo so light levels (snow, sky) stay readable. */
            p.setShadowLayer(dp(2.5f), 0f, dp(1f), 0xC0000000);
        }
        mainPaint.setColor(0xFFF2F4F8);
        smallPaint.setColor(0xFFCFE4FF);
        recomputeBaseSize();
        applyPaints();
    }

    /** false = GAME FONT: the native HUD glyphs draw, this view stays blank. */
    void setJavaMode(boolean on) {
        if (javaMode == on) {
            return;
        }
        javaMode = on;
        /* Pick up the live caption immediately instead of waiting for the
         * next poll, so an A/B switch shows text on the same frame. */
        if (on) {
            pollOnce();
        }
        invalidate();
    }

    /** 50..200 %; 100 matches a readable 17 sp monospace line. */
    void setSizePct(int pct) {
        int v = Math.max(50, Math.min(200, pct));
        if (sizePct == v) {
            return;
        }
        sizePct = v;
        recomputeBaseSize();
        invalidate();
    }

    /** 0..100 % opacity of the text (the shadow scales with it). */
    void setOpacityPct(int pct) {
        int v = Math.max(0, Math.min(100, pct));
        if (opacityPct == v) {
            return;
        }
        opacityPct = v;
        applyPaints();
        invalidate();
    }

    @Override
    public boolean onTouchEvent(android.view.MotionEvent event) {
        return false;
    }

    @Override
    protected void onAttachedToWindow() {
        super.onAttachedToWindow();
        handler.removeCallbacks(poll);
        handler.post(poll);
    }

    @Override
    protected void onDetachedFromWindow() {
        handler.removeCallbacks(poll);
        super.onDetachedFromWindow();
    }

    /**
     * Keep the native draw suppression in sync with this view's mode.
     *
     * The flag lives in the game's statics and is normally set by the activity,
     * but a Leia/3D surface change (or a restarted engine) can leave it at the
     * game-font default while the overlay still thinks it owns the text. That
     * is the 3D regression: the caption reappeared in HUD glyphs while
     * captions were set to JAVA. Re-asserting here is one integer comparison
     * per poll, converges within 66 ms, and also turns suppression back OFF
     * when GAME FONT is selected, so the two can never disagree.
     */
    private void syncNativeMode() {
        int want = javaMode ? 1 : 0;
        int have;
        try {
            have = sm64portActivity.nativeGetCaptionMode();
        } catch (Throwable t) {
            return;
        }
        if (have == want) {
            return;
        }
        try {
            sm64portActivity.nativeSetCaptionMode(want);
        } catch (Throwable t) {
            return;
        }
        modeReasserts++;
        /* First fix is interesting; afterwards only a heartbeat so a logcat
         * tail is not flooded by a persistent mismatch. */
        if (modeReasserts == 1 || (modeReasserts % 150) == 0) {
            Log.w(TAG, "CAPTIONS native mode " + have + " != " + want
                    + " -> re-asserted " + want + " count=" + modeReasserts);
        }
    }

    private void pollOnce() {
        /* Runs in both modes: JAVA re-suppresses the native glyphs, GAME FONT
         * re-enables them. Checked before the early return. */
        syncNativeMode();
        if (!javaMode || getWindowVisibility() != VISIBLE) {
            return;
        }
        boolean changed = false;

        String m = sm64portActivity.nativeGetCaptionText(0);
        String s = sm64portActivity.nativeGetCaptionText(1);
        if (!m.equals(mainText)) {
            mainText = m;
            changed = true;
        }
        if (!s.equals(smallText)) {
            smallText = s;
            changed = true;
        }
        int mx = sm64portActivity.nativeGetCaptionPos(0, 0);
        int my = sm64portActivity.nativeGetCaptionPos(0, 1);
        int ms = sm64portActivity.nativeGetCaptionPos(0, 2);
        int sx = sm64portActivity.nativeGetCaptionPos(1, 0);
        int sy = sm64portActivity.nativeGetCaptionPos(1, 1);
        int ss = sm64portActivity.nativeGetCaptionPos(1, 2);
        if (mx != mainX || my != mainY || ms != mainScaleMilli
                || sx != smallX || sy != smallY || ss != smallScaleMilli) {
            mainX = mx;
            mainY = my;
            mainScaleMilli = ms;
            smallX = sx;
            smallY = sy;
            smallScaleMilli = ss;
            changed = true;
        }
        if (changed) {
            invalidate();
        }
    }

    /** 17 sp at 100 %: readable on the panel without dominating the frame. */
    private void recomputeBaseSize() {
        baseSizePx = getResources().getDisplayMetrics().scaledDensity
                * 17f * (sizePct / 100f);
    }

    private void applyPaints() {
        mainPaint.setAlpha(Math.round(255f * opacityPct / 100f));
        smallPaint.setAlpha(Math.round(235f * opacityPct / 100f));
        /* Shadow alpha follows the text so a low opacity stays subtle. */
        int shadowAlpha = Math.round(0xC0 * opacityPct / 100f);
        mainPaint.setShadowLayer(dp(2.5f), 0f, dp(1f), shadowAlpha << 24);
        smallPaint.setShadowLayer(dp(2f), 0f, dp(1f), shadowAlpha << 24);
    }

    @Override
    protected void onDraw(Canvas canvas) {
        super.onDraw(canvas);
        if (!javaMode) {
            return;
        }
        int w = getWidth();
        int h = getHeight();
        if (w <= 0 || h <= 0) {
            return;
        }
        drawLine(canvas, mainText, mainX, mainY, mainScaleMilli, mainPaint, w, h);
        drawLine(canvas, smallText, smallX, smallY, smallScaleMilli, smallPaint, w, h);
    }

    /** Draw one caption at its native position and glyph scale, shrinking only
     * if a long line would run off the view (the native path overhangs). */
    private void drawLine(Canvas canvas, String text, int nativeX, int nativeY,
                          int scaleMilli, Paint paint, int w, int h) {
        if (text == null || text.isEmpty()) {
            return;
        }
        float cx = w * (nativeX / NATIVE_W);
        float top = h * ((NATIVE_Y_TOP - nativeY) / NATIVE_H);

        /* Native scale 0.5 is the main caption, 0.25 the small one; keep the
         * small line proportional but not illegible on a dense panel. */
        float size = baseSizePx * Math.max(0.55f, scaleMilli / 500f);
        paint.setTextSize(size);
        float maxWidth = w * 0.96f;
        float width = paint.measureText(text);
        if (width > maxWidth && width > 0f) {
            paint.setTextSize(size * (maxWidth / width));
        }
        /* Native y is the glyph-rect top; put the font's top edge there. */
        float baseline = top - paint.ascent();
        canvas.drawText(text, cx, baseline, paint);
    }

    private float dp(float v) {
        return v * getResources().getDisplayMetrics().density;
    }
}

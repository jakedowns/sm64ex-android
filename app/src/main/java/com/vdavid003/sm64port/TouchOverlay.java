package com.vdavid003.sm64port;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.view.MotionEvent;
import android.view.View;

/**
 * Touch controls drawn ABOVE the Leia view (screen space, like the debug panel),
 * instead of inside the per-eye game framebuffers.
 *
 * Left half  : floating thumbstick (appears where the thumb lands).
 * Right side : A / B / Z / R buttons.
 *
 * Input is pushed straight into the game via a native override, so the game's
 * own control sprites are not used.
 */
public class TouchOverlay extends View {
    public static final int BTN_A = 0x8000;
    public static final int BTN_B = 0x4000;
    public static final int BTN_Z = 0x2000;
    public static final int BTN_R = 0x0010;
    public static final int BTN_L = 0x0020;
    public static final int BTN_START = 0x1000;
    public static final int BTN_CUP = 0x0008;
    public static final int BTN_CDOWN = 0x0004;
    public static final int BTN_CLEFT = 0x0002;
    public static final int BTN_CRIGHT = 0x0001;
    // Overlay-only camera buttons (not N64 pad bits).
    public static final int BTN_CAM = 0x01000000;
    public static final int BTN_RECENTER = 0x02000000;
    // Overlay-only Select/Back (short press = exit FPV, hold = level select).
    public static final int BTN_BACK = 0x04000000;

    private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);

    private float stickX, stickY;      // base position
    private float knobX, knobY;        // current knob offset position
    private boolean stickActive;
    private int stickPointer = -1;

    // Two-finger camera gestures: pinch = FOV, two-finger drag = cam distance/height.
    // Enabled by default; the CAMERA tab has the toggle.
    public static boolean camGesturesEnabled = true;
    /* Hardware input hides the on-screen controls; touching the screen brings
     * them back. */
    public static volatile long lastTouchMs = 0;
    private boolean controlsHidden;
    private boolean camGesture;
    private int camGestureMode = GESTURE_IDLE;
    private int camPtr1 = -1, camPtr2 = -1;
    private float gSpan0, gCx0, gCy0;
    private float gLastSpan, gLastCy;
    private boolean freecamMove;
    private int gFov0, gDist0, gHeight0;

    /* Two-finger gesture recognition.  A gesture starts undecided and only
     * becomes a pinch or a swipe once the fingers have moved enough to tell
     * the two apart; anything ambiguous is ignored (no camera jumps).
     *   pinch : separation changes > 12% while the midpoint barely moves.
     *   swipe : both fingers travel together > 60dp with ~constant separation. */
    private static final int GESTURE_IDLE = 0;
    private static final int GESTURE_PINCH = 1;
    private static final int GESTURE_SWIPE = 2;
    private static final float PINCH_SPAN_CHANGE = 0.12f;    // 12% separation change
    private static final float SWIPE_SPAN_TOLERANCE = 0.15f; // "roughly constant"
    private static final float GESTURE_SHIFT_DP = 60f;       // common translation cutoff
    private static final float GESTURE_MIN_SPAN_DP = 48f;    // not a two-finger squish
    private static final float START_SPAN_FRACTION = 0.40f;  // of screen width

    private int buttons;
    private final int[] btnIds = {BTN_A, BTN_B, BTN_Z, BTN_R, BTN_START, BTN_L, BTN_BACK};
    private final String[] btnLabels = {"A", "B", "Z", "R", "S", "L", "SEL"};
    private final float[][] btnPos = new float[7][2];
    private int buttonPointer = -1;

    private final float radius = 90f;      // button radius
    private final float stickRadius = 190f;
    private final float knobRadius = 70f;

    private final int[] pressedPointer = {-1, -1, -1, -1, -1, -1, -1};

    // C buttons: smaller cluster (up/down/left/right)
    private final int[] cIds = {BTN_CUP, BTN_CDOWN, BTN_CLEFT, BTN_CRIGHT};
    private final String[] cLabels = {"^", "v", "<", ">"};
    private final float[][] cPos = new float[4][2];
    private final int[] cPressed = {-1, -1, -1, -1};
    private final float cRadius = 58f;

    // Camera buttons: X toggles free camera, Y recentres.
    private final int[] camIds = {BTN_CAM, BTN_RECENTER};
    private final String[] camLabels = {"X", "Y"};
    private final float[][] camPos = new float[2][2];
    private final int[] camPressed = {-1, -1};
    private final float camRadius = 52f;

    // Portal gun buttons (P1 = blue, P2 = orange).  The reticle lives in the
    // game UI pass so it can have its own stereo depth.
    private final float[][] portalPos = new float[2][2];
    private final float portalRadius = 46f;
    private int portalPressed = -1;
    private int portalPointer = -1;

    // One-press swap between the portal gun and the grappling hook.
    private final float[] toolPos = new float[2];
    private final float toolRadius = 46f;
    private boolean toolDown;
    private int toolPointer = -1;

    // Overall opacity of the control layer (0.1 .. 1.0).
    private float overlayAlpha = 1f;

    // Right-hand control: 4 C buttons, or an analog camera stick.
    private boolean analogMode = true;   // analog camera stick by default
    private boolean rstickActive;
    private int rstickPointer = -1;
    private float rstickX, rstickY, rknobX, rknobY;
    private final float[] modePos = new float[2];
    private final float modeRadius = 46f;

    public TouchOverlay(Context c) {
        super(c);
        setFocusable(false);
        setClickable(false);
        // The view must receive touches over the whole panel.
        setWillNotDraw(false);
    }

    /** Overall opacity of the floating controls (1.0 = fully opaque). */
    public void setOverlayAlpha(float a) {
        if (a < 0f) a = 0f;
        if (a > 1f) a = 1f;
        overlayAlpha = a;
        invalidate();
    }

    /** Switch the right-hand control between C buttons and an analog stick. */
    public void setAnalogMode(boolean on) {
        analogMode = on;
        rstickActive = false;
        rstickPointer = -1;
        sm64portActivity.nativeOverlayAnalog(on ? 1 : 0);
        pushInput();
        invalidate();
    }

    private void layoutButtons() {
        float w = getWidth(), h = getHeight();
        btnPos[0][0] = w * 0.90f; btnPos[0][1] = h * 0.70f;   // A
        btnPos[1][0] = w * 0.81f; btnPos[1][1] = h * 0.77f;   // B
        btnPos[2][0] = w * 0.94f; btnPos[2][1] = h * 0.58f;   // Z
        btnPos[3][0] = w * 0.79f; btnPos[3][1] = h * 0.60f;   // R
        btnPos[4][0] = w * 0.50f; btnPos[4][1] = h * 0.88f;   // START (bottom centre)
        btnPos[5][0] = w * 0.90f; btnPos[5][1] = h * 0.84f;   // L (below A)
        btnPos[6][0] = w * 0.38f; btnPos[6][1] = h * 0.88f;   // SELECT/BACK, left of START

        // C-button cluster: cross to the left of A/B
        float cx = w * 0.68f, cy = h * 0.70f, d = cRadius * 1.35f;
        cPos[0][0] = cx;       cPos[0][1] = cy - d;   // up
        cPos[1][0] = cx;       cPos[1][1] = cy + d;   // down
        cPos[2][0] = cx - d;   cPos[2][1] = cy;       // left
        cPos[3][0] = cx + d;   cPos[3][1] = cy;       // right

        // Camera buttons: upper-right, clear of A/B/Z/R.
        camPos[0][0] = w * 0.93f; camPos[0][1] = h * 0.40f;   // X
        camPos[1][0] = w * 0.84f; camPos[1][1] = h * 0.32f;   // Y

        // Portal gun buttons: left of the C cluster, clear of the thumbstick.
        portalPos[0][0] = w * 0.93f; portalPos[0][1] = h * 0.27f;   // P1 (blue)
        portalPos[1][0] = w * 0.84f; portalPos[1][1] = h * 0.19f;   // P2 (orange)

        // Tool swap: one button between the camera and portal clusters.
        toolPos[0] = w * 0.75f; toolPos[1] = h * 0.27f;

        // C-buttons / analog-camera toggle, below the C cluster.
        modePos[0] = w * 0.68f; modePos[1] = h * 0.88f;
    }

    private boolean portalGunHeld() {
        try {
            return sm64portActivity.nativeGetPortalGun() != 0;
        } catch (Throwable t) {
            return false;
        }
    }

    /** Index 0/1 of a portal button hit, else -1. */
    private int hitPortalButton(float x, float y) {
        for (int i = 0; i < 2; i++) {
            float dx = x - portalPos[i][0], dy = y - portalPos[i][1];
            if (dx * dx + dy * dy <= (portalRadius * 1.25f) * (portalRadius * 1.25f)) return i;
        }
        return -1;
    }

    private boolean hitToolButton(float x, float y) {
        float dx = x - toolPos[0], dy = y - toolPos[1];
        return dx * dx + dy * dy <= (toolRadius * 1.25f) * (toolRadius * 1.25f);
    }

    private boolean hitFreecamToggle(float x, float y) {
        if (sm64portActivity.nativeGetCamMode2() != 5) {
            return false;
        }
        return x >= dp(16) && x <= dp(16) + dp(168)
                && y >= dp(16) && y <= dp(16) + dp(46);
    }

    private boolean hitModeToggle(float x, float y) {
        float dx = x - modePos[0], dy = y - modePos[1];
        return dx * dx + dy * dy <= (modeRadius * 1.3f) * (modeRadius * 1.3f);
    }

    /** Index into camIds for a camera-button hit, else -1. */
    private int hitCamButton(float x, float y) {
        for (int i = 0; i < camIds.length; i++) {
            float dx = x - camPos[i][0], dy = y - camPos[i][1];
            if (dx * dx + dy * dy <= (camRadius * 1.3f) * (camRadius * 1.3f)) return i;
        }
        return -1;
    }

    private int hitButton(float x, float y) {
        for (int i = 0; i < btnPos.length; i++) {
            float dx = x - btnPos[i][0], dy = y - btnPos[i][1];
            if (dx * dx + dy * dy <= (radius * 1.25f) * (radius * 1.25f)) return i;
        }
        return -1;
    }

    /** Index into cIds for a C-button hit, else -1. */
    private int hitCButton(float x, float y) {
        /* In analog-camera mode the C cluster is hidden; its invisible hit
         * circles must not steal touches from the right analog stick. */
        if (analogMode) return -1;
        for (int i = 0; i < 4; i++) {
            float dx = x - cPos[i][0], dy = y - cPos[i][1];
            if (dx * dx + dy * dy <= (cRadius * 1.3f) * (cRadius * 1.3f)) return i;
        }
        return -1;
    }

    /**
     * Moves a button press between overlay buttons while a finger is dragged.
     * Only pointers that already own a button participate, so sliding a thumb
     * across the screen never starts an accidental press.
     */
    private void updateButtonDrag(int id, float px, float py) {
        int owner = -1, cowner = -1, camOwner = -1;

        for (int b = 0; b < btnIds.length; b++) {
            if (pressedPointer[b] == id) { owner = b; break; }
        }
        for (int cIdx = 0; cIdx < cIds.length; cIdx++) {
            if (cPressed[cIdx] == id) { cowner = cIdx; break; }
        }
        for (int cIdx = 0; cIdx < camIds.length; cIdx++) {
            if (camPressed[cIdx] == id) { camOwner = cIdx; break; }
        }
        if (owner < 0 && cowner < 0 && camOwner < 0) {
            return;
        }

        int nb = hitButton(px, py);
        int ncb = nb < 0 ? hitCButton(px, py) : -1;
        int ncam = (nb < 0 && ncb < 0) ? hitCamButton(px, py) : -1;
        boolean changed = false;

        if (owner >= 0 && nb != owner) {
            buttons &= ~btnIds[owner];
            pressedPointer[owner] = -1;
            owner = -1;
            changed = true;
        }
        if (cowner >= 0 && ncb != cowner) {
            buttons &= ~cIds[cowner];
            cPressed[cowner] = -1;
            cowner = -1;
            changed = true;
        }
        if (camOwner >= 0 && ncam != camOwner) {
            buttons &= ~camIds[camOwner];
            camPressed[camOwner] = -1;
            camOwner = -1;
            changed = true;
        }

        if (owner < 0 && nb >= 0) {
            buttons |= btnIds[nb];
            pressedPointer[nb] = id;
            changed = true;
        } else if (cowner < 0 && ncb >= 0) {
            buttons |= cIds[ncb];
            cPressed[ncb] = id;
            changed = true;
        } else if (camOwner < 0 && ncam >= 0) {
            buttons |= camIds[ncam];
            camPressed[ncam] = id;
            changed = true;
        }

        if (changed) {
            pushInput();
            invalidate();
        }
    }

    public void setControlsHidden(boolean hidden) {
        if (controlsHidden != hidden) {
            controlsHidden = hidden;
            invalidate();
        }
    }

    @Override
    public boolean onTouchEvent(MotionEvent e) {
        lastTouchMs = System.currentTimeMillis();
        if (controlsHidden) {
            controlsHidden = false;
            invalidate();
        }
        if (btnPos[0][0] == 0) layoutButtons();
        float x = e.getX(), y = e.getY();
        int action = e.getActionMasked();

        switch (action) {
            case MotionEvent.ACTION_DOWN:
            case MotionEvent.ACTION_POINTER_DOWN: {
                if (action == MotionEvent.ACTION_POINTER_DOWN && e.getPointerCount() == 2
                        && !camGesture && startCamGesture(e)) {
                    return true;
                }
                int idx = e.getActionIndex();
                int id = e.getPointerId(idx);
                float px = e.getX(idx), py = e.getY(idx);
                int b = hitButton(px, py);
                int cb = b < 0 ? hitCButton(px, py) : -1;
                int cam = (b < 0 && cb < 0) ? hitCamButton(px, py) : -1;
                boolean tb = (b < 0 && cb < 0 && cam < 0) && hitToolButton(px, py);
                int pb = (b < 0 && cb < 0 && cam < 0 && !tb && portalGunHeld())
                        ? hitPortalButton(px, py) : -1;
                if (b >= 0) {
                    buttons |= btnIds[b];
                    pressedPointer[b] = id;
                } else if (cb >= 0) {
                    buttons |= cIds[cb];
                    cPressed[cb] = id;
                } else if (cam >= 0) {
                    buttons |= camIds[cam];
                    camPressed[cam] = id;
                } else if (tb) {
                    toolDown = true;
                    toolPointer = id;
                    sm64portActivity.nativeToolToggle();
                    invalidate();
                } else if (pb >= 0) {
                    sm64portActivity.nativeShootPortal(pb);
                    portalPressed = pb;
                    portalPointer = id;
                    invalidate();
                } else if (hitFreecamToggle(px, py)) {
                    freecamMove = !freecamMove;
                    sm64portActivity.nativeSetFreecamMove(freecamMove ? 1 : 0);
                } else if (hitModeToggle(px, py)) {
                    analogMode = !analogMode;
                    sm64portActivity.nativeOverlayAnalog(analogMode ? 1 : 0);
                } else if (analogMode && px > getWidth() * 0.45f) {
                    rstickActive = true;
                    rstickPointer = id;
                    rstickX = px;
                    rstickY = py;
                    rknobX = px;
                    rknobY = py;
                } else if (px < getWidth() * 0.45f) {
                    stickActive = true;
                    stickPointer = id;
                    stickX = px;
                    stickY = py;
                    knobX = px;
                    knobY = py;
                }
                pushInput();
                invalidate();
                return true;
            }
            case MotionEvent.ACTION_MOVE: {
                if (camGesture && updateCamGesture(e)) {
                    return true;
                }
                for (int i = 0; i < e.getPointerCount(); i++) {
                    int id = e.getPointerId(i);
                    if (id == stickPointer) {
                        float px = e.getX(i), py = e.getY(i);
                        float dx = px - stickX, dy = py - stickY;
                        float len = (float) Math.hypot(dx, dy);
                        if (len > stickRadius) {
                            dx = dx / len * stickRadius;
                            dy = dy / len * stickRadius;
                        }
                        knobX = stickX + dx;
                        knobY = stickY + dy;
                        pushInput();
                        invalidate();
                    } else if (id == rstickPointer) {
                        float px = e.getX(i), py = e.getY(i);
                        float dx = px - rstickX, dy = py - rstickY;
                        float len = (float) Math.hypot(dx, dy);
                        if (len > stickRadius) {
                            dx = dx / len * stickRadius;
                            dy = dy / len * stickRadius;
                        }
                        rknobX = rstickX + dx;
                        rknobY = rstickY + dy;
                        pushInput();
                        invalidate();
                    } else {
                        // Drag a held button over another one: leave the old
                        // button and press the new one (Z -> A mid-air, etc).
                        updateButtonDrag(id, e.getX(i), e.getY(i));
                    }
                }
                return true;
            }
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_POINTER_UP:
            case MotionEvent.ACTION_CANCEL: {
                if (camGesture) {
                    if (action == MotionEvent.ACTION_CANCEL || action == MotionEvent.ACTION_UP) {
                        camGesture = false;
                    } else {
                        int upId = e.getPointerId(e.getActionIndex());
                        if (upId == camPtr1 || upId == camPtr2) {
                            camGesture = false;
                        }
                    }
                    if (!camGesture) {
                        camPtr1 = camPtr2 = -1;
                        camGestureMode = GESTURE_IDLE;
                        invalidate();
                        return true;
                    }
                }
                int idx = e.getActionIndex();
                int id = e.getPointerId(idx);
                if (id == stickPointer) {
                    stickActive = false;
                    stickPointer = -1;
                }
                if (id == rstickPointer) {
                    rstickActive = false;
                    rstickPointer = -1;
                }
                if (id == portalPointer) {
                    portalPressed = -1;
                    portalPointer = -1;
                    invalidate();
                }
                if (id == toolPointer) {
                    toolDown = false;
                    toolPointer = -1;
                    invalidate();
                }
                for (int b = 0; b < btnIds.length; b++) {
                    if (pressedPointer[b] == id) {
                        buttons &= ~btnIds[b];
                        pressedPointer[b] = -1;
                    }
                }
                for (int c = 0; c < 4; c++) {
                    if (cPressed[c] == id) {
                        buttons &= ~cIds[c];
                        cPressed[c] = -1;
                    }
                }
                for (int c = 0; c < camIds.length; c++) {
                    if (camPressed[c] == id) {
                        buttons &= ~camIds[c];
                        camPressed[c] = -1;
                    }
                }
                pushInput();
                invalidate();
                return true;
            }
        }
        return true;
    }

    private void pushInput() {
        int sx = 0, sy = 0;
        if (stickActive) {
            sx = (int) ((knobX - stickX) / stickRadius * 127f);
            sy = (int) (-(knobY - stickY) / stickRadius * 127f);
        }
        int rx = 0, ry = 0;
        if (rstickActive) {
            rx = (int) ((rknobX - rstickX) / stickRadius * 127f);
            ry = (int) (-(rknobY - rstickY) / stickRadius * 127f);
        }
        sm64portActivity.nativeOverlayStick2(rx, ry, rstickActive ? 1 : 0);
        // Buttons are always merged by the native side. The stick only takes
        // over while the overlay stick is actually held, so a paired gamepad
        // keeps its left stick after tapping the overlay.
        sm64portActivity.nativeOverlayInput(stickActive ? 1 : 0, sx, sy, buttons);
    }

    @Override
    protected void onDraw(Canvas c) {
        super.onDraw(c);
        if (controlsHidden) {
            return;
        }
        if (btnPos[0][0] == 0) layoutButtons();

        // 0 = portal gun, 1 = grapple.  Read once so the portal buttons and
        // the tool button agree within a frame.
        int toolState = 0;
        try {
            toolState = sm64portActivity.nativeGetTool();
        } catch (Throwable ignored) {
        }
        boolean grappleEquipped = toolState == 1;

        // One layer for the whole control set so "Ctl opacity" can fade it.
        int layer = c.saveLayerAlpha(0f, 0f, getWidth(), getHeight(),
                (int) (overlayAlpha * 255f));

        paint.setStyle(Paint.Style.STROKE);
        paint.setStrokeWidth(6f);
        paint.setColor(0x88FFFFFF);

        if (stickActive) {
            c.drawCircle(stickX, stickY, stickRadius, paint);
            paint.setStyle(Paint.Style.FILL);
            paint.setColor(0x66FFFFFF);
            c.drawCircle(knobX, knobY, knobRadius, paint);
        }

        paint.setTextAlign(Paint.Align.CENTER);
        paint.setTextSize(70f);
        for (int i = 0; i < btnIds.length; i++) {
            boolean down = (buttons & btnIds[i]) != 0;
            paint.setStyle(Paint.Style.FILL);
            paint.setColor(down ? 0xCC66FF99 : 0x66FFFFFF);
            c.drawCircle(btnPos[i][0], btnPos[i][1], radius, paint);
            paint.setStyle(Paint.Style.STROKE);
            paint.setColor(0xAA000000);
            c.drawCircle(btnPos[i][0], btnPos[i][1], radius, paint);
            paint.setStyle(Paint.Style.FILL);
            paint.setColor(Color.BLACK);
            c.drawText(btnLabels[i], btnPos[i][0], btnPos[i][1] + 25f, paint);
        }

        paint.setTextSize(46f);
        if (!analogMode) {
            for (int i = 0; i < 4; i++) {
                boolean down = (buttons & cIds[i]) != 0;
                paint.setStyle(Paint.Style.FILL);
                paint.setColor(down ? 0xCC66FF99 : 0x55FFFFFF);
                c.drawCircle(cPos[i][0], cPos[i][1], cRadius, paint);
                paint.setStyle(Paint.Style.STROKE);
                paint.setColor(0xAA000000);
                c.drawCircle(cPos[i][0], cPos[i][1], cRadius, paint);
                paint.setStyle(Paint.Style.FILL);
                paint.setColor(Color.BLACK);
                c.drawText(cLabels[i], cPos[i][0], cPos[i][1] + 16f, paint);
            }
        } else if (rstickActive) {
            paint.setStyle(Paint.Style.STROKE);
            paint.setStrokeWidth(6f);
            paint.setColor(0x88FFFFFF);
            c.drawCircle(rstickX, rstickY, stickRadius, paint);
            paint.setStyle(Paint.Style.FILL);
            paint.setColor(0x66FFFFFF);
            c.drawCircle(rknobX, rknobY, knobRadius, paint);
        }

        // C-buttons / analog-camera mode toggle.
        paint.setStyle(Paint.Style.FILL);
        paint.setColor(analogMode ? 0xCC66FF99 : 0x44FFFFFF);
        c.drawCircle(modePos[0], modePos[1], modeRadius, paint);
        paint.setStyle(Paint.Style.STROKE);
        paint.setColor(0xAA000000);
        c.drawCircle(modePos[0], modePos[1], modeRadius, paint);
        paint.setStyle(Paint.Style.FILL);
        paint.setColor(Color.BLACK);
        paint.setTextSize(38f);
        c.drawText(analogMode ? "ANA" : "C", modePos[0], modePos[1] + 13f, paint);

        // Camera buttons (X = free camera toggle, Y = recentre). X turns solid
        // green while the free camera is active, so the mode is always visible.
        paint.setTextSize(48f);
        boolean freeCam = sm64portActivity.nativeGetCamMode() != 0;
        for (int i = 0; i < camIds.length; i++) {
            boolean down = (buttons & camIds[i]) != 0;
            boolean on = (i == 0) && freeCam;
            paint.setStyle(Paint.Style.FILL);
            paint.setColor(on ? 0xEE33DD55 : (down ? 0xCC66FF99 : 0x44FFFFFF));
            c.drawCircle(camPos[i][0], camPos[i][1], camRadius, paint);
            paint.setStyle(Paint.Style.STROKE);
            paint.setColor(0xAA000000);
            c.drawCircle(camPos[i][0], camPos[i][1], camRadius, paint);
            paint.setStyle(Paint.Style.FILL);
            paint.setColor(Color.BLACK);
            c.drawText(camLabels[i], camPos[i][0], camPos[i][1] + 17f, paint);
        }

        // Portal gun: two colour buttons (the reticle is drawn in-game).
        if (portalGunHeld() && !grappleEquipped) {
            paint.setTextSize(38f);
            for (int i = 0; i < 2; i++) {
                boolean on = portalPressed == i;
                paint.setStyle(Paint.Style.FILL);
                if (i == 0) {
                    paint.setColor(on ? 0xFF3366FF : 0x993060FF);
                } else {
                    paint.setColor(on ? 0xFFFF9933 : 0x99FF8020);
                }
                c.drawCircle(portalPos[i][0], portalPos[i][1], portalRadius, paint);
                paint.setStyle(Paint.Style.STROKE);
                paint.setColor(0xCCFFFFFF);
                c.drawCircle(portalPos[i][0], portalPos[i][1], portalRadius, paint);
                paint.setStyle(Paint.Style.FILL);
                paint.setColor(Color.WHITE);
                c.drawText(i == 0 ? "P1" : "P2",
                        portalPos[i][0] - 22f, portalPos[i][1] + 14f, paint);
            }

        }

        // Tool swap: always visible.  Green when the grapple is in hand, blue
        // when the portal gun is; a tap toggles and the HUD note confirms.
        {
            paint.setStyle(Paint.Style.FILL);
            paint.setColor(grappleEquipped ? 0xCC33CC66 : 0xCC3366FF);
            c.drawCircle(toolPos[0], toolPos[1], toolRadius, paint);
            paint.setStyle(Paint.Style.STROKE);
            paint.setColor(0xCCFFFFFF);
            paint.setStrokeWidth(4f);
            c.drawCircle(toolPos[0], toolPos[1], toolRadius, paint);
            paint.setStrokeWidth(6f);
            paint.setStyle(Paint.Style.FILL);
            paint.setColor(Color.WHITE);
            paint.setTextSize(30f);
            c.drawText(grappleEquipped ? "HOOK" : "GUN",
                    toolPos[0], toolPos[1] + 11f, paint);
        }

        // Jetpack fuel gauge: bottom-left, outside the opacity layer.
        {
            int mv = 0;
            try {
                mv = sm64portActivity.nativeGetMovekitState();
            } catch (Throwable ignored) {
            }
            if ((mv & 4) != 0) {
                int fuel = sm64portActivity.nativeGetJetpackFuel();
                float x0 = 40f, y0 = getHeight() * 0.42f, w = 26f, hgt = 260f;
                paint.setStyle(Paint.Style.STROKE);
                paint.setStrokeWidth(3f);
                paint.setColor(0xCCFFFFFF);
                c.drawRect(x0, y0, x0 + w, y0 + hgt, paint);
                paint.setStyle(Paint.Style.FILL);
                float fh = hgt * Math.max(0, Math.min(100, fuel)) / 100f;
                paint.setColor(fuel > 25 ? 0xCC66FF99 : 0xCCFF6666);
                c.drawRect(x0 + 3, y0 + hgt - fh + 3, x0 + w - 3, y0 + hgt - 3, paint);
            }
        }

        c.restoreToCount(layer);

        // The aiming reticle is drawn by the game UI pass (N64 display list)
        // so it gets per-eye stereo depth from the aim-surface distance.

        if (sm64portActivity.nativeGetCamMode2() == 5) {
            float x0 = dp(16), y0 = dp(16), w = dp(168), h2 = dp(46);
            paint.setStyle(Paint.Style.FILL);
            paint.setColor(freecamMove ? 0xDD33CC66 : 0xAA222222);
            c.drawRoundRect(x0, y0, x0 + w, y0 + h2, dp(10), dp(10), paint);
            paint.setColor(Color.WHITE);
            paint.setTextSize(dp(16));
            c.drawText(freecamMove ? "FREE MOVE: ON" : "FREE MOVE: OFF",
                    x0 + dp(10), y0 + dp(29), paint);
        }

        if (fpvActive()) {
            String hint = "SELECT / BACK exits FPV";
            paint.setStyle(Paint.Style.FILL);
            paint.setTextSize(28f);
            float tw = paint.measureText(hint);
            float hx = getWidth() * 0.5f;
            float hy = getHeight() - 44f;
            paint.setColor(0x8C000000);
            c.drawRoundRect(hx - tw * 0.5f - 18f, hy - 32f,
                    hx + tw * 0.5f + 18f, hy + 10f, 12f, 12f, paint);
            paint.setColor(0xFFFFFFFF);
            c.drawText(hint, hx - tw * 0.5f, hy, paint);
        }
    }

    private boolean overControl(float x, float y) {
        if (hitButton(x, y) >= 0 || hitCButton(x, y) >= 0 || hitCamButton(x, y) >= 0) {
            return true;
        }
        if (hitModeToggle(x, y)) {
            return true;
        }
        return portalGunHeld() && hitPortalButton(x, y) >= 0;
    }

    private boolean startCamGesture(MotionEvent e) {
        if (!camGesturesEnabled || e.getPointerCount() != 2) {
            return false;
        }
        float x0 = e.getX(0), y0 = e.getY(0);
        float x1 = e.getX(1), y1 = e.getY(1);
        float span = (float) Math.hypot(x1 - x0, y1 - y0);
        /* Both thumbsticks in use (left thumb + right thumb): never a gesture. */
        if (stickActive && rstickActive) {
            return false;
        }
        /* A thumb that is actually pushing a stick keeps driving it. */
        if (stickDeflected()) {
            return false;
        }
        /* Buttons / mode toggle / portal buttons keep their touches. */
        if (overControl(x0, y0) || overControl(x1, y1)) {
            return false;
        }
        /* Separation gate: a pinch starts with the fingers close to each
         * other.  Two thumbs on the left/right clusters start far apart, so
         * they can never reach this code as a gesture. */
        if (span > getWidth() * START_SPAN_FRACTION) {
            return false;
        }
        if (span < dp(GESTURE_MIN_SPAN_DP)) {
            return false;
        }
        sm64portActivity a = sm64portActivity.instance;
        camGesture = true;
        camGestureMode = GESTURE_IDLE;
        camPtr1 = e.getPointerId(0);
        camPtr2 = e.getPointerId(1);
        gSpan0 = span;
        gLastSpan = span;
        gCx0 = (x0 + x1) * 0.5f;
        gCy0 = (y0 + y1) * 0.5f;
        gLastCy = gCy0;
        gFov0 = a != null ? a.getCamValue(20) : 100;
        gDist0 = a != null ? a.getCamValue(46) : 700;
        gHeight0 = a != null ? a.getCamValue(47) : 110;
        // A second finger means the first one is no longer driving a stick.
        stickActive = false;
        stickPointer = -1;
        rstickActive = false;
        rstickPointer = -1;
        pushInput();
        invalidate();
        return true;
    }

    /** True when an active thumbstick is pushed away from its base. */
    private boolean stickDeflected() {
        if (stickActive
                && Math.hypot(knobX - stickX, knobY - stickY) > dp(20f)) {
            return true;
        }
        return rstickActive
                && Math.hypot(rknobX - rstickX, rknobY - rstickY) > dp(20f);
    }

    private float dp(float v) {
        return v * getResources().getDisplayMetrics().density;
    }

    private boolean updateCamGesture(MotionEvent e) {
        int i1 = e.findPointerIndex(camPtr1);
        int i2 = e.findPointerIndex(camPtr2);
        if (i1 < 0 || i2 < 0) {
            return false;
        }
        float x0 = e.getX(i1), y0 = e.getY(i1);
        float x1 = e.getX(i2), y1 = e.getY(i2);
        float span = (float) Math.hypot(x1 - x0, y1 - y0);
        if (span < 1f) span = 1f;
        float cx = (x0 + x1) * 0.5f;
        float cy = (y0 + y1) * 0.5f;
        float spanRatio = (span - gSpan0) / gSpan0;
        float shiftX = cx - gCx0, shiftY = cy - gCy0;
        float shift = (float) Math.hypot(shiftX, shiftY);

        // Classify once, then stay in that mode for the rest of the gesture.
        if (camGestureMode == GESTURE_IDLE) {
            boolean pinching = Math.abs(spanRatio) > PINCH_SPAN_CHANGE
                    && shift <= dp(GESTURE_SHIFT_DP);
            boolean swiping = shift > dp(GESTURE_SHIFT_DP)
                    && Math.abs(spanRatio) < SWIPE_SPAN_TOLERANCE;
            if (pinching) {
                camGestureMode = GESTURE_PINCH;
            } else if (swiping) {
                camGestureMode = GESTURE_SWIPE;
                /* Anchor the drag where it was recognised so the camera does
                 * not snap when the swipe threshold is crossed. */
                gCx0 = cx;
                gCy0 = cy;
                sm64portActivity a = sm64portActivity.instance;
                if (a != null) {
                    gDist0 = a.getCamValue(46);
                    gHeight0 = a.getCamValue(47);
                }
            } else {
                // Ambiguous (or not moved enough yet): ignore, no jumps.
                return true;
            }
        }

        sm64portActivity a = sm64portActivity.instance;
        if (a != null && a.getCamMode2() == 5) {
            /* Freecam gestures: pinch = distance from Mario's last position,
             * drag X = FOV, drag Y = camera height. */
            if (camGestureMode == GESTURE_PINCH) {
                sm64portActivity.nativeFreecamNudge((span - gLastSpan) * 0.6f);
            } else if (camGestureMode == GESTURE_SWIPE) {
                int fov = Math.round(gFov0 + (cx - gCx0) * 0.25f);
                fov = Math.max(1, Math.min(400, fov));
                a.setCamValue(20, fov);
                sm64portActivity.nativeFreecamY((gLastCy - cy) * 0.5f);
            }
            gLastSpan = span;
            gLastCy = cy;
            return true;
        }
        if (camGestureMode == GESTURE_PINCH) {
            int fov = Math.round(gFov0 * gSpan0 / span);
            fov = Math.max(1, Math.min(400, fov));
            if (a != null) {
                a.setCamValue(20, fov);
            }
        } else if (camGestureMode == GESTURE_SWIPE) {
            int dist = gDist0 - Math.round((cy - gCy0) * 1.5f);
            dist = Math.max(1, Math.min(1500, dist));
            int height = gHeight0 + Math.round((cx - gCx0) * 0.5f);
            height = Math.max(0, Math.min(400, height));
            if (a != null) {
                a.setCamValue(46, dist);
                a.setCamValue(47, height);
            }
        }
        return true;
    }

    private boolean fpvActive() {
        try {
            return sm64portActivity.nativeGetCamMode() == 4;
        } catch (Throwable t) {
            return false;
        }
    }
}

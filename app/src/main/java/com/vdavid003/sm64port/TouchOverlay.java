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

    private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);

    private float stickX, stickY;      // base position
    private float knobX, knobY;        // current knob offset position
    private boolean stickActive;
    private int stickPointer = -1;

    private int buttons;
    private final int[] btnIds = {BTN_A, BTN_B, BTN_Z, BTN_R, BTN_START, BTN_L};
    private final String[] btnLabels = {"A", "B", "Z", "R", "S", "L"};
    private final float[][] btnPos = new float[6][2];
    private int buttonPointer = -1;

    private final float radius = 90f;      // button radius
    private final float stickRadius = 190f;
    private final float knobRadius = 70f;

    private final int[] pressedPointer = {-1, -1, -1, -1, -1, -1};

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

    // Portal gun buttons (P1 = blue, P2 = orange) and the aiming reticle.
    private final float[][] portalPos = new float[2][2];
    private final float portalRadius = 46f;
    private int portalPressed = -1;
    private int portalPointer = -1;

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

    @Override
    public boolean onTouchEvent(MotionEvent e) {
        if (btnPos[0][0] == 0) layoutButtons();
        float x = e.getX(), y = e.getY();
        int action = e.getActionMasked();

        switch (action) {
            case MotionEvent.ACTION_DOWN:
            case MotionEvent.ACTION_POINTER_DOWN: {
                int idx = e.getActionIndex();
                int id = e.getPointerId(idx);
                float px = e.getX(idx), py = e.getY(idx);
                int b = hitButton(px, py);
                int cb = b < 0 ? hitCButton(px, py) : -1;
                int cam = (b < 0 && cb < 0) ? hitCamButton(px, py) : -1;
                int pb = (b < 0 && cb < 0 && cam < 0 && portalGunHeld())
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
                } else if (pb >= 0) {
                    sm64portActivity.nativeShootPortal(pb);
                    portalPressed = pb;
                    portalPointer = id;
                    invalidate();
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
        sm64portActivity.nativeOverlayStick2(rx, ry);
        // Buttons are always merged by the native side. The stick only takes
        // over while the overlay stick is actually held, so a paired gamepad
        // keeps its left stick after tapping the overlay.
        sm64portActivity.nativeOverlayInput(stickActive ? 1 : 0, sx, sy, buttons);
    }

    @Override
    protected void onDraw(Canvas c) {
        super.onDraw(c);
        if (btnPos[0][0] == 0) layoutButtons();

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

        // Portal gun: two colour buttons plus a screen-centre aiming reticle.
        if (portalGunHeld()) {
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

        c.restoreToCount(layer);

        // Aiming reticle stays at full opacity even when the controls are faded.
        if (portalGunHeld()) {
            float cx = getWidth() * 0.5f, cy = getHeight() * 0.5f;
            paint.setStyle(Paint.Style.STROKE);
            paint.setStrokeWidth(3f);
            paint.setColor(0xCCFFFFFF);
            c.drawCircle(cx, cy, 24f, paint);
            c.drawLine(cx - 42, cy, cx - 16, cy, paint);
            c.drawLine(cx + 16, cy, cx + 42, cy, paint);
            c.drawLine(cx, cy - 42, cx, cy - 16, paint);
            c.drawLine(cx, cy + 16, cx, cy + 42, paint);
            paint.setStrokeWidth(6f);
        }
    }
}

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
    public static final int BTN_START = 0x1000;

    private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);

    private float stickX, stickY;      // base position
    private float knobX, knobY;        // current knob offset position
    private boolean stickActive;
    private int stickPointer = -1;

    private int buttons;
    private final int[] btnIds = {BTN_A, BTN_B, BTN_Z, BTN_R, BTN_START};
    private final String[] btnLabels = {"A", "B", "Z", "R", "S"};
    private final float[][] btnPos = new float[5][2];
    private int buttonPointer = -1;

    private final float radius = 90f;      // button radius
    private final float stickRadius = 190f;
    private final float knobRadius = 70f;

    private final int[] pressedPointer = {-1, -1, -1, -1, -1};

    public TouchOverlay(Context c) {
        super(c);
        setFocusable(false);
        setClickable(false);
        // The view must receive touches over the whole panel.
        setWillNotDraw(false);
    }

    private void layoutButtons() {
        float w = getWidth(), h = getHeight();
        btnPos[0][0] = w * 0.90f; btnPos[0][1] = h * 0.70f;   // A
        btnPos[1][0] = w * 0.81f; btnPos[1][1] = h * 0.77f;   // B
        btnPos[2][0] = w * 0.94f; btnPos[2][1] = h * 0.58f;   // Z
        btnPos[3][0] = w * 0.79f; btnPos[3][1] = h * 0.60f;   // R
        btnPos[4][0] = w * 0.50f; btnPos[4][1] = h * 0.93f;   // START (bottom centre)
    }

    private int hitButton(float x, float y) {
        for (int i = 0; i < btnPos.length; i++) {
            float dx = x - btnPos[i][0], dy = y - btnPos[i][1];
            if (dx * dx + dy * dy <= (radius * 1.25f) * (radius * 1.25f)) return i;
        }
        return -1;
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
                if (b >= 0) {
                    buttons |= btnIds[b];
                    pressedPointer[b] = id;
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
                for (int b = 0; b < btnIds.length; b++) {
                    if (pressedPointer[b] == id) {
                        buttons &= ~btnIds[b];
                        pressedPointer[b] = -1;
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
        // The overlay is the sole input source, so always stay active -
        // otherwise button presses were dropped when the stick wasn't held.
        sm64portActivity.nativeOverlayInput(1, sx, sy, buttons);
    }

    @Override
    protected void onDraw(Canvas c) {
        super.onDraw(c);
        if (btnPos[0][0] == 0) layoutButtons();

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
    }
}

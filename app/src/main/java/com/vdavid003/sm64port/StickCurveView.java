package com.vdavid003.sm64port;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Path;
import android.view.MotionEvent;
import android.view.View;

/**
 * Draggable cam-stick response curve (CAMERA panel).
 *
 * Five points, the two end points pinned to the corners and the three interior
 * ones draggable in both axes.  x is clamped between the neighbours (and y
 * between their values) so the curve is always monotonic and can never invert.
 * The drawn line is the same Fritsch-Carlson monotone cubic used by the camera
 * and the stick heatmap, so what you see is what the stick does.
 *
 * The dashed 45-degree line is a linear response, for comparison.
 */
public class StickCurveView extends View {
    private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Path path = new Path();

    private int dragIndex = -1;
    private int cacheVer = -1;
    private int[] xs = new int[101];
    private float[] ys = new float[101];

    public StickCurveView(Context c) {
        super(c);
        setWillNotDraw(false);
        setBackgroundColor(0xFF101418);
    }

    private float padL() { return dp(30); }
    private float padR() { return dp(10); }
    private float padT() { return dp(10); }
    private float padB() { return dp(22); }

    private float px(float x) {
        return padL() + x * (getWidth() - padL() - padR());
    }

    private float py(float y) {
        return getHeight() - padB() - y * (getHeight() - padT() - padB());
    }

    private float dp(float v) {
        return v * getResources().getDisplayMetrics().density;
    }

    private void rebuild() {
        if (cacheVer == StickCurve.version) {
            return;
        }
        for (int i = 0; i < xs.length; i++) {
            float u = i / (float) (xs.length - 1);
            xs[i] = i;
            ys[i] = StickCurve.effective(u);
        }
        cacheVer = StickCurve.version;
    }

    @Override
    protected void onDraw(Canvas c) {
        super.onDraw(c);
        rebuild();

        float x0 = padL(), x1 = getWidth() - padR();
        float y0 = getHeight() - padB(), y1 = padT();

        // Frame + quarter grid.
        paint.setStyle(Paint.Style.STROKE);
        paint.setStrokeWidth(dp(1f));
        paint.setColor(0x33556677);
        c.drawRect(x0, y1, x1, y0, paint);
        for (int i = 1; i < 4; i++) {
            float gx = x0 + (x1 - x0) * i / 4f;
            float gy = y0 - (y0 - y1) * i / 4f;
            c.drawLine(gx, y0, gx, y1, paint);
            c.drawLine(x0, gy, x1, gy, paint);
        }

        // Linear reference.
        paint.setColor(0x4477AACC);
        paint.setPathEffect(new android.graphics.DashPathEffect(
                new float[] {dp(6f), dp(6f)}, 0f));
        c.drawLine(x0, y0, x1, y1, paint);
        paint.setPathEffect(null);

        // Deadzone band (effective() is 0 while raw < dz).
        float dz = StickCurve.deadzonePct / 100f;
        if (dz > 0.001f) {
            paint.setStyle(Paint.Style.FILL);
            paint.setColor(0x33FF5555);
            c.drawRect(x0, y1, x0 + (x1 - x0) * dz, y0, paint);
            paint.setStyle(Paint.Style.STROKE);
            paint.setColor(0xAAFF7777);
            c.drawLine(x0 + (x1 - x0) * dz, y0, x0 + (x1 - x0) * dz, y1, paint);
        }

        // The response itself.
        path.reset();
        path.moveTo(px(0f), py(ys[0]));
        for (int i = 1; i < xs.length; i++) {
            path.lineTo(px(xs[i] / (float) (xs.length - 1)), py(ys[i]));
        }
        paint.setStyle(Paint.Style.STROKE);
        paint.setStrokeWidth(dp(2.5f));
        paint.setColor(0xFFFFCC44);
        c.drawPath(path, paint);

        // Control points: end points pinned, interior ones draggable.
        for (int i = 0; i < StickCurve.N; i++) {
            float cx = px(StickCurve.PX[i]);
            float cy = py(StickCurve.PY[i]);
            boolean fixed = (i == 0 || i == StickCurve.N - 1);
            paint.setStyle(Paint.Style.FILL);
            paint.setColor(fixed ? 0xFF445566 : (i == dragIndex ? 0xFFFF8844 : 0xFF66FF99));
            if (fixed) {
                c.drawRect(cx - dp(5f), cy - dp(5f), cx + dp(5f), cy + dp(5f), paint);
            } else {
                c.drawCircle(cx, cy, dp(9f), paint);
            }
            paint.setStyle(Paint.Style.STROKE);
            paint.setStrokeWidth(dp(1.5f));
            paint.setColor(0xFF101418);
            if (fixed) {
                c.drawRect(cx - dp(5f), cy - dp(5f), cx + dp(5f), cy + dp(5f), paint);
            } else {
                c.drawCircle(cx, cy, dp(9f), paint);
            }
        }

        // Labels.
        paint.setStyle(Paint.Style.FILL);
        paint.setColor(0xFF99AABB);
        paint.setTextSize(dp(11f));
        paint.setTextAlign(Paint.Align.LEFT);
        c.drawText("100%", x0 - dp(27f), y1 + dp(10f), paint);
        c.drawText("0", x0 - dp(16f), y0 + dp(4f), paint);
        paint.setTextAlign(Paint.Align.RIGHT);
        c.drawText("raw stick", x1, y0 + dp(14f), paint);
        paint.setTextAlign(Paint.Align.LEFT);
        paint.setColor(0xFFCCDDEE);
        paint.setTextSize(dp(12f));
        c.drawText("CAM STICK CURVE  \u2192  out " + Math.round(StickCurve.effective(1f) * 100f)
                        + "%  dz " + StickCurve.deadzonePct + "%",
                x0, padT() + dp(12f), paint);
    }

    private int hitPoint(float x, float y) {
        int best = -1;
        float bestD = dp(34f) * dp(34f);
        for (int i = 1; i < StickCurve.N - 1; i++) {
            float dx = x - px(StickCurve.PX[i]);
            float dy = y - py(StickCurve.PY[i]);
            float d = dx * dx + dy * dy;
            if (d < bestD) {
                bestD = d;
                best = i;
            }
        }
        return best;
    }

    @Override
    public boolean onTouchEvent(MotionEvent e) {
        float x = e.getX(), y = e.getY();
        switch (e.getActionMasked()) {
            case MotionEvent.ACTION_DOWN: {
                dragIndex = hitPoint(x, y);
                if (dragIndex >= 0) {
                    getParent().requestDisallowInterceptTouchEvent(true);
                    invalidate();
                }
                return true;
            }
            case MotionEvent.ACTION_MOVE: {
                if (dragIndex < 0) return true;
                float nx = (x - padL()) / (getWidth() - padL() - padR());
                float ny = (getHeight() - padB() - y) / (getHeight() - padT() - padB());
                float loX = StickCurve.PX[dragIndex - 1] + 0.02f;
                float hiX = StickCurve.PX[dragIndex + 1] - 0.02f;
                nx = Math.max(loX, Math.min(hiX, nx));
                // Clamp y between the neighbours so y stays monotonic too.
                float loY = StickCurve.PY[dragIndex - 1];
                float hiY = StickCurve.PY[dragIndex + 1];
                ny = Math.max(loY, Math.min(hiY, ny));
                StickCurve.setPoint(dragIndex, nx, ny);
                invalidate();
                return true;
            }
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_CANCEL: {
                if (dragIndex >= 0) {
                    dragIndex = -1;
                    invalidate();
                }
                return true;
            }
        }
        return true;
    }
}

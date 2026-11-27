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

    private final int[] pressedPointer = {-1, -1, -1, -1, -1, -1, -1};

    // C buttons: smaller cluster (up/down/left/right)
    private final int[] cIds = {BTN_CUP, BTN_CDOWN, BTN_CLEFT, BTN_CRIGHT};
    private final String[] cLabels = {"^", "v", "<", ">"};
    private final float[][] cPos = new float[4][2];
    private final int[] cPressed = {-1, -1, -1, -1};

    // Camera buttons: X toggles free camera, Y recentres.
    private final int[] camIds = {BTN_CAM, BTN_RECENTER};
    private final String[] camLabels = {"X", "Y"};
    private final float[][] camPos = new float[2][2];
    private final int[] camPressed = {-1, -1};

    // Portal gun buttons (P1 = blue, P2 = orange).  The reticle lives in the
    // game UI pass so it can have its own stereo depth.
    private final float[][] portalPos = new float[2][2];
    private int portalPressed = -1;
    private int portalPointer = -1;

    // One-press swap between the portal gun and the grappling hook.
    private final float[] toolPos = new float[2];
    private boolean toolDown;
    private int toolPointer = -1;

    // Hold-to-thrust jetpack button.
    private final float[] jetPos = new float[2];
    private boolean jetDown;
    private int jetPointer = -1;

    /* --------------------------------------------------------------------
     * Re-arrangeable layout model.
     *
     * Every movable control has a normalised centre (0..1 of the panel
     * width/height) and a size multiplier. Radii additionally scale with
     * min(width,height)/1600, so the same profile looks right on a panel
     * with a different resolution. Profiles live in TouchLayoutStore.
     * ------------------------------------------------------------------ */
    public static final int E_STICK = 0, E_A = 1, E_B = 2, E_Z = 3, E_R = 4,
            E_S = 5, E_L = 6, E_SEL = 7, E_CUP = 8, E_CDN = 9, E_CLF = 10,
            E_CRT = 11, E_CAMX = 12, E_CAMY = 13, E_P1 = 14, E_P2 = 15,
            E_TOOL = 16, E_JET = 17, E_MODE = 18, E_RSTICK = 19;
    public static final int ELEM_COUNT = 20;
    public static final String[] ELEM_KEYS = {
            "stick", "a", "b", "z", "r", "s", "l", "sel",
            "cup", "cdn", "clf", "crt", "camx", "camy", "p1", "p2",
            "tool", "jet", "mode", "rstick"};
    public static final String[] ELEM_LABELS = {
            "STICK", "A", "B", "Z", "R", "S", "L", "SEL",
            "C-UP", "C-DOWN", "C-LEFT", "C-RIGHT", "CAM-X", "CAM-Y",
            "P1", "P2", "TOOL", "JET", "MODE", "R-STICK"};

    private final float[] nx = new float[ELEM_COUNT];   // 0..1 of width
    private final float[] ny = new float[ELEM_COUNT];   // 0..1 of height
    private final float[] ns = new float[ELEM_COUNT];   // radius multiplier
    private final float[] factoryX = new float[ELEM_COUNT];
    private final float[] factoryY = new float[ELEM_COUNT];
    private final float[] factoryS = new float[ELEM_COUNT];

    private TouchLayoutStore layoutStore;
    private boolean editMode;
    private int selectedElem = -1;
    private int dragElem = -1, dragPointer = -1;
    private static final int DRAG_NONE = 0, DRAG_MOVE = 1, DRAG_RESIZE = 2;
    private int dragMode = DRAG_NONE;
    private float dragOffX, dragOffY, dragStartDist, dragStartScale;
    private float infoX, infoY;          // last normalised coords (banner)
    private boolean infoValid;
    private static final float GRID = 0.0125f;   // snap step (~32 px at 2560)
    private static final float SCALE_MIN = 0.4f, SCALE_MAX = 2.5f;

    /* Last committed layout of the active profile, captured on entering the
     * editor and refreshed by SAVE. Drags only touch nx/ny/ns, so DISCARD can
     * put the working copy back without reading the store. */
    private final float[] savedX = new float[ELEM_COUNT];
    private final float[] savedY = new float[ELEM_COUNT];
    private final float[] savedS = new float[ELEM_COUNT];
    /* SAVE / DISCARD hit boxes under the editor banner, and a one-line note
     * ("saved default") that the next touch clears. */
    private final float[] editBtnRects = new float[8];
    private String editNote;

    /* Pixel size the btnPos/cPos/... caches were built for. A resize or a drag
     * changes the model, so this is checked before draw and touch. */
    private int layoutW = -1, layoutH = -1;

    // Overall opacity of the control layer (0.1 .. 1.0).
    private float overlayAlpha = 1f;

    // Right-hand control: 4 C buttons, or an analog camera stick.
    private boolean analogMode = true;   // analog camera stick by default
    private boolean rstickActive;
    private int rstickPointer = -1;
    private float rstickX, rstickY, rknobX, rknobY;
    private final float[] modePos = new float[2];

    /* Radial heatmap inside the cam-stick base circle.  The bitmap is built
     * once per curve change and only blitted while the stick is held, so a
     * still stick costs one drawBitmap (the view is already invalidated at
     * 2 Hz by the debug readout). */
    private android.graphics.Bitmap heatBmp;
    private int heatVer = -1;
    private int heatRad = -1;

    public TouchOverlay(Context c) {
        super(c);
        setFocusable(false);
        setClickable(false);
        // The view must receive touches over the whole panel.
        setWillNotDraw(false);
        initFactoryLayout();
        layoutStore = new TouchLayoutStore(c, ELEM_KEYS, nx, ny, ns);
        layoutStore.applyProfile(layoutStore.active(), ELEM_KEYS, nx, ny, ns);
        if (btnPos[0][0] == 0 && getWidth() > 0) layoutButtons();
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

    /** Hard-coded factory layout (also the RESET target for the `default`
     *  profile). See {@link #ELEM_KEYS} for the storage order. */
    private void initFactoryLayout() {
        setDef(E_STICK, 0.16f, 0.72f);
        setDef(E_A, 0.90f, 0.70f);
        setDef(E_B, 0.81f, 0.77f);
        setDef(E_Z, 0.94f, 0.58f);
        setDef(E_R, 0.79f, 0.60f);
        setDef(E_S, 0.50f, 0.88f);
        setDef(E_L, 0.90f, 0.84f);
        setDef(E_SEL, 0.38f, 0.88f);
        // C cross around 0.68/0.70, spacing from the old 78.3 px at 2560x1600.
        float dx = 78.3f / 2560f, dy = 78.3f / 1600f;
        setDef(E_CUP, 0.68f, 0.70f - dy);
        setDef(E_CDN, 0.68f, 0.70f + dy);
        setDef(E_CLF, 0.68f - dx, 0.70f);
        setDef(E_CRT, 0.68f + dx, 0.70f);
        setDef(E_CAMX, 0.93f, 0.40f);
        setDef(E_CAMY, 0.84f, 0.32f);
        setDef(E_P1, 0.93f, 0.27f);
        setDef(E_P2, 0.84f, 0.19f);
        setDef(E_TOOL, 0.75f, 0.27f);
        setDef(E_JET, 0.72f, 0.76f);
        setDef(E_MODE, 0.68f, 0.88f);
        setDef(E_RSTICK, 0.84f, 0.74f);
        System.arraycopy(nx, 0, factoryX, 0, ELEM_COUNT);
        System.arraycopy(ny, 0, factoryY, 0, ELEM_COUNT);
        System.arraycopy(ns, 0, factoryS, 0, ELEM_COUNT);
    }

    private void setDef(int e, float x, float y) {
        nx[e] = x;
        ny[e] = y;
        ns[e] = 1f;
    }

    private void restoreFactory() {
        System.arraycopy(factoryX, 0, nx, 0, ELEM_COUNT);
        System.arraycopy(factoryY, 0, ny, 0, ELEM_COUNT);
        System.arraycopy(factoryS, 0, ns, 0, ELEM_COUNT);
    }

    /** Reference size: a 1600-px short edge keeps the old pixel radii. */
    private float uiScale() {
        int w = getWidth(), h = getHeight();
        float m = Math.min(w, h);
        return m > 0 ? m / 1600f : 1f;
    }

    private float radius(int i) {
        return 90f * uiScale() * ns[E_A + i];
    }

    private float cRadius(int i) {
        return 58f * uiScale() * ns[E_CUP + i];
    }

    private float camRadius(int i) {
        return 52f * uiScale() * ns[E_CAMX + i];
    }

    private float portalRadius(int i) {
        return 46f * uiScale() * ns[E_P1 + i];
    }

    private float toolRadius() {
        return 46f * uiScale() * ns[E_TOOL];
    }

    private float jetRadius() {
        return 46f * uiScale() * ns[E_JET];
    }

    private float modeRadius() {
        return 46f * uiScale() * ns[E_MODE];
    }

    private float stickRadiusPx(int e) {
        return 190f * uiScale() * ns[e];
    }

    private float knobRadiusPx(int e) {
        return 70f * uiScale() * ns[e];
    }

    private float elemRadiusPx(int e) {
        switch (e) {
            case E_STICK:
            case E_RSTICK:
                return stickRadiusPx(e);
            case E_CUP:
            case E_CDN:
            case E_CLF:
            case E_CRT:
                return cRadius(e - E_CUP);
            case E_CAMX:
            case E_CAMY:
                return camRadius(e - E_CAMX);
            case E_P1:
            case E_P2:
                return portalRadius(e - E_P1);
            case E_TOOL:
                return toolRadius();
            case E_JET:
                return jetRadius();
            case E_MODE:
                return modeRadius();
            default:
                return radius(e - E_A);
        }
    }

    /** Map the normalised model onto the pixel caches used by the draw/hit code. */
    private void layoutButtons() {
        float w = getWidth(), h = getHeight();
        if (w <= 0 || h <= 0) return;
        layoutW = (int) w;
        layoutH = (int) h;
        btnPos[0][0] = nx[E_A] * w;   btnPos[0][1] = ny[E_A] * h;
        btnPos[1][0] = nx[E_B] * w;   btnPos[1][1] = ny[E_B] * h;
        btnPos[2][0] = nx[E_Z] * w;   btnPos[2][1] = ny[E_Z] * h;
        btnPos[3][0] = nx[E_R] * w;   btnPos[3][1] = ny[E_R] * h;
        btnPos[4][0] = nx[E_S] * w;   btnPos[4][1] = ny[E_S] * h;
        btnPos[5][0] = nx[E_L] * w;   btnPos[5][1] = ny[E_L] * h;
        btnPos[6][0] = nx[E_SEL] * w; btnPos[6][1] = ny[E_SEL] * h;

        cPos[0][0] = nx[E_CUP] * w;   cPos[0][1] = ny[E_CUP] * h;
        cPos[1][0] = nx[E_CDN] * w;   cPos[1][1] = ny[E_CDN] * h;
        cPos[2][0] = nx[E_CLF] * w;   cPos[2][1] = ny[E_CLF] * h;
        cPos[3][0] = nx[E_CRT] * w;   cPos[3][1] = ny[E_CRT] * h;

        camPos[0][0] = nx[E_CAMX] * w; camPos[0][1] = ny[E_CAMX] * h;
        camPos[1][0] = nx[E_CAMY] * w; camPos[1][1] = ny[E_CAMY] * h;

        portalPos[0][0] = nx[E_P1] * w; portalPos[0][1] = ny[E_P1] * h;
        portalPos[1][0] = nx[E_P2] * w; portalPos[1][1] = ny[E_P2] * h;

        toolPos[0] = nx[E_TOOL] * w;   toolPos[1] = ny[E_TOOL] * h;
        jetPos[0] = nx[E_JET] * w;     jetPos[1] = ny[E_JET] * h;
        modePos[0] = nx[E_MODE] * w;   modePos[1] = ny[E_MODE] * h;
    }

    /** Rebuild the pixel caches when the panel size changed (first draw, rotation). */
    private void syncGeometry() {
        if (getWidth() != layoutW || getHeight() != layoutH) layoutButtons();
    }

    /** Rebuild the pixel caches after the model moved, so a drag/resize edits the
     *  control that is drawn and hit-tested, not just the editor's boxes. */
    private void rebindGeometry() {
        layoutButtons();
        invalidate();
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
            float r = portalRadius(i) * 1.25f;
            float dx = x - portalPos[i][0], dy = y - portalPos[i][1];
            if (dx * dx + dy * dy <= r * r) return i;
        }
        return -1;
    }

    private boolean hitJetButton(float x, float y) {
        float r = jetRadius() * 1.25f;
        float dx = x - jetPos[0], dy = y - jetPos[1];
        return dx * dx + dy * dy <= r * r;
    }

    private boolean hitToolButton(float x, float y) {
        float r = toolRadius() * 1.25f;
        float dx = x - toolPos[0], dy = y - toolPos[1];
        return dx * dx + dy * dy <= r * r;
    }

    private boolean hitFreecamToggle(float x, float y) {
        if (sm64portActivity.nativeGetCamMode2() != 5) {
            return false;
        }
        return x >= dp(16) && x <= dp(16) + dp(168)
                && y >= dp(16) && y <= dp(16) + dp(46);
    }

    private boolean hitModeToggle(float x, float y) {
        float r = modeRadius() * 1.3f;
        float dx = x - modePos[0], dy = y - modePos[1];
        return dx * dx + dy * dy <= r * r;
    }

    /** Index into camIds for a camera-button hit, else -1. */
    private int hitCamButton(float x, float y) {
        for (int i = 0; i < camIds.length; i++) {
            float r = camRadius(i) * 1.3f;
            float dx = x - camPos[i][0], dy = y - camPos[i][1];
            if (dx * dx + dy * dy <= r * r) return i;
        }
        return -1;
    }

    private int hitButton(float x, float y) {
        for (int i = 0; i < btnPos.length; i++) {
            float r = radius(i) * 1.25f;
            float dx = x - btnPos[i][0], dy = y - btnPos[i][1];
            if (dx * dx + dy * dy <= r * r) return i;
        }
        return -1;
    }

    /** Index into cIds for a C-button hit, else -1. */
    private int hitCButton(float x, float y) {
        /* In analog-camera mode the C cluster is hidden; its invisible hit
         * circles must not steal touches from the right analog stick. */
        if (analogMode) return -1;
        for (int i = 0; i < 4; i++) {
            float r = cRadius(i) * 1.3f;
            float dx = x - cPos[i][0], dy = y - cPos[i][1];
            if (dx * dx + dy * dy <= r * r) return i;
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
        if (controlsHidden && !editMode) {
            controlsHidden = false;
            invalidate();
        }
        syncGeometry();
        int action = e.getActionMasked();

        if (editMode) {
            return handleEditTouch(e);
        }

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
                boolean jb = (b < 0 && cb < 0 && cam < 0 && !tb) && hitJetButton(px, py);
                int pb = (b < 0 && cb < 0 && cam < 0 && !tb && !jb && portalGunHeld())
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
                } else if (jb) {
                    jetDown = true;
                    jetPointer = id;
                    sm64portActivity.nativeJetpack(1);
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
                    float[] home = stickHome(E_RSTICK, px, py);
                    rstickX = home[0];
                    rstickY = home[1];
                    rknobX = px;
                    rknobY = py;
                    // Keep the knob inside the ring when the base snapped home.
                    clampKnob(E_RSTICK);
                } else if (px < getWidth() * 0.45f) {
                    stickActive = true;
                    stickPointer = id;
                    float[] home = stickHome(E_STICK, px, py);
                    stickX = home[0];
                    stickY = home[1];
                    knobX = px;
                    knobY = py;
                    clampKnob(E_STICK);
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
                        float sr = stickRadiusPx(E_STICK);
                        float dx = px - stickX, dy = py - stickY;
                        float len = (float) Math.hypot(dx, dy);
                        if (len > sr) {
                            dx = dx / len * sr;
                            dy = dy / len * sr;
                        }
                        knobX = stickX + dx;
                        knobY = stickY + dy;
                        pushInput();
                        invalidate();
                    } else if (id == rstickPointer) {
                        float px = e.getX(i), py = e.getY(i);
                        float sr = stickRadiusPx(E_RSTICK);
                        float dx = px - rstickX, dy = py - rstickY;
                        float len = (float) Math.hypot(dx, dy);
                        if (len > sr) {
                            dx = dx / len * sr;
                            dy = dy / len * sr;
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
                if (id == jetPointer) {
                    jetDown = false;
                    jetPointer = -1;
                    sm64portActivity.nativeJetpack(0);
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

    /** Current cam-stick vector in N64 units (positive x = right, y = up). */
    private int stickVecX() {
        return (int) ((rknobX - rstickX) / stickRadiusPx(E_RSTICK) * 127f);
    }

    private int stickVecY() {
        return (int) (-(rknobY - rstickY) / stickRadiusPx(E_RSTICK) * 127f);
    }

    /**
     * Radial heatmap of the cam-stick response: the colour at radius r is the
     * effective output rate the curve gives for a raw deflection of r, the
     * deadzone is a dark disc ringed in white, and the response curve is
     * overlaid as a small graph so the shape is readable at a glance.
     */
    private void drawStickHeatmap(Canvas c, float cx, float cy, float r, int vx, int vy) {
        if (!StickCurve.heatmap) {
            return;
        }
        int ri = (int) r;
        if (heatBmp == null || heatVer != StickCurve.version || heatRad != ri) {
            heatBmp = buildHeatBitmap(ri);
            heatVer = StickCurve.version;
            heatRad = ri;
        }
        paint.setStyle(Paint.Style.FILL);
        paint.setFilterBitmap(true);
        c.drawBitmap(heatBmp, cx - ri, cy - ri, paint);
        paint.setFilterBitmap(false);

        // Deadzone ring.
        float dz = StickCurve.deadzonePct / 100f;
        if (dz > 0.01f) {
            paint.setStyle(Paint.Style.STROKE);
            paint.setStrokeWidth(3f);
            paint.setColor(0xCCFFFFFF);
            paint.setPathEffect(new android.graphics.DashPathEffect(
                    new float[] {10f, 8f}, 0f));
            c.drawCircle(cx, cy, r * dz, paint);
            paint.setPathEffect(null);
            paint.setTextAlign(Paint.Align.CENTER);
            paint.setTextSize(26f);
            paint.setStyle(Paint.Style.FILL);
            paint.setColor(0xCCFFFFFF);
            c.drawText("DZ", cx, cy - r * dz - 6f, paint);
        }

        // Response curve overlay: x = raw deflection, y = effective output,
        // drawn in the lower-left half of the circle so it stays inside it.
        {
            float gx = cx - r * 0.82f;
            float gy = cy + r * 0.82f;
            float gs = r * 0.80f;
            paint.setStyle(Paint.Style.STROKE);
            paint.setStrokeWidth(3f);
            // Linear reference.
            paint.setColor(0x6688CCFF);
            paint.setPathEffect(new android.graphics.DashPathEffect(
                    new float[] {8f, 8f}, 0f));
            c.drawLine(gx, gy, gx + gs, gy - gs, paint);
            paint.setPathEffect(null);
            // Curve.
            paint.setColor(0xFFFFDD44);
            android.graphics.Path p = new android.graphics.Path();
            int steps = 48;
            for (int i = 0; i <= steps; i++) {
                float u = i / (float) steps;
                float e = StickCurve.effective(u);
                float x = gx + gs * u;
                float y = gy - gs * e;
                if (i == 0) p.moveTo(x, y);
                else p.lineTo(x, y);
            }
            c.drawPath(p, paint);
        }

        // Live stick vector (base -> knob, in effective units) and its numbers.
        {
            float len = (float) Math.hypot(vx, vy);
            float eff = StickCurve.effective(len / 127f);
            float k = len > 0.5f ? eff * 127f / len : 0f;
            paint.setStyle(Paint.Style.STROKE);
            paint.setStrokeWidth(5f);
            paint.setColor(0xFF00FFCC);
            c.drawLine(cx, cy, cx + vx * k * r / 127f, cy - vy * k * r / 127f, paint);
            paint.setStyle(Paint.Style.FILL);
            paint.setTextAlign(Paint.Align.CENTER);
            paint.setTextSize(24f);
            paint.setColor(0xFF00FFCC);
            c.drawText(Math.round(eff * 100f) + "%", cx, cy - r - 8f, paint);
        }
    }

    private android.graphics.Bitmap buildHeatBitmap(int r) {
        int d = Math.max(2, r * 2);
        int[] px = new int[d * d];
        float rr = r;
        for (int y = 0; y < d; y++) {
            float dy = (y - r + 0.5f) / rr;
            for (int x = 0; x < d; x++) {
                float dx = (x - r + 0.5f) / rr;
                float u = (float) Math.sqrt(dx * dx + dy * dy);
                if (u > 1f) {
                    px[y * d + x] = 0;
                    continue;
                }
                if (u <= StickCurve.deadzonePct / 100f) {
                    px[y * d + x] = 0xE0101820;   // dead disc
                } else {
                    px[y * d + x] = heatColor(StickCurve.effective(u));
                }
            }
        }
        return android.graphics.Bitmap.createBitmap(px, d, d,
                android.graphics.Bitmap.Config.ARGB_8888);
    }

    /** Blue (slow) -> cyan -> green -> yellow -> red (full rate). */
    private int heatColor(float e) {
        float r, g, b;
        if (e < 0.25f) {
            float t = e / 0.25f;
            r = 20f + 20f * t;
            g = 40f + 150f * t;
            b = 110f + 90f * t;
        } else if (e < 0.5f) {
            float t = (e - 0.25f) / 0.25f;
            r = 40f + 20f * t;
            g = 190f + 20f * t;
            b = 200f - 120f * t;
        } else if (e < 0.75f) {
            float t = (e - 0.5f) / 0.25f;
            r = 60f + 170f * t;
            g = 210f - 0f * t;
            b = 80f - 30f * t;
        } else {
            float t = (e - 0.75f) / 0.25f;
            r = 230f + 20f * t;
            g = 210f - 150f * t;
            b = 50f - 20f * t;
        }
        int a = (int) (110f + 120f * e);   // denser where the output is higher
        return (a << 24) | ((int) r << 16) | ((int) g << 8) | (int) b;
    }

    private void pushInput() {
        if (editMode) return;
        int sx = 0, sy = 0;
        if (stickActive) {
            float sr = Math.max(1f, stickRadiusPx(E_STICK));
            sx = (int) ((knobX - stickX) / sr * 127f);
            sy = (int) (-(knobY - stickY) / sr * 127f);
        }
        int rx = 0, ry = 0;
        if (rstickActive) {
            float sr = Math.max(1f, stickRadiusPx(E_RSTICK));
            rx = (int) ((rknobX - rstickX) / sr * 127f);
            ry = (int) (-(rknobY - rstickY) / sr * 127f);
        }
        sm64portActivity.nativeOverlayStick2(rx, ry, rstickActive ? 1 : 0);
        // Buttons are always merged by the native side. The stick only takes
        // over while the overlay stick is actually held, so a paired gamepad
        // keeps its left stick after tapping the overlay.
        sm64portActivity.nativeOverlayInput(stickActive ? 1 : 0, sx, sy, buttons);
    }

    /** Snapped base for a thumbstick: the saved home when the thumb lands near
     *  it, otherwise the touch point (the floating-thumbstick behaviour). */
    private float[] stickHome(int e, float px, float py) {
        float hx = nx[e] * getWidth(), hy = ny[e] * getHeight();
        float r = elemRadiusPx(e) * 2.0f;
        if ((float) Math.hypot(px - hx, py - hy) <= r) {
            return new float[]{hx, hy};
        }
        return new float[]{px, py};
    }

    private void clampKnob(int e) {
        float sr = Math.max(1f, stickRadiusPx(e));
        float dx, dy;
        if (e == E_STICK) {
            dx = knobX - stickX;
            dy = knobY - stickY;
        } else {
            dx = rknobX - rstickX;
            dy = rknobY - rstickY;
        }
        float len = (float) Math.hypot(dx, dy);
        if (len > sr) {
            dx = dx / len * sr;
            dy = dy / len * sr;
        }
        if (e == E_STICK) {
            knobX = stickX + dx;
            knobY = stickY + dy;
        } else {
            rknobX = rstickX + dx;
            rknobY = rstickY + dy;
        }
    }

    @Override
    protected void onDraw(Canvas c) {
        super.onDraw(c);
        if (controlsHidden && !editMode) {
            return;
        }
        syncGeometry();

        // 0 = portal gun, 1 = grapple.  Read once so the portal buttons and
        // the tool button agree within a frame.
        int toolState = 0;
        try {
            toolState = sm64portActivity.nativeGetTool();
        } catch (Throwable ignored) {
        }
        boolean grappleEquipped = toolState == 1;

        // One layer for the whole control set so "Ctl opacity" can fade it.
        // The editor always draws at full opacity so handles stay visible.
        int layer = c.saveLayerAlpha(0f, 0f, getWidth(), getHeight(),
                editMode ? 255 : (int) (overlayAlpha * 255f));

        paint.setStyle(Paint.Style.STROKE);
        paint.setStrokeWidth(6f);
        paint.setColor(0x88FFFFFF);

        if (editMode) {
            /* Ghost of both thumbstick homes, so they can be positioned even
             * though the live sticks only appear under a thumb. */
            int[] homes = {E_STICK, E_RSTICK};
            for (int e : homes) {
                float hx = nx[e] * getWidth(), hy = ny[e] * getHeight();
                float r = stickRadiusPx(e);
                paint.setStyle(Paint.Style.STROKE);
                paint.setStrokeWidth(3f);
                paint.setColor(0x66FFFFFF);
                c.drawCircle(hx, hy, r, paint);
                paint.setStyle(Paint.Style.FILL);
                paint.setColor(0x88FFFFFF);
                c.drawCircle(hx, hy, Math.max(6f, 10f * uiScale()), paint);
            }
        } else if (stickActive) {
            c.drawCircle(stickX, stickY, stickRadiusPx(E_STICK), paint);
            paint.setStyle(Paint.Style.FILL);
            paint.setColor(0x66FFFFFF);
            c.drawCircle(knobX, knobY, knobRadiusPx(E_STICK), paint);
        }

        paint.setTextAlign(Paint.Align.CENTER);
        for (int i = 0; i < btnIds.length; i++) {
            boolean down = (buttons & btnIds[i]) != 0;
            float r = radius(i);
            paint.setStyle(Paint.Style.FILL);
            paint.setColor(down ? 0xCC66FF99 : 0x66FFFFFF);
            c.drawCircle(btnPos[i][0], btnPos[i][1], r, paint);
            paint.setStyle(Paint.Style.STROKE);
            paint.setColor(0xAA000000);
            c.drawCircle(btnPos[i][0], btnPos[i][1], r, paint);
            paint.setStyle(Paint.Style.FILL);
            paint.setColor(Color.BLACK);
            paint.setTextSize(70f * uiScale() * ns[E_A + i]);
            c.drawText(btnLabels[i], btnPos[i][0],
                    btnPos[i][1] + 25f * uiScale() * ns[E_A + i], paint);
        }

        if (!analogMode || editMode) {
            for (int i = 0; i < 4; i++) {
                boolean down = (buttons & cIds[i]) != 0;
                float r = cRadius(i);
                paint.setStyle(Paint.Style.FILL);
                paint.setColor(down ? 0xCC66FF99 : 0x55FFFFFF);
                c.drawCircle(cPos[i][0], cPos[i][1], r, paint);
                paint.setStyle(Paint.Style.STROKE);
                paint.setColor(0xAA000000);
                c.drawCircle(cPos[i][0], cPos[i][1], r, paint);
                paint.setStyle(Paint.Style.FILL);
                paint.setColor(Color.BLACK);
                paint.setTextSize(46f * uiScale() * ns[E_CUP + i]);
                c.drawText(cLabels[i], cPos[i][0],
                        cPos[i][1] + 16f * uiScale() * ns[E_CUP + i], paint);
            }
        } else if (rstickActive) {
            // Radial heatmap: colour = effective cam rate at that radius, so
            // the response curve, the deadzone ring and the live vector are
            // all readable in the stick circle.
            drawStickHeatmap(c, rstickX, rstickY, stickRadiusPx(E_RSTICK), stickVecX(), stickVecY());
            paint.setStyle(Paint.Style.STROKE);
            paint.setStrokeWidth(6f);
            paint.setColor(0x88FFFFFF);
            c.drawCircle(rstickX, rstickY, stickRadiusPx(E_RSTICK), paint);
            paint.setStyle(Paint.Style.FILL);
            paint.setColor(0x66FFFFFF);
            c.drawCircle(rknobX, rknobY, knobRadiusPx(E_RSTICK), paint);
        }

        // C-buttons / analog-camera mode toggle.
        {
            float r = modeRadius();
            paint.setStyle(Paint.Style.FILL);
            paint.setColor(analogMode ? 0xCC66FF99 : 0x44FFFFFF);
            c.drawCircle(modePos[0], modePos[1], r, paint);
            paint.setStyle(Paint.Style.STROKE);
            paint.setColor(0xAA000000);
            c.drawCircle(modePos[0], modePos[1], r, paint);
            paint.setStyle(Paint.Style.FILL);
            paint.setColor(Color.BLACK);
            paint.setTextSize(38f * uiScale() * ns[E_MODE]);
            c.drawText(analogMode ? "ANA" : "C", modePos[0],
                    modePos[1] + 13f * uiScale() * ns[E_MODE], paint);
        }

        // Camera buttons (X = free camera toggle, Y = recentre). X turns solid
        // green while the free camera is active, so the mode is always visible.
        boolean freeCam = sm64portActivity.nativeGetCamMode() != 0;
        for (int i = 0; i < camIds.length; i++) {
            boolean down = (buttons & camIds[i]) != 0;
            boolean on = (i == 0) && freeCam;
            float r = camRadius(i);
            paint.setStyle(Paint.Style.FILL);
            paint.setColor(on ? 0xEE33DD55 : (down ? 0xCC66FF99 : 0x44FFFFFF));
            c.drawCircle(camPos[i][0], camPos[i][1], r, paint);
            paint.setStyle(Paint.Style.STROKE);
            paint.setColor(0xAA000000);
            c.drawCircle(camPos[i][0], camPos[i][1], r, paint);
            paint.setStyle(Paint.Style.FILL);
            paint.setColor(Color.BLACK);
            paint.setTextSize(48f * uiScale() * ns[E_CAMX + i]);
            c.drawText(camLabels[i], camPos[i][0],
                    camPos[i][1] + 17f * uiScale() * ns[E_CAMX + i], paint);
        }

        // Portal gun: two colour buttons (the reticle is drawn in-game).
        if ((portalGunHeld() && !grappleEquipped) || editMode) {
            for (int i = 0; i < 2; i++) {
                boolean on = portalPressed == i;
                float r = portalRadius(i);
                paint.setStyle(Paint.Style.FILL);
                if (i == 0) {
                    paint.setColor(on ? 0xFF3366FF : 0x993060FF);
                } else {
                    paint.setColor(on ? 0xFFFF9933 : 0x99FF8020);
                }
                c.drawCircle(portalPos[i][0], portalPos[i][1], r, paint);
                paint.setStyle(Paint.Style.STROKE);
                paint.setColor(0xCCFFFFFF);
                c.drawCircle(portalPos[i][0], portalPos[i][1], r, paint);
                paint.setStyle(Paint.Style.FILL);
                paint.setColor(Color.WHITE);
                paint.setTextSize(38f * uiScale() * ns[E_P1 + i]);
                c.drawText(i == 0 ? "P1" : "P2",
                        portalPos[i][0] - 22f * uiScale() * ns[E_P1 + i],
                        portalPos[i][1] + 14f * uiScale() * ns[E_P1 + i], paint);
            }

        }

        // Tool swap: always visible.  Green when the grapple is in hand, blue
        // when the portal gun is; a tap toggles and the HUD note confirms.
        {
            float r = toolRadius();
            paint.setStyle(Paint.Style.FILL);
            paint.setColor(grappleEquipped ? 0xCC33CC66 : 0xCC3366FF);
            c.drawCircle(toolPos[0], toolPos[1], r, paint);
            paint.setStyle(Paint.Style.STROKE);
            paint.setColor(0xCCFFFFFF);
            paint.setStrokeWidth(4f);
            c.drawCircle(toolPos[0], toolPos[1], r, paint);
            paint.setStrokeWidth(6f);
            paint.setStyle(Paint.Style.FILL);
            paint.setColor(Color.WHITE);
            paint.setTextSize(30f * uiScale() * ns[E_TOOL]);
            c.drawText(grappleEquipped ? "HOOK" : "GUN",
                    toolPos[0], toolPos[1] + 11f * uiScale() * ns[E_TOOL], paint);
        }

        // Jetpack: hold to thrust (equips on the first press).
        {
            float r = jetRadius();
            paint.setStyle(Paint.Style.FILL);
            paint.setColor(jetDown ? 0xEEFF9933 : 0xAAFF8020);
            c.drawCircle(jetPos[0], jetPos[1], r, paint);
            paint.setStyle(Paint.Style.STROKE);
            paint.setColor(0xCCFFFFFF);
            paint.setStrokeWidth(4f);
            c.drawCircle(jetPos[0], jetPos[1], r, paint);
            paint.setStrokeWidth(6f);
            paint.setStyle(Paint.Style.FILL);
            paint.setColor(Color.WHITE);
            paint.setTextSize(30f * uiScale() * ns[E_JET]);
            c.drawText("JET", jetPos[0], jetPos[1] + 11f * uiScale() * ns[E_JET], paint);
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

        if (editMode) {
            drawEditOverlay(c);
        }

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
            /* A manual camera with no on-screen affordance reads as a frozen
             * black screen when it is pointed at open sky. */
            paint.setTextSize(dp(12));
            paint.setColor(0xFFCCD6E6);
            c.drawText("FREE CAM: drag to look, pinch to move",
                    x0, y0 + h2 + dp(16), paint);
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

    /* ------------------------------------------------------------------ *
     * Layout editor
     * ------------------------------------------------------------------ */

    /** Half-size of the resize grip drawn in the control's bottom-right. */
    private float gripHalf(int e) {
        return Math.max(14f, elemRadiusPx(e) * 0.20f);
    }

    /** Banner font size, shared by the drawing and the button-row geometry. */
    private float editBannerTextSize() {
        return Math.max(18f, 22f * uiScale());
    }

    /**
     * Screen boxes of the SAVE / DISCARD buttons, left-aligned under the
     * three-line editor banner: {x, y, w, h} for SAVE then DISCARD.  The
     * banner height is fixed (three lines), so the row does not move while a
     * control is dragged and the buttons stay under the same finger.
     */
    private void editButtonRects(float[] out) {
        float ts = editBannerTextSize();
        float pad = 10f, lh = ts * 1.35f;
        float y = dp(8) + (3f * lh + pad) + dp(8);
        float h = Math.max(dp(36), lh + pad);
        float gap = dp(8);
        paint.setTextSize(ts);
        float w0 = Math.max(dp(92), paint.measureText("SAVE") + pad * 2f);
        float w1 = Math.max(dp(92), paint.measureText("DISCARD") + pad * 2f);
        out[0] = dp(8);  out[1] = y; out[2] = w0; out[3] = h;
        out[4] = out[0] + w0 + gap; out[5] = y; out[6] = w1; out[7] = h;
    }

    /** 0 = SAVE, 1 = DISCARD, -1 = neither. */
    private int hitEditButton(float x, float y) {
        editButtonRects(editBtnRects);
        for (int i = 0; i < 2; i++) {
            float bx = editBtnRects[i * 4], by = editBtnRects[i * 4 + 1];
            if (x >= bx && x <= bx + editBtnRects[i * 4 + 2]
                    && y >= by && y <= by + editBtnRects[i * 4 + 3]) {
                return i;
            }
        }
        return -1;
    }

    private int hitEditable(float x, float y) {
        // Grip first, so a grip that pokes into a neighbour stays grabbable.
        for (int e = 0; e < ELEM_COUNT; e++) {
            float r = elemRadiusPx(e);
            float gx = nx[e] * getWidth() + r * 0.95f;
            float gy = ny[e] * getHeight() + r * 0.95f;
            float g = gripHalf(e);
            if (Math.abs(x - gx) <= g && Math.abs(y - gy) <= g) return e;
        }
        int best = -1;
        float bestD = Float.MAX_VALUE;
        for (int e = 0; e < ELEM_COUNT; e++) {
            float cx = nx[e] * getWidth(), cy = ny[e] * getHeight();
            float d = (float) Math.hypot(x - cx, y - cy);
            if (d <= elemRadiusPx(e) * 1.12f && d < bestD) {
                best = e;
                bestD = d;
            }
        }
        return best;
    }

    private float snapX(float v, int e) {
        float r = elemRadiusPx(e) / Math.max(1f, getWidth());
        float lo = Math.min(r, 0.45f), hi = Math.max(1f - r, 0.55f);
        v = Math.max(lo, Math.min(hi, v));
        if (Math.abs(v - lo) <= 0.012f) return lo;
        if (Math.abs(v - hi) <= 0.012f) return hi;
        if (Math.abs(v - 0.5f) <= 0.008f) return 0.5f;
        return Math.max(lo, Math.min(hi, Math.round(v / GRID) * GRID));
    }

    private float snapY(float v, int e) {
        float r = elemRadiusPx(e) / Math.max(1f, getHeight());
        float lo = Math.min(r, 0.45f), hi = Math.max(1f - r, 0.55f);
        v = Math.max(lo, Math.min(hi, v));
        if (Math.abs(v - lo) <= 0.012f) return lo;
        if (Math.abs(v - hi) <= 0.012f) return hi;
        if (Math.abs(v - 0.5f) <= 0.008f) return 0.5f;
        return Math.max(lo, Math.min(hi, Math.round(v / GRID) * GRID));
    }

    private boolean handleEditTouch(MotionEvent e) {
        lastTouchMs = System.currentTimeMillis();
        int action = e.getActionMasked();
        switch (action) {
            case MotionEvent.ACTION_DOWN: {
                float px = e.getX(), py = e.getY();
                /* SAVE / DISCARD live under the banner; they never start a
                 * drag, so a tap there cannot nudge the control beneath. */
                int btn = hitEditButton(px, py);
                if (btn >= 0) {
                    selectedElem = -1;
                    dragElem = -1;
                    dragPointer = -1;
                    dragMode = DRAG_NONE;
                    infoValid = false;
                    if (btn == 0) {
                        saveEditLayout();
                    } else {
                        discardEditLayout();
                    }
                    invalidate();
                    return true;
                }
                editNote = null;
                int hit = hitEditable(px, py);
                selectedElem = hit;
                dragElem = hit;
                dragPointer = e.getPointerId(0);
                if (hit >= 0) {
                    float cx = nx[hit] * getWidth(), cy = ny[hit] * getHeight();
                    float r = elemRadiusPx(hit);
                    float gx = cx + r * 0.95f, gy = cy + r * 0.95f;
                    float g = gripHalf(hit);
                    if (Math.abs(px - gx) <= g && Math.abs(py - gy) <= g) {
                        dragMode = DRAG_RESIZE;
                        dragStartDist = Math.max(1f,
                                (float) Math.hypot(px - cx, py - cy));
                        dragStartScale = ns[hit];
                    } else {
                        dragMode = DRAG_MOVE;
                        dragOffX = cx - px;
                        dragOffY = cy - py;
                    }
                    infoX = nx[hit];
                    infoY = ny[hit];
                    infoValid = true;
                } else {
                    dragMode = DRAG_NONE;
                    dragElem = -1;
                    dragPointer = -1;
                }
                invalidate();
                return true;
            }
            case MotionEvent.ACTION_MOVE: {
                if (dragElem < 0) return true;
                int idx = e.findPointerIndex(dragPointer);
                if (idx < 0) return true;
                editNote = null;
                float px = e.getX(idx), py = e.getY(idx);
                if (dragMode == DRAG_MOVE) {
                    float cx = px + dragOffX, cy = py + dragOffY;
                    nx[dragElem] = snapX(cx / Math.max(1f, getWidth()), dragElem);
                    ny[dragElem] = snapY(cy / Math.max(1f, getHeight()), dragElem);
                } else if (dragMode == DRAG_RESIZE) {
                    float cx = nx[dragElem] * getWidth();
                    float cy = ny[dragElem] * getHeight();
                    float d = Math.max(1f, (float) Math.hypot(px - cx, py - cy));
                    float s = dragStartScale * (d / dragStartDist);
                    ns[dragElem] = Math.max(SCALE_MIN, Math.min(SCALE_MAX, s));
                }
                infoX = nx[dragElem];
                infoY = ny[dragElem];
                infoValid = true;
                /* The boxes are drawn from nx/ny/ns, but every control's hit
                 * area and render position comes from the pixel caches.  Rebind
                 * them with the drag, or the box moves while the control does
                 * not.  Twenty multiplies + three array writes per delta. */
                rebindGeometry();
                return true;
            }
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_CANCEL:
            case MotionEvent.ACTION_POINTER_UP: {
                int id = e.getPointerId(e.getActionIndex());
                if (editMode && (action != MotionEvent.ACTION_POINTER_UP || id == dragPointer)) {
                    dragElem = -1;
                    dragPointer = -1;
                    dragMode = DRAG_NONE;
                    /* Deliberately no persist here: a drag edits the working
                     * copy.  SAVE, or leaving the editor (chip / panel toggle /
                     * `cmd layout off`), commits it; DISCARD throws it away. */
                    invalidate();
                }
                return true;
            }
        }
        return true;
    }

    /** Draw boxes, names, resize grips, SAVE / DISCARD and a live banner. */
    private void drawEditOverlay(Canvas c) {
        float w = getWidth(), h = getHeight();
        float base = Math.max(16f, 20f * uiScale());
        for (int e = 0; e < ELEM_COUNT; e++) {
            float cx = nx[e] * w, cy = ny[e] * h, r = elemRadiusPx(e);
            boolean sel = e == selectedElem;
            paint.setStyle(Paint.Style.STROKE);
            paint.setStrokeWidth(sel ? 4f : 2f);
            paint.setColor(sel ? 0xFFFFCC33 : 0xBB66CCFF);
            c.drawRect(cx - r, cy - r, cx + r, cy + r, paint);

            paint.setStyle(Paint.Style.FILL);
            paint.setColor(sel ? 0xFFFFCC33 : 0xFF8FD4FF);
            paint.setTextAlign(Paint.Align.LEFT);
            paint.setTextSize(base);
            c.drawText(ELEM_LABELS[e], cx - r, Math.max(base, cy - r - 6f), paint);

            float g = gripHalf(e);
            float gx = cx + r * 0.95f, gy = cy + r * 0.95f;
            paint.setStyle(Paint.Style.FILL);
            paint.setColor(sel ? 0xFFFFCC33 : 0xFF66CCFF);
            c.drawRect(gx - g, gy - g, gx + g, gy + g, paint);
            paint.setStyle(Paint.Style.STROKE);
            paint.setStrokeWidth(2f);
            paint.setColor(0xCC000000);
            c.drawRect(gx - g, gy - g, gx + g, gy + g, paint);
        }

        paint.setTextAlign(Paint.Align.LEFT);
        paint.setTextSize(editBannerTextSize());
        float pad = 10f;
        float lh = paint.getTextSize() * 1.35f;
        String head = "EDIT LAYOUT  -  drag = move, corner grip = resize"
                + "  |  grid " + Math.round(GRID * 1000f) / 10f + "%";
        String profile = "profile: " + activeProfile() + "   (DEBUG > SYS > RESET LAYOUT)";
        String status = null;
        if (dragElem >= 0) {
            int e = dragElem;
            status = ELEM_LABELS[e] + "  x=" + String.format(java.util.Locale.US, "%.3f", nx[e])
                    + "  y=" + String.format(java.util.Locale.US, "%.3f", ny[e])
                    + "  scale=" + String.format(java.util.Locale.US, "%.2f", ns[e])
                    + "  px=" + Math.round(nx[e] * w) + "," + Math.round(ny[e] * h)
                    + "  r=" + Math.round(elemRadiusPx(e));
        } else if (editNote != null) {
            status = editNote;
        } else if (infoValid && selectedElem >= 0) {
            int e = selectedElem;
            status = ELEM_LABELS[e] + "  x=" + String.format(java.util.Locale.US, "%.3f", nx[e])
                    + "  y=" + String.format(java.util.Locale.US, "%.3f", ny[e])
                    + "  scale=" + String.format(java.util.Locale.US, "%.2f", ns[e]);
        }
        /* Always three lines, even when the status line is empty, so the
         * SAVE / DISCARD row below never jumps during a drag. */
        float bw = Math.max(paint.measureText(head), paint.measureText(profile));
        if (status != null) bw = Math.max(bw, paint.measureText(status));
        bw += pad * 2;
        float bh = 3f * lh + pad;
        float x0 = dp(8), y0 = dp(8);
        paint.setStyle(Paint.Style.FILL);
        paint.setColor(0xCC101820);
        c.drawRoundRect(x0, y0, x0 + bw, y0 + bh, 12f, 12f, paint);
        paint.setColor(0xFF66CCFF);
        c.drawText(head, x0 + pad, y0 + pad + lh * 0.8f, paint);
        paint.setColor(0xFFAACCFF);
        c.drawText(profile, x0 + pad, y0 + pad + lh * 1.8f, paint);
        if (status != null) {
            paint.setColor(0xFFFFCC33);
            c.drawText(status, x0 + pad, y0 + pad + lh * 2.8f, paint);
        } else {
            paint.setColor(0xFF6F8FAF);
            c.drawText(editDirty() ? "unsaved changes - SAVE commits, DISCARD reverts"
                    : "tap SAVE to commit, DISCARD to revert and leave",
                    x0 + pad, y0 + pad + lh * 2.8f, paint);
        }

        /* SAVE / DISCARD: canvas buttons (not Android views) so they work with
         * the debug panel closed and cannot lose a race with a drag. */
        editButtonRects(editBtnRects);
        for (int i = 0; i < 2; i++) {
            boolean save = i == 0;
            float bx = editBtnRects[i * 4], by = editBtnRects[i * 4 + 1];
            float bw2 = editBtnRects[i * 4 + 2], bh2 = editBtnRects[i * 4 + 3];
            paint.setStyle(Paint.Style.FILL);
            paint.setColor(save ? 0xE61E4D2E : 0xE64D1E1E);
            c.drawRoundRect(bx, by, bx + bw2, by + bh2, 10f, 10f, paint);
            paint.setStyle(Paint.Style.STROKE);
            paint.setStrokeWidth(2f);
            paint.setColor(save ? 0xFF66E08A : 0xFFE08866);
            c.drawRoundRect(bx, by, bx + bw2, by + bh2, 10f, 10f, paint);
            paint.setStyle(Paint.Style.FILL);
            paint.setColor(save ? 0xFFB8FFCF : 0xFFFFC4B8);
            paint.setTextAlign(Paint.Align.CENTER);
            paint.setTextSize(editBannerTextSize());
            float ts = paint.getTextSize();
            c.drawText(save ? "SAVE" : "DISCARD", bx + bw2 * 0.5f,
                    by + bh2 * 0.5f + ts * 0.35f, paint);
        }
        paint.setTextAlign(Paint.Align.LEFT);
    }

    /**
     * Start/stop editing. Entering clears any held game input and records the
     * committed layout as the DISCARD target; leaving commits the working copy
     * (the chip, the panel toggle and `cmd layout off` all exit this way).
     */
    public void setEditMode(boolean on) {
        if (editMode == on) return;
        editMode = on;
        if (on) {
            clearInput();
            controlsHidden = false;
            infoValid = false;
            selectedElem = -1;
            dragElem = -1;
            dragPointer = -1;
            dragMode = DRAG_NONE;
            editNote = null;
            snapshotEditLayout();
        } else {
            persistLayout();
            snapshotEditLayout();
            selectedElem = -1;
            dragElem = -1;
            dragPointer = -1;
            dragMode = DRAG_NONE;
            editNote = null;
        }
        /* The drag ran the model forward; make the hit/render caches agree
         * before the next frame draws either state. */
        layoutButtons();
        invalidate();
        android.util.Log.i("SM64Layout", "edit mode " + (on ? "ON" : "OFF")
                + " profile=" + activeProfile());
        /* Mirror the state in the HUD chip / panel widgets immediately instead
         * of waiting for the 500 ms readout. */
        sm64portActivity act = sm64portActivity.instance;
        if (act != null) act.layoutEditModeChanged();
    }

    /** Remember the working copy as the last committed layout. */
    private void snapshotEditLayout() {
        System.arraycopy(nx, 0, savedX, 0, ELEM_COUNT);
        System.arraycopy(ny, 0, savedY, 0, ELEM_COUNT);
        System.arraycopy(ns, 0, savedS, 0, ELEM_COUNT);
    }

    /** True while the working copy differs from the last SAVE / entry state. */
    private boolean editDirty() {
        for (int e = 0; e < ELEM_COUNT; e++) {
            if (nx[e] != savedX[e] || ny[e] != savedY[e] || ns[e] != savedS[e]) {
                return true;
            }
        }
        return false;
    }

    /**
     * SAVE: commit the working layout to the active profile and stay in edit
     * mode (so a tune-save-tune loop needs one tap, not an exit and re-entry).
     * This is the same store write the exit path uses.
     */
    public boolean saveEditLayout() {
        persistLayout();
        snapshotEditLayout();
        editNote = "saved '" + activeProfile() + "'";
        android.util.Log.i("SM64Layout", "layout saved " + activeProfile());
        invalidate();
        return true;
    }

    /**
     * DISCARD: restore the layout as of editor entry / the last SAVE, write it
     * back so the store matches the screen (drag deltas are working-copy only),
     * and leave edit mode.
     */
    public boolean discardEditLayout() {
        if (!editMode) {
            android.util.Log.i("SM64Layout", "layout discard ignored (not editing)");
            return false;
        }
        System.arraycopy(savedX, 0, nx, 0, ELEM_COUNT);
        System.arraycopy(savedY, 0, ny, 0, ELEM_COUNT);
        System.arraycopy(savedS, 0, ns, 0, ELEM_COUNT);
        infoValid = false;
        selectedElem = -1;
        android.util.Log.i("SM64Layout", "layout discarded -> " + activeProfile());
        setEditMode(false);   // commits the restored (= last saved) layout
        return true;
    }

    public boolean isEditMode() {
        return editMode;
    }

    private void clearInput() {
        buttons = 0;
        for (int i = 0; i < pressedPointer.length; i++) pressedPointer[i] = -1;
        for (int i = 0; i < cPressed.length; i++) cPressed[i] = -1;
        for (int i = 0; i < camPressed.length; i++) camPressed[i] = -1;
        stickActive = false;
        stickPointer = -1;
        rstickActive = false;
        rstickPointer = -1;
        toolDown = false;
        toolPointer = -1;
        jetDown = false;
        jetPointer = -1;
        portalPressed = -1;
        portalPointer = -1;
        camGesture = false;
        camPtr1 = camPtr2 = -1;
        camGestureMode = GESTURE_IDLE;
        try {
            sm64portActivity.nativeOverlayStick2(0, 0, 0);
            sm64portActivity.nativeOverlayInput(0, 0, 0, 0);
            sm64portActivity.nativeJetpack(0);
        } catch (Throwable ignored) {
        }
    }

    private void persistLayout() {
        if (layoutStore != null) {
            layoutStore.saveActive(ELEM_KEYS, nx, ny, ns);
        }
    }

    /** RESET: active profile gets the `default` profile's contents; if it is
     *  already the default profile, the hard-coded factory layout is used. */
    public void resetLayout() {
        if (layoutStore == null) {
            restoreFactory();
        } else if (TouchLayoutStore.DEFAULT_PROFILE.equals(layoutStore.active())) {
            restoreFactory();
        } else {
            layoutStore.loadProfile(TouchLayoutStore.DEFAULT_PROFILE,
                    ELEM_KEYS, nx, ny, ns);
        }
        persistLayout();
        if (editMode) snapshotEditLayout();   // RESET is itself a commit
        infoValid = false;
        layoutButtons();
        invalidate();
        android.util.Log.i("SM64Layout", "layout reset; active=" + activeProfile());
    }

    public java.util.List<String> listProfiles() {
        if (layoutStore == null) {
            java.util.List<String> one = new java.util.ArrayList<>();
            one.add(TouchLayoutStore.DEFAULT_PROFILE);
            return one;
        }
        return layoutStore.profiles();
    }

    public String activeProfile() {
        return layoutStore == null ? TouchLayoutStore.DEFAULT_PROFILE
                : layoutStore.active();
    }

    public boolean switchProfile(String name) {
        if (layoutStore == null || name == null) return false;
        if (!layoutStore.applyProfile(name, ELEM_KEYS, nx, ny, ns)) return false;
        infoValid = false;
        if (editMode) snapshotEditLayout();   // new profile = new commit point
        layoutButtons();
        invalidate();
        return true;
    }

    public boolean saveProfileAs(String name) {
        if (layoutStore == null || name == null) return false;
        boolean ok = layoutStore.saveAs(name, ELEM_KEYS, nx, ny, ns);
        if (ok && editMode) snapshotEditLayout();
        invalidate();
        return ok;
    }

    public boolean renameProfile(String oldName, String newName) {
        return layoutStore != null && layoutStore.rename(oldName, newName);
    }

    public boolean deleteProfile(String name) {
        if (layoutStore == null || !layoutStore.delete(name)) return false;
        layoutStore.applyProfile(layoutStore.active(), ELEM_KEYS, nx, ny, ns);
        if (editMode) snapshotEditLayout();
        layoutButtons();
        invalidate();
        return true;
    }

    public void exportProfile() {
        if (layoutStore != null) layoutStore.exportActive();
    }

    public String externalDir() {
        return layoutStore == null ? "unavailable" : layoutStore.externalDir();
    }

    public int elementCount() {
        return ELEM_COUNT;
    }

    public String elementName(int i) {
        return i >= 0 && i < ELEM_COUNT ? ELEM_KEYS[i] : "-";
    }

    public void setSelectedElement(int i) {
        selectedElem = i >= 0 && i < ELEM_COUNT ? i : -1;
        infoValid = false;
        invalidate();
    }

    public int selectedElement() {
        return selectedElem;
    }

    /** axis 0 = x, 1 = y; values are normalised. */
    public void setSelectedNorm(int axis, float v) {
        if (selectedElem < 0) return;
        if (axis == 0) {
            nx[selectedElem] = snapX(v, selectedElem);
        } else {
            ny[selectedElem] = snapY(v, selectedElem);
        }
        infoX = nx[selectedElem];
        infoY = ny[selectedElem];
        infoValid = true;
        persistLayout();
        if (editMode) snapshotEditLayout();
        layoutButtons();
        invalidate();
    }

    public void setSelectedScale(float s) {
        if (selectedElem < 0) return;
        ns[selectedElem] = Math.max(SCALE_MIN, Math.min(SCALE_MAX, s));
        persistLayout();
        if (editMode) snapshotEditLayout();
        layoutButtons();
        invalidate();
    }

    public String statusLine() {
        String sel = selectedElem >= 0 ? ELEM_KEYS[selectedElem] : "-";
        String coords = selectedElem >= 0
                ? String.format(java.util.Locale.US, "%.3f/%.3f/%.2f",
                nx[selectedElem], ny[selectedElem], ns[selectedElem])
                : "-";
        return "layout=" + activeProfile()
                + ",profiles=" + listProfiles().size()
                + ",edit=" + (editMode ? 1 : 0)
                + ",unsaved=" + (editMode && editDirty() ? 1 : 0)
                + ",sel=" + sel + "," + coords
                + ",elems=" + ELEM_COUNT
                + ",ext=" + externalDir();
    }

    private boolean fpvActive() {
        try {
            return sm64portActivity.nativeGetCamMode() == 4;
        } catch (Throwable t) {
            return false;
        }
    }
}

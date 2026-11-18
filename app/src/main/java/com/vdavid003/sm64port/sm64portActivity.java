package com.vdavid003.sm64port;

import android.graphics.Color;
import android.hardware.Sensor;
import android.hardware.SensorEvent;
import android.hardware.SensorEventListener;
import android.hardware.SensorManager;
import android.graphics.SurfaceTexture;
import android.os.Bundle;
import android.util.Log;
import android.view.Gravity;
import android.view.Surface;
import android.view.View;
import android.view.ViewGroup;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.RelativeLayout;
import android.widget.SeekBar;
import android.widget.TextView;

import com.leia.core.PlatformInitArgs;
import com.leia.sdk.LeiaSDK;
import com.leia.sdk.graphics.SurfaceTextureReadyCallback;
import com.leia.sdk.views.InputViewsAsset;
import com.leia.sdk.views.InterlacedSurfaceView;

import org.libsdl.app.SDLActivity;

/**
 * SDL activity that redirects SM64's rendering into a Leia video surface.
 *
 * SDL renders a 2x1 side-by-side image (left eye / right eye) into the Surface
 * we hand it, Leia's interlacer weaves that onto the lenticular panel, and the
 * visible {@link InterlacedSurfaceView} shows the woven result. Touch input
 * still goes to SDL's own SurfaceView underneath.
 */
public class sm64portActivity extends SDLActivity implements SensorEventListener {
    private static final String TAG = "SM64Leia";
    private static final String BUILD_NAME = "hfr3+ssfx";
    public static volatile sm64portActivity instance;
    /** Leia's convention: a 2-view SBS source is 2 x viewResolution
     *  (1920x1200 each), i.e. 3840x1200 total. The interlacer derives its
     *  source geometry from the SurfaceTexture's default buffer size, not
     *  from setSourceSize(). */
    private static final int SBS_W = 3840;
    private static final int SBS_H = 1200;

    private LeiaSDK sdk;
    private InterlacedSurfaceView leiaView;
    private InputViewsAsset asset;
    private LinearLayout controls;
    private SurfaceTexture leiaTexture;
    private final SeekBar[] bars = new SeekBar[24];
    private final SeekBar[] outBars = new SeekBar[6];
    private final int[] outDefs = new int[6];
    private final int[] outVal = new int[6];
    private android.widget.GridLayout grid;
    private TextView modeButton;
    private TextView fpsView;
    private TextView fpsButton;
    private TextView interpButton;
    private TouchOverlay touchOverlay;
    private android.widget.ScrollView scrollerRef;
    private TextView debugView;
    private LinearLayout tabContent;
    private final java.util.List<View> tabViews = new java.util.ArrayList<>();
    private final java.util.List<TextView> tabButtons = new java.util.ArrayList<>();
    private final java.util.List<TextView> fxDebugButtons = new java.util.ArrayList<>();
    private final java.util.List<Integer> fxDebugModes = new java.util.ArrayList<>();
    private TextView aoToggle, dofToggle, giToggle;
    private boolean aoOn, dofOn, giOn;
    private int fxDebug = 0;
    private boolean stereoOn = true;
    private boolean testOn = false;
    private boolean tintOn, zbufOn, wireOn, perfOn, swapOn, sameOn, nocullOn, flatOn, analogOn;
    private boolean stayOn = true, uiParallaxOn, uiBufOn;
    private boolean toeAutoOn = true;
    // Camera tracking (Leia eye/face tracking + tablet gyro).
    private boolean hdtOn, gyroOn;
    private int hdtStrength = 100, gyroStrength = 100, gyroPivot = 12;
    private SensorManager sensorManager;
    private Sensor gyroSensor;
    private final float[] gyroRef = new float[3];
    private boolean gyroRefSet, gyroHaveSample;
    private int hdtLogCount;
    private TextView camHeadToggle, camGyroToggle;
    private boolean uiPostOn = true;
    private int overlayAlphaPct = 100;
    // true = the assistant is driving the app: debug line turns red.
    private boolean busyOn;
    private final java.util.List<TextView> fitButtons = new java.util.ArrayList<>();
    private int fitMode = 0;
    private int modeIndex = 0;
    private int refreshIdx = 0;

    /** Cycle the panel refresh rate: default -> 60 -> 120 Hz. */
    private int fpsMode = 2;   // 120 Hz render rate by default
    private boolean interpOn = true;

    /** Cycle render rate: 30 -> 60 -> 120 -> uncapped, and match panel refresh. */
    private void cycleFpsMode() {
        fpsMode = (fpsMode + 1) % 4;
        try {
            nativeSetFpsMode(fpsMode);
            String label = fpsMode == 0 ? "30" : fpsMode == 1 ? "60"
                    : fpsMode == 2 ? "120" : "uncapped";
            Log.i(TAG, "FPS_MODE " + label);
            if (fpsButton != null) fpsButton.setText("FPS " + label);
            int hz = fpsMode == 0 ? 30 : fpsMode == 1 ? 60 : fpsMode == 2 ? 120 : 120;
            applyPanelRefresh(hz);
        } catch (Throwable t) {
            Log.e(TAG, "fps mode failed", t);
        }
    }

    private void applyPanelRefresh(int hz) {
        try {
            android.view.Display d = getWindowManager().getDefaultDisplay();
            android.view.Display.Mode[] modes = d.getSupportedModes();
            int modeId = 0;
            for (android.view.Display.Mode m : modes) {
                if (Math.round(m.getRefreshRate()) == hz) { modeId = m.getModeId(); break; }
            }
            android.view.WindowManager.LayoutParams lp = getWindow().getAttributes();
            lp.preferredDisplayModeId = modeId;
            getWindow().setAttributes(lp);
            Log.i(TAG, "panel refresh -> " + hz + "Hz modeId=" + modeId);
        } catch (Throwable t) {
            Log.e(TAG, "panel refresh failed", t);
        }
    }

    private void cycleRefreshRate() {
        try {
            android.view.Display d = getWindowManager().getDefaultDisplay();
            android.view.Display.Mode[] modes = d.getSupportedModes();
            refreshIdx = (refreshIdx + 1) % 3;
            int want = refreshIdx == 1 ? 60 : refreshIdx == 2 ? 120 : 0;
            int modeId = 0;
            if (want != 0) {
                for (android.view.Display.Mode m : modes) {
                    if (Math.round(m.getRefreshRate()) == want) {
                        modeId = m.getModeId();
                        break;
                    }
                }
            }
            android.view.WindowManager.LayoutParams lp = getWindow().getAttributes();
            lp.preferredDisplayModeId = modeId;
            getWindow().setAttributes(lp);
            try {
                if (android.os.Build.VERSION.SDK_INT >= 30 && leiaView != null
                        && leiaView.getHolder().getSurface() != null) {
                    float rate = want == 0 ? 60f : (float) want;
                    leiaView.getHolder().getSurface().setFrameRate(
                            rate, android.view.Surface.FRAME_RATE_COMPATIBILITY_DEFAULT);
                    Log.i(TAG, "surface frameRate " + rate);
                }
            } catch (Throwable t) {
                Log.e(TAG, "setFrameRate failed", t);
            }
            Log.i(TAG, "refresh -> " + (want == 0 ? "default" : want + "Hz") + " modeId=" + modeId);
            Log.i(TAG, "refresh " + want + "Hz unavailable");
        } catch (Throwable t) {
            Log.e(TAG, "refresh cycle failed", t);
        }
    }
    private int stIndex = 0;
    private int tglIndex = 0;
    private InterlacedSurfaceView testView;

    static {
        System.loadLibrary("main");
    }

    @Override
    protected String getMainFunction() {
        return "main";
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        instance = this;

        try {
            initLeia();
        } catch (Throwable t) {
            // Never brick the game: fall back to SDL's normal flat output.
            Log.e(TAG, "Leia init failed, staying flat", t);
            SDLActivity.failOverride();
        }
    }

    private void initLeia() throws Exception {
        LeiaSDK.InitArgs args = new LeiaSDK.InitArgs();
        if (args.platform == null) {
            args.platform = new PlatformInitArgs();
        }
        args.platform.context = getApplicationContext();
        args.enableFaceTracking = true;
        args.requiresFaceTrackingPermissionCheck = false;
        // Leia's default logging floods logcat and rotates out our markers.
        try {
            args.faceTrackingServerLogLevel = com.leia.core.LogLevel.Error;
        } catch (Throwable ignored) {}
        sdk = LeiaSDK.createSDK(args);
        sdk.enableBacklight(true);
        Log.i(TAG, "LeiaSDK ready; initialized=" + sdk.isInitialized());
        startCameraTracking();

        // Make SDL wait for our surface rather than grabbing its own.
        SDLActivity.mWaitForOverride = true;

        asset = new InputViewsAsset();
        // Explicit size: without it the interlacer assumes a 1x1 video and
        // stretches a single texel across the panel. 2 x (1920x1200) = SBS.
        asset.CreateEmptySurfaceForVideo(3840, 1200, new SurfaceTextureReadyCallback() {
            @Override
            public void onSurfaceTextureReady(SurfaceTexture texture) {
                // The producer buffer size must match what the interlacer is
                // told the source is, otherwise it samples a magnified crop.
                texture.setDefaultBufferSize(SBS_W, SBS_H);
                Log.i(TAG, "Leia SurfaceTexture ready, default buffer " + SBS_W + "x" + SBS_H);
                // The first woven frame sometimes comes up black until the view
                // is reconfigured; nudge it once the surface exists.
                // The first woven frame comes up black until the view is
                // re-attached; mimic the FLAT toggle the user found works.
                leiaView.postDelayed(new Runnable() {
                    @Override public void run() {
                        leiaView.setVisibility(View.GONE);
                        leiaView.postDelayed(new Runnable() {
                            @Override public void run() {
                                leiaView.setVisibility(View.VISIBLE);
                                reapplySourceSize();
                                Log.i(TAG, "interlacer reattach nudge applied");
                            }
                        }, 400);
                    }
                }, 1500);
                SDLActivity.setOverrideSurface(new Surface(texture));
            }
        });
        Log.i(TAG, "asset created");

        leiaView = new InterlacedSurfaceView(this);
        leiaView.setClickable(false);
        leiaView.setFocusable(false);

        // Attach the view before binding the asset: the interlacer only creates
        // the SurfaceTexture once the view is in a window.
        ViewGroup layout = mLayout;
        if (layout != null) {
            layout.addView(leiaView, new RelativeLayout.LayoutParams(
                    RelativeLayout.LayoutParams.MATCH_PARENT,
                    RelativeLayout.LayoutParams.MATCH_PARENT));

            // Controls on their own layer, above the interlaced view but below
            // the debug panel. Screen-space: never per-eye, never letterboxed.
            touchOverlay = new TouchOverlay(this);
            layout.addView(touchOverlay, new RelativeLayout.LayoutParams(
                    RelativeLayout.LayoutParams.MATCH_PARENT,
                    RelativeLayout.LayoutParams.MATCH_PARENT));
            try {
                nativeHideInGameControls(1);
                Log.i(TAG, "overlay controls active; in-game sprites hidden");
            } catch (Throwable t) {
                Log.w(TAG, "hide in-game controls failed", t);
            }

            RelativeLayout.LayoutParams panelParams = new RelativeLayout.LayoutParams(
                    RelativeLayout.LayoutParams.MATCH_PARENT,
                    RelativeLayout.LayoutParams.WRAP_CONTENT);
            panelParams.addRule(RelativeLayout.ALIGN_PARENT_BOTTOM);
            layout.addView(buildTuningPanel(), panelParams);
        }
        leiaView.setViewAsset(asset);
        Log.i(TAG, "setViewAsset done");
        // Size the surface to the view once laid out (mpv does this on layout).
        leiaView.addOnLayoutChangeListener(new View.OnLayoutChangeListener() {
            @Override
            public void onLayoutChange(View v, int l, int t, int r, int b,
                                       int ol, int ot, int orr, int ob) {
                resizeLeiaSurface(v.getWidth(), v.getHeight());
            }
        });
        // The view may not be laid out yet at this point; retry until it has
        // a real size, then create the surface at that exact geometry.
        leiaView.postDelayed(new Runnable() {
            @Override
            public void run() {
                if (leiaView.getWidth() > 0) {
                    resizeLeiaSurface(leiaView.getWidth(), leiaView.getHeight());
                } else {
                    leiaView.postDelayed(this, 300);
                }
            }
        }, 200);

        leiaView.setSingleViewMode(false);
    }

    /** Debug sliders: they push values straight into the native stereo renderer. */
    private LinearLayout buildTuningPanel() {
        LinearLayout panel = new LinearLayout(this);
        panel.setOrientation(LinearLayout.VERTICAL);
        panel.setBackgroundColor(0xCC101010);
        int pad = (int) (16 * getResources().getDisplayMetrics().density);
        panel.setPadding(pad, pad / 2, pad * 3, pad / 2);

        // Fixed pixel positions so automation can tap buttons reliably.
        android.widget.AbsoluteLayout header = new android.widget.AbsoluteLayout(this);

        TextView title = new TextView(this);
        title.setText("Stereo");
        title.setTextColor(Color.WHITE);
        title.setTextSize(16);
        header.addView(title, abs(16, 4, 300, 60));

        final TextView hide = new TextView(this);
        hide.setText("SHOW");
        hide.setTextColor(0xFF66CCFF);
        hide.setTextSize(16);
        hide.setPadding(pad, pad / 2, pad, pad / 2);
        header.addView(hide, abs(1400, 0, 220, 64));

        TextView reset = new TextView(this);
        reset.setText("RESET");
        reset.setTextColor(0xFFFFCC66);
        reset.setTextSize(16);
        reset.setPadding(pad, pad / 2, pad, pad / 2);
        header.addView(reset, abs(1620, 0, 220, 64));

        modeButton = new TextView(this);
        modeButton.setText("3D");
        modeButton.setTextColor(0xFF66FF99);
        modeButton.setTextSize(16);
        modeButton.setPadding(pad, pad / 2, pad, pad / 2);
        header.addView(modeButton, abs(1840, 0, 220, 64));
        modeButton.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                setStereoMode(!stereoOn);
            }
        });

        TextView flat = new TextView(this);
        flat.setText("FLAT");
        flat.setTextColor(0xFFCCCCCC);
        flat.setTextSize(16);
        flat.setPadding(pad, pad / 2, pad, pad / 2);
        header.addView(flat, abs(2060, 0, 220, 64));
        flat.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                // Zero-parallax view; hiding the Leia view goes black and cannot
                // be recovered (SDL renders into Leia's surface, not its own).
                flatOn = !flatOn;
                nativeSetStereoInt(21, flatOn ? 1 : 0);
                Log.i(TAG, "FLAT zeroParallax=" + flatOn);
            }
        });

        TextView interpBtn = new TextView(this);
        interpButton = interpBtn;
        interpBtn.setText("INTERP ON");
        interpBtn.setTextColor(0xFFCCCCCC);
        interpBtn.setTextSize(16);
        interpBtn.setPadding(pad, pad / 2, pad, pad / 2);
        header.addView(interpBtn, abs(200, 0, 260, 64));
        interpBtn.setOnClickListener(v -> {
            interpOn = !interpOn;
            nativeSetHfrInterp(interpOn ? 1 : 0);
            interpBtn.setText(interpOn ? "INTERP ON" : "INTERP OFF");
            Log.i(TAG, "INTERP " + interpOn);
        });

        TextView fpsBtn = new TextView(this);
        fpsButton = fpsBtn;
        fpsBtn.setText("FPS 120");
        fpsBtn.setTextColor(0xFFCCCCCC);
        fpsBtn.setTextSize(16);
        fpsBtn.setPadding(pad, pad / 2, pad, pad / 2);
        header.addView(fpsBtn, abs(1200, 0, 200, 64));
        fpsBtn.setOnClickListener(v -> cycleFpsMode());

        TextView test = new TextView(this);
        test.setText("TEST");
        test.setTextColor(0xFFCCCCCC);
        test.setTextSize(16);
        test.setPadding(pad, pad / 2, pad, pad / 2);
        test.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                showStaticTestImage();
            }
        });
        panel.addView(header);

        // Tab bar: one page per effect family. The stereo page keeps all the
        // original Leia knobs; the other pages are screen-space FX knobs.
        LinearLayout tabBar = new LinearLayout(this);
        tabBar.setOrientation(LinearLayout.HORIZONTAL);
        tabBar.setPadding(dp(4), 0, dp(4), 0);
        addTabButton(tabBar, "STEREO");
        addTabButton(tabBar, "MXAO");
        addTabButton(tabBar, "C.DOF");
        addTabButton(tabBar, "SSGI");
        addTabButton(tabBar, "CAMERA");
        panel.addView(tabBar);

        tabContent = new LinearLayout(this);
        tabContent.setOrientation(LinearLayout.VERTICAL);
        panel.addView(tabContent, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT));

        // Scrolling container so sliders/buttons never run off screen.
        controls = new LinearLayout(this);
        controls.setOrientation(LinearLayout.VERTICAL);
        android.widget.ScrollView scroller = new android.widget.ScrollView(this);
        scroller.addView(controls, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT));
        int maxH = (int) (getResources().getDisplayMetrics().heightPixels * 0.42f);
        scroller.setFillViewport(false);
        scrollerRef = scroller;
        tabContent.addView(scroller, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                Math.min(900, maxH)));
        tabViews.add(scroller);
        // Collapsed by default: the open panel covers the bottom of the screen
        // and swallowed taps meant for the overlay's START button. HIDE/SHOW
        // toggles all tab pages at once (see the hide listener below).
        tabContent.setVisibility(android.view.View.GONE);

        grid = new android.widget.GridLayout(this);
        grid.setColumnCount(2);
        grid.setOrientation(android.widget.GridLayout.HORIZONTAL);
        controls.addView(grid);
        addSlider("IPD", -300, 300, 0, " px", 0);
        addSlider("Zero parallax", -1280, 1280, 0, " px", 1);
        addSlider("Depth", -200, 200, 10, " %", 2);
        addSlider("UI depth", -60, 60, 0, " px", 38);
        addSlider("Toe in/out", -100, 100, 0, " %", 17);
        addSlider("Aspect % (in)", 10, 240, 178, " %", 18);
        addIntSlider("Aspect % (out)", 0, 240, 0, new java.util.function.IntConsumer() {
            @Override public void accept(int v) { applyOutAspect(v); }
        });
        addSlider("Eye margin%", 50, 100, 100, " %", 14);
        addSlider("FOV %", 1, 200, 100, " %", 20);
        addIntSlider("Ctl opacity", 10, 100, 100, new java.util.function.IntConsumer() {
            @Override public void accept(int v) { applyOverlayAlpha(v); }
        });
        // Hidden for now (kept for later): V%, W-px, H-shift%, V-shift.
        outVal[1] = 100; outVal[3] = 0; outVal[4] = 0;
        startDebugReadout(panel);

        // FPS counter, top-left, green.
        fpsView = new TextView(this);
        fpsView.setTextColor(0xFF44FF66);
        fpsView.setTextSize(18);
        mLayout.addView(fpsView, new RelativeLayout.LayoutParams(
                android.widget.RelativeLayout.LayoutParams.WRAP_CONTENT,
                android.widget.RelativeLayout.LayoutParams.WRAP_CONTENT));

        android.widget.AbsoluteLayout toggles = new android.widget.AbsoluteLayout(this);
        String[][] tgl = {{"ZBUF", "15"}, {"WIRE", "16"}, {"SWAP", "4"}, {"SAME", "6"}, {"TINT", "7"}, {"PERF2x", "12"}, {"STAY", "30"},
                {"UIPOST", "35"}, {"UIPAR", "36"}, {"UIBUF", "37"}, {"TOEAUTO", "40"},
                {"FILL", "13"}, {"4:3", "13"}, {"16:9", "13"}};
        for (String[] t : tgl) {
            final int which = Integer.parseInt(t[1]);
            final TextView b = new TextView(this);
            b.setText(t[0]);
            // UIPOST defaults to on in the renderer; keep the button in sync.
            b.setTextColor((t[0].equals("UIPOST") || t[0].equals("STAY")
                    || t[0].equals("TOEAUTO")) ? 0xFF66FF99 : 0xFF888888);
            b.setTextSize(14);
            b.setPadding(pad, pad / 2, pad, pad / 2);
            final int id = System.identityHashCode(b);
            if (t[1].equals("13")) fitButtons.add(b);
            b.setOnClickListener(new View.OnClickListener() {
                @Override
                public void onClick(View v) {
                    boolean on = b.getCurrentTextColor() == 0xFF888888;
                    b.setTextColor(on ? 0xFF66FF99 : 0xFF888888);
                    Log.i(TAG, "toggle " + t[0] + "=" + (on ? 1 : 0));
                    if (t[0].startsWith("MODE")) {
                        modeIndex = (modeIndex + 1) % 4;
                        Log.i(TAG, "interlaceMode -> #" + modeIndex + " (unsupported in this AAR)");
                        return;
                    }
                    if (t[0].startsWith("SRCFIT")) {
                        com.leia.sdk.views.ScaleType[] st =
                                com.leia.sdk.views.ScaleType.values();
                        stIndex = (stIndex + 1) % st.length;
                        Log.i(TAG, "scaleType -> " + st[stIndex] + " (unsupported in this AAR)");
                        return;
                    }
                    if (t[0].startsWith("RCV")) {
                        float amt = Float.parseFloat(t[0].substring(3)) / 100f;
                        Log.i(TAG, "reconvergence -> " + amt + " (unsupported in this AAR)");
                        return;
                    }
                    if (t[1].equals("13")) {
                        fitMode = on ? fitButtons.indexOf(b) : 0;
                        for (TextView f : fitButtons) {
                            f.setTextColor(f == b && on ? 0xFF66FF99 : 0xFF888888);
                        }
                        nativeSetStereoInt(13, fitMode);
                    } else {
                        nativeSetStereoInt(which, on ? 1 : 0);
                    }
                }
            });
            int bx = 16 + (tglIndex % 7) * 340;
            int by = (tglIndex / 7) * 64;
            toggles.addView(b, abs(bx, by, 340, 60));
            Log.i(TAG, "BTN " + t[0] + " " + bx + " " + by + " 340 60");
            tglIndex++;
        }
        controls.addView(toggles, new LinearLayout.LayoutParams(-2, 130));

        buildFxTabs();
        selectTab(0);

        TextView src = new TextView(this);
        src.setText("SRC");
        src.setTextColor(0xFFCCCCCC);
        src.setTextSize(16);
        src.setPadding(pad, pad / 2, pad, pad / 2);
        header.addView(src, abs(2440, 0, 120, 64));
        src.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                srcIndex = (srcIndex + 1) % SRC_SIZES.length;
                applySourceSize();
            }
        });

        reset.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                // Setting progress fires the listeners, which push to native.
                for (int i = 0; i < barDefaults.size(); i++) {
                    barDefaults.get(i).bar.setProgress(barDefaults.get(i).def);
                }
                for (int i = 0; i < 6; i++) {
                    if (outBars[i] != null) outBars[i].setProgress(outDefs[i]);
                }
                leiaView.setScaleX(1f); leiaView.setScaleY(1f);
                leiaView.setTranslationX(0); leiaView.setTranslationY(0);
            }
        });

        hide.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                boolean show = tabContent.getVisibility() != View.VISIBLE;
                tabContent.setVisibility(show ? View.VISIBLE : View.GONE);
                ((TextView) v).setText(show ? "HIDE" : "SHOW");
            }
        });
        return panel;
    }

    // ---- Debug panel tabs ---------------------------------------------------

    private void addTabButton(LinearLayout bar, String label) {
        TextView b = new TextView(this);
        b.setText(label);
        b.setTextSize(15);
        b.setTextColor(0xFF888888);
        b.setPadding(dp(14), dp(6), dp(14), dp(6));
        final int idx = tabButtons.size();
        b.setOnClickListener(v -> selectTab(idx));
        bar.addView(b);
        tabButtons.add(b);
    }

    private void selectTab(int idx) {
        for (int i = 0; i < tabViews.size(); i++) {
            tabViews.get(i).setVisibility(i == idx ? View.VISIBLE : View.GONE);
        }
        for (int i = 0; i < tabButtons.size(); i++) {
            tabButtons.get(i).setTextColor(i == idx ? 0xFF66FF99 : 0xFF888888);
        }
    }

    /** Creates (and appends) a new scrollable tab page and makes it current. */
    private LinearLayout beginTab() {
        android.widget.ScrollView sc = new android.widget.ScrollView(this);
        sc.setFillViewport(false);
        LinearLayout content = new LinearLayout(this);
        content.setOrientation(LinearLayout.VERTICAL);
        sc.addView(content, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT));
        int maxH = (int) (getResources().getDisplayMetrics().heightPixels * 0.42f);
        tabContent.addView(sc, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, Math.min(900, maxH)));
        tabViews.add(sc);
        controls = content;
        grid = new android.widget.GridLayout(this);
        grid.setColumnCount(2);
        grid.setOrientation(android.widget.GridLayout.HORIZONTAL);
        content.addView(grid);
        return content;
    }

    private void buildFxTabs() {
        // --- MXAO-style ambient occlusion ---
        beginTab();
        aoToggle = addFxToggle("MXAO ENABLE", 1, aoOn);
        addFxSlider("Intensity", 0, 300, 100, " %", 2);
        addFxSlider("Radius", 10, 500, 120, " u", 3);
        addFxSlider("Samples", 4, 32, 12, "", 4);
        addFxSlider("Bias", 0, 100, 20, " u", 5);
        addFxSlider("Power", 50, 400, 150, " %", 6);
        addFxDebugButton("SHOW AO", 1);

        // --- Cinematic DOF ---
        beginTab();
        dofToggle = addFxToggle("CINEMATIC DOF", 11, dofOn);
        addFxToggle("AUTO FOCUS", 16, true);   // tracks Mario by default
        addFxSlider("Focus", 50, 5000, 800, " u", 12);
        addFxSlider("Aperture", 0, 300, 100, " %", 13);
        addFxSlider("Max blur", 0, 40, 12, " px", 14);
        addFxSlider("Samples", 4, 32, 16, "", 15);
        addFxDebugButton("SHOW CoC", 2);

        // --- Screen-space GI (SSGI/RTGI-style) ---
        beginTab();
        giToggle = addFxToggle("SSGI ENABLE", 21, giOn);
        addFxSlider("Intensity", 0, 300, 100, " %", 22);
        addFxSlider("Radius", 10, 500, 150, " u", 23);
        addFxSlider("Steps", 2, 12, 6, "", 24);
        addFxSlider("Rays", 2, 8, 4, "", 25);
        addFxDebugButton("SHOW GI", 3);

        // --- Camera tracking / sensors ---
        beginTab();
        camHeadToggle = addCamToggle("HEAD TRACK", 41, hdtOn);
        camGyroToggle = addCamToggle("GYRO CAM", 42, gyroOn);
        addCamSlider("HDT strength", 0, 200, hdtStrength, " %", 43);
        addCamSlider("Gyro strength", 0, 200, gyroStrength, " %", 44);
        addCamSlider("Pivot height", 0, 200, gyroPivot, " cm", 45);
        addCamButton("RECENTRE GYRO", () -> recentreGyro());
    }

    // ---- camera-tracking widgets (stereo-int channel, keys 41..44) ----

    private void addCamSlider(String name, int min, int max, int initial, String unit,
                              final int key) {
        final TextView label = new TextView(this);
        label.setTextColor(Color.WHITE);
        label.setTextSize(14);
        label.setText(name + ": " + initial + unit);
        SeekBar bar = new SeekBar(this);
        bar.setMax(max - min);
        bar.setProgress(initial - min);
        bar.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override public void onProgressChanged(SeekBar sb, int progress, boolean fromUser) {
                int v = progress + min;
                label.setText(name + ": " + v + unit);
                nativeSetStereoInt(key, v);
                if (key == 43) hdtStrength = v;
                if (key == 44) gyroStrength = v;
                if (key == 45) gyroPivot = v;
            }
            @Override public void onStartTrackingTouch(SeekBar sb) {}
            @Override public void onStopTrackingTouch(SeekBar sb) {}
        });
        addToGrid(label, bar);
        nativeSetStereoInt(key, initial);
    }

    private TextView addCamToggle(String name, final int key, boolean initial) {
        final TextView b = new TextView(this);
        b.setText(name + ": " + (initial ? "ON" : "OFF"));
        b.setTextSize(16);
        b.setTextColor(initial ? 0xFF66FF99 : 0xFF888888);
        b.setPadding(dp(4), dp(8), dp(4), dp(8));
        b.setOnClickListener(v -> {
            boolean on = b.getCurrentTextColor() != 0xFF66FF99;
            b.setText(name + ": " + (on ? "ON" : "OFF"));
            b.setTextColor(on ? 0xFF66FF99 : 0xFF888888);
            nativeSetStereoInt(key, on ? 1 : 0);
            if (key == 41) hdtOn = on;
            if (key == 42) {
                gyroOn = on;
                if (on) gyroRefSet = false;   // recentre on enable
            }
            Log.i(TAG, name + "=" + on);
        });
        controls.addView(b);
        nativeSetStereoInt(key, initial ? 1 : 0);
        return b;
    }

    private void addCamButton(String name, final Runnable action) {
        TextView b = new TextView(this);
        b.setText(name);
        b.setTextSize(16);
        b.setTextColor(0xFF66CCFF);
        b.setPadding(dp(4), dp(10), dp(4), dp(10));
        b.setOnClickListener(v -> action.run());
        controls.addView(b);
    }

    private void recentreGyro() {
        gyroRefSet = false;
        nativeSetGyro(0f, 0f, 0f);
        Log.i(TAG, "gyro recentred");
    }

    // ---- sensors / tracker ----

    private static float wrapPi(float a) {
        while (a > (float) Math.PI) a -= (float) (2 * Math.PI);
        while (a < -(float) Math.PI) a += (float) (2 * Math.PI);
        return a;
    }

    private void startCameraTracking() {
        try {
            sensorManager = (SensorManager) getSystemService(SENSOR_SERVICE);
            Sensor s = sensorManager.getDefaultSensor(Sensor.TYPE_GAME_ROTATION_VECTOR);
            if (s == null) {
                s = sensorManager.getDefaultSensor(Sensor.TYPE_ROTATION_VECTOR);
            }
            if (s != null && sensorManager.registerListener(this, s, SensorManager.SENSOR_DELAY_GAME)) {
                gyroSensor = s;
                Log.i(TAG, "gyro sensor: " + s.getName() + " type=" + s.getType());
            } else {
                Log.w(TAG, "no rotation-vector sensor available");
            }
        } catch (Throwable t) {
            Log.w(TAG, "gyro sensor init failed", t);
        }

        // Head tracking: poll the Leia face tracker while HEAD TRACK is on.
        final android.os.Handler h = new android.os.Handler();
        h.post(new Runnable() {
            @Override public void run() {
                if (hdtOn && sdk != null) {
                    try {
                        boolean started = sdk.isFaceTrackingStarted();
                        com.leia.core.Vector3 f = started ? sdk.getPrimaryFace() : null;
                        if (f != null) {
                            nativeSetHeadPose(f.x, f.y, f.z, 1);
                        }
                        if ((++hdtLogCount % 60) == 0) {
                            Log.i(TAG, "head started=" + started + " face="
                                    + (f == null ? "null"
                                                 : (Math.round(f.x) + "," + Math.round(f.y) + ","
                                                    + Math.round(f.z))));
                        }
                    } catch (Throwable t) {
                        if ((++hdtLogCount % 60) == 0) Log.w(TAG, "head poll failed", t);
                    }
                }
                h.postDelayed(this, 16);
            }
        });
    }

    @Override
    public void onSensorChanged(SensorEvent e) {
        if (gyroSensor == null || e.sensor.getType() != gyroSensor.getType()) return;
        float[] r = new float[9];
        float[] o = new float[3];
        try {
            SensorManager.getRotationMatrixFromVector(r, e.values);
            SensorManager.getOrientation(r, o);
        } catch (Throwable t) {
            return;
        }
        if (!gyroRefSet || !gyroHaveSample) {
            System.arraycopy(o, 0, gyroRef, 0, 3);
            gyroRefSet = true;
            gyroHaveSample = true;
            return;   // first sample is the reference, no motion yet
        }
        float dAz = wrapPi(o[0] - gyroRef[0]);    // yaw about world up
        float dPitch = wrapPi(o[1] - gyroRef[1]); // tilt up/down
        float dRoll = wrapPi(o[2] - gyroRef[2]);  // twist

        // Axis signs: flip here if an axis moves the view the wrong way.
        try {
            nativeSetGyro(dPitch, -dAz, -dRoll);
        } catch (Throwable ignored) {
            // native library not loaded yet
        }
    }

    @Override
    public void onAccuracyChanged(Sensor sensor, int accuracy) {
    }

    /** FX slider: writes straight to the native post pass via nativeSetSSFX. */
    private void addFxSlider(String name, int min, int max, int initial, String unit,
                             final int key) {
        final TextView label = new TextView(this);
        label.setTextColor(Color.WHITE);
        label.setTextSize(14);
        label.setText(name + ": " + initial + unit);

        SeekBar bar = new SeekBar(this);
        bar.setMax(max - min);
        bar.setProgress(initial - min);
        bar.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override
            public void onProgressChanged(SeekBar seekBar, int progress, boolean fromUser) {
                int value = progress + min;
                label.setText(name + ": " + value + unit);
                nativeSetSSFX(key, value);
            }
            @Override public void onStartTrackingTouch(SeekBar seekBar) {}
            @Override public void onStopTrackingTouch(SeekBar seekBar) {}
        });
        barDefaults.add(new BarDefault(bar, initial - min));
        addToGrid(label, bar);
        nativeSetSSFX(key, initial);
    }

    private TextView addFxToggle(String name, final int key, boolean initial) {
        final TextView b = new TextView(this);
        b.setText(name + ": " + (initial ? "ON" : "OFF"));
        b.setTextSize(16);
        b.setTextColor(initial ? 0xFF66FF99 : 0xFF888888);
        b.setPadding(dp(4), dp(8), dp(4), dp(8));
        b.setOnClickListener(v -> {
            boolean on = b.getCurrentTextColor() != 0xFF66FF99;
            applyFxToggle(b, name, key, on);
        });
        controls.addView(b);
        nativeSetSSFX(key, initial ? 1 : 0);
        return b;
    }

    private void applyFxToggle(TextView b, String name, int key, boolean on) {
        b.setText(name + ": " + (on ? "ON" : "OFF"));
        b.setTextColor(on ? 0xFF66FF99 : 0xFF888888);
        nativeSetSSFX(key, on ? 1 : 0);
        if (key == 1) aoOn = on;
        else if (key == 11) dofOn = on;
        else if (key == 21) giOn = on;
        Log.i(TAG, "FX toggle " + name + "=" + (on ? 1 : 0));
    }

    private void addFxDebugButton(String name, final int mode) {
        TextView b = new TextView(this);
        b.setText(name);
        b.setTextSize(14);
        b.setTextColor(0xFF888888);
        b.setPadding(dp(4), dp(6), dp(4), dp(6));
        b.setOnClickListener(v -> setFxDebug(mode));
        controls.addView(b);
        fxDebugButtons.add(b);
        fxDebugModes.add(mode);
    }

    private void setFxDebug(int mode) {
        fxDebug = (fxDebug == mode) ? 0 : mode;
        nativeSetSSFX(31, fxDebug);
        for (int i = 0; i < fxDebugButtons.size(); i++) {
            boolean active = fxDebugModes.get(i) == fxDebug;
            fxDebugButtons.get(i).setTextColor(active ? 0xFFFFAA33 : 0xFF888888);
        }
        Log.i(TAG, "fxDebug=" + fxDebug);
    }

    /** Opacity of the floating touch-control overlay (percent). */
    private void applyOverlayAlpha(int pct) {
        overlayAlphaPct = pct;
        if (touchOverlay != null) {
            touchOverlay.setOverlayAlpha(pct / 100f);
        }
        Log.i(TAG, "overlay alpha " + pct);
    }

    /** Output-layer transform knobs. kind: 0=H% 1=V% 2=Wpx 3=Hshift 4=Vshift */
    private void addOutSlider(String name, int min, int max, int initial, final int kind) {
        final TextView label = new TextView(this);
        label.setTextColor(Color.WHITE);
        label.setTextSize(14);
        label.setText(name + ": " + initial);

        SeekBar bar = new SeekBar(this);
        bar.setMax(max - min);
        bar.setProgress(initial - min);
        bar.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override
            public void onProgressChanged(SeekBar seekBar, int progress, boolean fromUser) {
                int value = progress + min;
                label.setText(name + ": " + value);
                outVal[kind] = value;
                applyOutTransform();
            }

            @Override public void onStartTrackingTouch(SeekBar seekBar) {}
            @Override public void onStopTrackingTouch(SeekBar seekBar) {}
        });
        outVal[kind] = initial;
        outBars[kind] = bar;
        outDefs[kind] = initial - min;
        barDefaults.add(new BarDefault(bar, initial - min));
        addToGrid(label, bar);
    }

    /** OUT aspect: letterbox the woven output layer (0 = off). */
    private void applyOutAspect(int pct) {
        if (leiaView == null || leiaView.getWidth() == 0) return;
        float viewAr = (float) leiaView.getWidth() / (float) leiaView.getHeight();
        if (pct <= 0) { leiaView.setScaleX(1f); leiaView.setScaleY(1f); return; }
        float want = pct / 100f;
        if (want > viewAr) { leiaView.setScaleX(1f); leiaView.setScaleY(viewAr / want); }
        else { leiaView.setScaleY(1f); leiaView.setScaleX(want / viewAr); }
        Log.i(TAG, "OUT_AR " + pct + " viewAr=" + viewAr);
    }

    private void applyOutTransform() {
        View v = testOn && testView != null ? testView : leiaView;
        if (v == null) return;
        v.setPivotX(0);
        v.setPivotY(0);
        float sx = outVal[0] / 100f;
        v.setScaleX(sx);
        v.setScaleY(outVal[5] > 0 ? outVal[5] / 100f : outVal[1] / 100f);
        v.setTranslationX(v.getWidth() > 0 ? outVal[3] / 100f * v.getWidth() : 0);
        v.setTranslationY(outVal[4]);
    }

    private void addSlider(String name, int min, int max, int initial, String unit,
                           final int which) {
        final TextView label = new TextView(this);
        label.setTextColor(Color.WHITE);
        label.setTextSize(14);
        label.setText(name + ": " + initial + unit);

        SeekBar bar = new SeekBar(this);
        bar.setMax(max - min);
        bar.setProgress(initial - min);
        bar.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override
            public void onProgressChanged(SeekBar seekBar, int progress, boolean fromUser) {
                int value = progress + min;
                label.setText(name + ": " + value + unit);
                Log.i(TAG, "knob " + name + "=" + value);
                if (which == 11) {
                    applySourceWidthPercent(value);
                } else if (which >= 8) {
                    nativeSetStereoInt(which, value);
                } else {
                    nativeSetStereo(which, value);
                }
            }

            @Override public void onStartTrackingTouch(SeekBar seekBar) {}
            @Override public void onStopTrackingTouch(SeekBar seekBar) {}
        });
        if (which >= 0 && which < bars.length) bars[which] = bar;
        barDefaults.add(new BarDefault(bar, initial - min));
        addToGrid(label, bar);
    }

    private static final class BarDefault {
        final SeekBar bar; final int def;
        BarDefault(SeekBar b, int d) { bar = b; def = d; }
    }
    private final java.util.List<BarDefault> barDefaults = new java.util.ArrayList<>();

    private void addIntSlider(String name, int min, int max, int initial,
                              final java.util.function.IntConsumer sink) {
        final TextView label = new TextView(this);
        label.setTextColor(Color.WHITE);
        label.setTextSize(14);
        label.setText(name + ": " + initial);
        SeekBar bar = new SeekBar(this);
        bar.setMax(max - min);
        bar.setProgress(initial - min);
        bar.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override public void onProgressChanged(SeekBar sb, int progress, boolean fromUser) {
                int value = progress + min;
                label.setText(name + ": " + value);
                sink.accept(value);
            }
            @Override public void onStartTrackingTouch(SeekBar sb) {}
            @Override public void onStopTrackingTouch(SeekBar sb) {}
        });
        addToGrid(label, bar);
    }

    private void addToGrid(TextView label, SeekBar bar) {
        android.widget.LinearLayout col = new android.widget.LinearLayout(this);
        col.setOrientation(LinearLayout.VERTICAL);
        col.setPadding(0, 0, dp(8), 0);
        col.addView(label);
        col.addView(bar);
        if (grid != null) {
            android.widget.GridLayout.LayoutParams lp = new android.widget.GridLayout.LayoutParams();
            lp.width = 0;
            lp.columnSpec = android.widget.GridLayout.spec(
                    android.widget.GridLayout.UNDEFINED, 1f);
            grid.addView(col, lp);
        } else {
            controls.addView(col);
        }
    }

    private android.widget.AbsoluteLayout.LayoutParams abs(int x, int y, int w, int h) {
        return new android.widget.AbsoluteLayout.LayoutParams(w, h, x, y);
    }

    private int dp(int v) {
        return (int) (v * getResources().getDisplayMetrics().density);
    }

    /** Feeds a known-good red|blue SBS bitmap through the same Leia asset path. */
    private boolean surfaceValid = false;

    private void resizeLeiaSurface(int w, int h) {
        if (w <= 0 || h <= 0 || asset == null) {
            Log.i(TAG, "resize skipped w=" + w + " h=" + h + " asset=" + (asset != null));
            return;
        }
        if (!surfaceValid) {
            Log.i(TAG, "CreateEmptySurfaceForVideo " + w + "x" + h);
            try {
                // Video asset = the binding that CONSUMES a SurfaceTexture (the
                // picture asset samples its own texture id instead, which needs
                // rendering inside Leia's GL context).
                asset.CreateEmptySurfaceForVideo(w, h, new SurfaceTextureReadyCallback() {
                    @Override
                    public void onSurfaceTextureReady(SurfaceTexture texture) {
                        texture.setDefaultBufferSize(w, h);
                        Log.i(TAG, "VIDEO_SURFACE_CREATED " + w + "x" + h + " " + texture);
                        SDLActivity.setOverrideSurface(new Surface(texture));
                        // mpv's Enable3D(): backlight and tracking together,
                        // once the video surface exists.
                        try {
                            sdk.enableBacklight(true);
                            sdk.enableFaceTracking(true);
                            Log.i(TAG, "ENABLE3D bl=" + sdk.getBacklight()
                                    + " ft=" + sdk.isFaceTrackingEnabled());
                        } catch (Throwable t) {
                            Log.e(TAG, "enable3d", t);
                        }
                        Log.i(TAG, "SDL_OVERRIDE_SET");
                    }
                });
                surfaceValid = true;
            } catch (Throwable t) {
                Log.e(TAG, "CreateEmptySurfaceForPicture failed", t);
            }
        } else if (leiaTexture != null) {
            leiaTexture.setDefaultBufferSize(w, h);
        }
    }

    private static final int[][] SRC_SIZES = {
            {2560, 1454}, {3840, 1200}, {2560, 1600}, {5120, 3200}, {1280, 720}
    };
    private int srcIndex = 0;  // 2560x1454 first
    private volatile int srcW = 3840;

    private void applySourceSize() {
        int[] sz = SRC_SIZES[srcIndex];
        srcW = sz[0]; srcH = sz[1];
        Log.i(TAG, "sourceSize -> " + sz[0] + "x" + sz[1] + " (informational)");
    }

    private void showStaticTestImage() {
        int w = 1920, h = 1200;
        android.graphics.Bitmap bmp = android.graphics.Bitmap.createBitmap(
                w * 2, h, android.graphics.Bitmap.Config.ARGB_8888);
        android.graphics.Canvas c = new android.graphics.Canvas(bmp);
        android.graphics.Paint p = new android.graphics.Paint();
        p.setColor(0xFFCC2222);
        c.drawRect(0, 0, w, h, p);
        p.setColor(0xFF2222CC);
        c.drawRect(w, 0, w * 2, h, p);
        p.setColor(0xFFFFFFFF);
        p.setTextSize(400);
        c.drawText("L", 700, 700, p);
        c.drawText("R", w + 700, 700, p);
        if (testView != null) {
            testView.setVisibility(testOn ? View.GONE : View.VISIBLE);
        }
        testOn = !testOn;
        Log.i(TAG, "static SBS test image " + (testOn ? "on" : "off"));
    }

    private void setStereoMode(boolean on) {
        stereoOn = on;
        nativeSetStereoMode(1);          // keep the stereo render path either way
        nativeSetStereoInt(21, on ? 0 : 1);   // 2D = zero parallax, keep FOV/aspect
        if (modeButton != null) {
            modeButton.setText(on ? "3D" : "2D");
            modeButton.setTextColor(on ? 0xFF66FF99 : 0xFFFF9966);
        }
        // Backlight + single-view must follow, otherwise the panel keeps
        // weaving a frame that is no longer side-by-side.
        if (sdk != null) {
            sdk.enableBacklight(on);
        }
        if (leiaView != null) {
            leiaView.setSingleViewMode(!on);
        }
        Log.i(TAG, "stereo mode " + (on ? "3D" : "2D"));
    }

    private void startDebugReadout(LinearLayout panel) {
        debugView = new TextView(this);
        debugView.setTextColor(0xFFAAFFAA);
        debugView.setTextSize(12);
        panel.addView(debugView);
        final android.os.Handler h = new android.os.Handler();
        h.post(new Runnable() {
            @Override
            public void run() {
                if (fpsView != null) {
                    fpsView.setText(nativeGetFps() + " fps"
                            + "  bl=" + (sdk != null && sdk.getBacklight())
                            + " ft=" + (sdk != null && sdk.isFaceTrackingStarted()));
                }
                debugView.setTextColor(busyOn ? 0xFFFF5555 : 0xFFAAFFAA);
                debugView.setText((busyOn ? "BUSY - hands off  |  " : "FREE  |  ")
                        + BUILD_NAME + " drawable " + nativeGetDrawableWidth() + "x"
                        + nativeGetDrawableHeight()
                        + "  mode=" + (stereoOn ? "3D" : "2D")
                        + "  ipd=" + (int) bars[0].getProgress()
                        + " conv=" + (bars[1].getProgress() - 300)
                        + " depth=" + bars[2].getProgress() + "%"
                        + " src=" + SRC_SIZES[srcIndex][0] + "x" + SRC_SIZES[srcIndex][1]
                        + " eyeW=" + nativeGetEyeWidth() + " st=" + nativeGetStereoEnabled()
                        + " sx=" + (testOn && testView != null ? testView.getScaleX() : leiaView.getScaleX())
                        + " srcW=" + srcW
                        + " vp0x=" + nativeGetVpX(0) + "/" + nativeGetVpW(0)
                        + " vp1x=" + nativeGetVpX(1) + "/" + nativeGetVpW(1));
                h.postDelayed(this, 500);
            }
        });
    }

    /** which: 0 = IPD px, 1 = convergence px, 2 = depth percent. */
    private native void nativeSetStereo(int which, int value);
    private native void nativeSetStereoMode(int enabled);
    private native void nativeSetStereoInt(int which, int value);

    /** Screen-space FX knobs, implemented by the per-eye post pass. */
    private native void nativeSetSSFX(int which, int value);
    private native int nativeGetSSFX(int which);

    /** 11 = interlacer source width as a percentage of the panel width. */
    private volatile int srcH = 1200;

    private void reapplySourceSize() {
        Log.i(TAG, "sourceSize (informational) " + srcW + "x" + srcH);
    }

    private void applySourceWidthPercent(int pct) {
        // mpv AAR has no sourceSize accessor; geometry is set by the asset.
        srcW = 3840 * pct / 100;
        srcH = 1200;
        Log.i(TAG, "sourceWidth% -> " + pct + " (informational)");
    }
    private native int nativeGetEyeWidth();
    private native int nativeGetFps();

    static native void nativeOverlayInput(int active, int sx, int sy, int buttons);
    static native void nativeSetFpsMode(int mode);
    static native int nativeGetFpsMode();
    static native void nativeSetHfrInterp(int on);
    static native int nativeGetHfrInterp();
    static native int nativeGetHfrMtxCount();
    static native int nativeGetCamMode();
    static native void nativeToggleCamMode();
    static native void nativeSetHeadPose(float x, float y, float z, int valid);
    static native void nativeSetGyro(float pitch, float yaw, float roll);
    static native int nativeGetHeadPose(int axis);
    static native void nativeShootPortal(int which);
    static native int nativeGetPortalGun();
    static native int nativeGetPortalCount();
    static native int nativeGetPortalDebug();
    static native void nativeGivePortalGun();
    static native void nativeOverlayStick2(int rx, int ry);
    static native void nativeOverlayAnalog(int on);
    static native void nativeHideInGameControls(int hide);
    private native int nativeGetVpX(int eye);
    private native int nativeGetVpW(int eye);
    private native int nativeGetStereoEnabled();
    private native int nativeGetDrawableWidth();
    private native int nativeGetDrawableHeight();

    @Override
    protected void onResume() {
        super.onResume();
        if (leiaView != null) leiaView.onResume();
        if (sdk != null) {
            sdk.onResume();
            // The system forces 2D backlight while the screen is off; put the
            // panel back into 3D mode after wake.
            sdk.enableBacklight(stereoOn);
        }
        if (leiaView != null) reapplySourceSize();
        applyPanelRefresh(fpsMode == 0 ? 30 : fpsMode == 1 ? 60 : 120);
    }

    @Override
    protected void onPause() {
        if (leiaView != null) leiaView.onPause();
        if (sdk != null) {
            sdk.onPause();
            // Leave the panel in a sane 2D state while we are in background.
            sdk.enableBacklight(false);
        }
        super.onPause();
    }

    // ---- runtime debug control (see DebugReceiver) -------------------------
    /** Red debug line + notice while the assistant is driving the app. */
    void debugSetBusy(int on) {
        busyOn = on != 0;
        runOnUiThread(() -> {
            if (debugView != null) {
                debugView.setTextColor(busyOn ? 0xFFFF5555 : 0xFFAAFFAA);
            }
        });
        Log.i(TAG, "BUSY " + busyOn);
    }

    void debugSet(int k, int v) {
        if (k < 0) return;
        if (k == 39) {
            applyOverlayAlpha(v);
        } else if (k == 11) {
            applySourceWidthPercent(v);
        } else if (k >= 8) {
            nativeSetStereoInt(k, v);
        } else {
            nativeSetStereo(k, v);
        }
    }

    void debugPortal(int which) {
        nativeShootPortal(which);
        Log.i(TAG, "PORTAL shoot " + which + " count=" + nativeGetPortalCount());
    }

    void debugGiveGun() {
        nativeGivePortalGun();
        Log.i(TAG, "PORTAL gun given");
    }

    void debugFx(int k, int v) {
        nativeSetSSFX(k, v);
        Log.i(TAG, "FX set " + k + "=" + v);
    }

    void debugToggle(String name) {
        if (name == null) return;
        switch (name) {
            case "MXAO":
                runOnUiThread(() -> {
                    aoOn = !aoOn;
                    if (aoToggle != null) applyFxToggle(aoToggle, "MXAO ENABLE", 1, aoOn);
                    else nativeSetSSFX(1, aoOn ? 1 : 0);
                });
                break;
            case "CDOF":
                runOnUiThread(() -> {
                    dofOn = !dofOn;
                    if (dofToggle != null) applyFxToggle(dofToggle, "CINEMATIC DOF", 11, dofOn);
                    else nativeSetSSFX(11, dofOn ? 1 : 0);
                });
                break;
            case "SSGI":
                runOnUiThread(() -> {
                    giOn = !giOn;
                    if (giToggle != null) applyFxToggle(giToggle, "SSGI ENABLE", 21, giOn);
                    else nativeSetSSFX(21, giOn ? 1 : 0);
                });
                break;
            case "FXDBG":
                runOnUiThread(() -> {
                    fxDebug = (fxDebug + 1) % 4;
                    nativeSetSSFX(31, fxDebug);
                    Log.i(TAG, "fxDebug=" + fxDebug);
                });
                break;
            case "TINT": nativeSetStereoInt(7, tintOn ? 0 : 1); tintOn = !tintOn; break;
            case "ZBUF": nativeSetStereoInt(15, zbufOn ? 0 : 1); zbufOn = !zbufOn; break;
            case "WIRE": nativeSetStereoInt(16, wireOn ? 0 : 1); wireOn = !wireOn; break;
            case "PERF2x": nativeSetStereoInt(12, perfOn ? 0 : 1); perfOn = !perfOn; break;
            case "SWAP": nativeSetStereoInt(4, swapOn ? 0 : 1); swapOn = !swapOn; break;
            case "SAME": nativeSetStereoInt(6, sameOn ? 0 : 1); sameOn = !sameOn; break;
            case "NOCULL": nativeSetStereoInt(19, nocullOn ? 0 : 1); nocullOn = !nocullOn; break;
            case "ANALOG":
                analogOn = !analogOn;
                if (touchOverlay != null) touchOverlay.setAnalogMode(analogOn);
                break;
            case "GIVEGUN":
                runOnUiThread(this::debugGiveGun);
                break;
            case "HEADTRACK":
                runOnUiThread(() -> {
                    hdtOn = !hdtOn;
                    nativeSetStereoInt(41, hdtOn ? 1 : 0);
                    Log.i(TAG, "HEADTRACK=" + hdtOn);
                });
                break;
            case "GYRO":
                runOnUiThread(() -> {
                    gyroOn = !gyroOn;
                    nativeSetStereoInt(42, gyroOn ? 1 : 0);
                    if (gyroOn) gyroRefSet = false;
                    Log.i(TAG, "GYRO=" + gyroOn);
                });
                break;
            case "GYROZERO":
                runOnUiThread(this::recentreGyro);
                break;
            case "TOEAUTO":
                toeAutoOn = !toeAutoOn;
                nativeSetStereoInt(40, toeAutoOn ? 1 : 0);
                Log.i(TAG, "TOEAUTO=" + toeAutoOn);
                break;
            case "CAM":
                nativeToggleCamMode();
                Log.i(TAG, "CAM toggle -> " + nativeGetCamMode());
                break;
            case "STAY":
                stayOn = !stayOn;
                nativeSetStereoInt(30, stayOn ? 1 : 0);
                Log.i(TAG, "STAY=" + stayOn);
                break;
            case "UIPOST":
                uiPostOn = !uiPostOn;
                nativeSetStereoInt(35, uiPostOn ? 1 : 0);
                Log.i(TAG, "UIPOST=" + uiPostOn);
                break;
            case "UIPARALLAX":
                uiParallaxOn = !uiParallaxOn;
                nativeSetStereoInt(36, uiParallaxOn ? 1 : 0);
                Log.i(TAG, "UIPARALLAX=" + uiParallaxOn);
                break;
            case "UIBUF":
                uiBufOn = !uiBufOn;
                nativeSetStereoInt(37, uiBufOn ? 1 : 0);
                Log.i(TAG, "UIBUF=" + uiBufOn);
                break;
            case "FLAT":
                runOnUiThread(() -> {
                    flatOn = !flatOn;
                    nativeSetStereoInt(21, flatOn ? 1 : 0);   // zero parallax
                    Log.i(TAG, "FLAT zeroParallax=" + flatOn);
                });
                break;
            case "INTERP":
                runOnUiThread(() -> {
                    interpOn = !interpOn;
                    nativeSetHfrInterp(interpOn ? 1 : 0);
                    Log.i(TAG, "INTERP " + interpOn);
                });
                break;
            case "3D": runOnUiThread(() -> setStereoMode(!stereoOn)); break;
            case "TEST": runOnUiThread(this::showStaticTestImage); break;
            default: Log.w(TAG, "unknown toggle " + name);
        }
    }

    void debugSetFps(int mode) {
        runOnUiThread(() -> {
            fpsMode = mode & 3;
            nativeSetFpsMode(fpsMode);
            if (fpsButton != null) {
                fpsButton.setText("FPS " + (fpsMode == 0 ? "30" : fpsMode == 1 ? "60"
                        : fpsMode == 2 ? "120" : "MAX"));
            }
            Log.i(TAG, "FPS_MODE(set) " + fpsMode);
        });
    }

    void debugReinit() {
        runOnUiThread(() -> {
            surfaceValid = false;
            leiaTexture = null;
            Log.i(TAG, "REINIT requested");
            resizeLeiaSurface(leiaView.getWidth(), leiaView.getHeight());
        });
    }

    void debugReset() {
        runOnUiThread(() -> {
            if (!barDefaults.isEmpty()) {
                for (int i = 0; i < barDefaults.size(); i++) {
                    barDefaults.get(i).bar.setProgress(barDefaults.get(i).def);
                }
            }
            Log.i(TAG, "RESET applied");
        });
    }

    void debugDump() {
        runOnUiThread(() -> {
            Log.i(TAG, "DUMP drawable=" + nativeGetDrawableWidth() + "x" + nativeGetDrawableHeight()
                    + " eyeW=" + nativeGetEyeWidth() + " st=" + nativeGetStereoEnabled()
                    + " fps=" + nativeGetFps()
                    + " view=" + leiaView.getWidth() + "x" + leiaView.getHeight()
                    + " surfaceValid=" + surfaceValid
                    + " stereoOn=" + stereoOn
                    + " tint=" + tintOn + " zbuf=" + zbufOn + " wire=" + wireOn
                    + " perf=" + perfOn + " swap=" + swapOn + " same=" + sameOn
                    + " nocull=" + nocullOn
                        + " hfr=" + nativeGetHfrMtxCount() + "/" + nativeGetHfrInterp()
                    + " mxao=" + nativeGetSSFX(1) + " cdof=" + nativeGetSSFX(11)
                    + " ssgi=" + nativeGetSSFX(21) + " fxdbg=" + nativeGetSSFX(31)
                    + " dofauto=" + nativeGetSSFX(16) + " dofz=" + nativeGetSSFX(17)
                    + " stay=" + stayOn + " uipost=" + uiPostOn
                    + " cam=" + nativeGetCamMode() + " ctlA=" + overlayAlphaPct
                    + " toeauto=" + toeAutoOn
                    + " gun=" + nativeGetPortalGun() + " portals=" + nativeGetPortalCount()
                    + " pdbg=" + nativeGetPortalDebug()
                    + " hdt=" + (hdtOn ? 1 : 0) + "/" + hdtStrength
                    + " gyro=" + (gyroOn ? 1 : 0) + "/" + gyroStrength
                    + " pivot=" + gyroPivot
                    + " head=" + nativeGetHeadPose(0) + "," + nativeGetHeadPose(1)
                    + "," + nativeGetHeadPose(2)
                    + " uipar=" + uiParallaxOn + " uibuf=" + uiBufOn);
        });
    }

    @Override
    protected void onDestroy() {
        if (instance == this) instance = null;
        // asset lifecycle handled by the SDK (no close() in this AAR)
        LeiaSDK.shutdownSDK();
        super.onDestroy();
    }
}

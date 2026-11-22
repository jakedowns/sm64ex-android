package com.vdavid003.sm64port;

import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.hardware.Sensor;
import android.hardware.SensorEvent;
import android.hardware.SensorEventListener;
import android.hardware.SensorManager;
import android.content.Intent;
import android.content.pm.PackageInfo;
import android.graphics.SurfaceTexture;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.SystemClock;
import android.text.Html;
import android.text.method.LinkMovementMethod;
import android.util.DisplayMetrics;
import android.util.Log;
import android.view.Display;
import android.view.Gravity;
import android.view.Surface;
import android.view.WindowManager;
import android.view.View;
import android.view.ViewGroup;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.RelativeLayout;
import android.widget.SeekBar;
import android.widget.TextView;
import android.widget.Toast;

import java.io.File;
import java.io.FileInputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.Locale;

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
    private static final String BUILD_NAME = "v1.0-alpha3";
    private static final String PREFS_NAME = "sm64port";
    private static final String PREF_OVERLAY_OPACITY = "overlay_opacity";
    private static final int REQ_EXPORT_LOG = 0x5A64;
    public static volatile sm64portActivity instance;
    private long leiaT0;
    private TextView debugLogInfoView;
    private boolean exportInProgress;
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
    /* Any slider key -> its SeekBar/min, for gesture control (cam keys > 23). */
    private final java.util.Map<Integer, SeekBar> keyBars = new java.util.HashMap<>();
    private final java.util.Map<Integer, Integer> keyMins = new java.util.HashMap<>();
    private final SeekBar[] outBars = new SeekBar[6];
    private final int[] outDefs = new int[6];
    private final int[] outVal = new int[6];
    private android.widget.GridLayout grid;
    private TextView modeButton;
    private TextView fpsView;
    private TextView fpsButton;
    private TextView interpButton;
    private TouchOverlay touchOverlay;
    /** Debug/trailer: keep touch circles + tuning panel hidden regardless of input. */
    private boolean forceHudHidden = false;
    private android.widget.ScrollView scrollerRef;
    private TextView debugView;
    private LinearLayout tabContent;
    private final java.util.List<View> tabViews = new java.util.ArrayList<>();
    private final java.util.List<TextView> tabButtons = new java.util.ArrayList<>();
    private final java.util.List<Integer> topTabPages = new java.util.ArrayList<>();
    private final java.util.Map<Integer, Integer> stereoValues = new java.util.HashMap<>();
    private StereoPictogramView stereoPictogram;
    /* The pictogram shows live engine values (cam->Mario distance, focus,
     * toe), so it needs continuous redraws: it only used to invalidate on
     * slider/menu events, which made the read-out look static. */
    private final android.os.Handler pictoHandler =
            new android.os.Handler(android.os.Looper.getMainLooper());
    private final Runnable pictoRefresh = new Runnable() {
        @Override public void run() {
            if (stereoPictogram != null && stereoPictogram.getVisibility() == View.VISIBLE) {
                stereoPictogram.invalidate();
                pictoHandler.postDelayed(this, 100);
            }
        }
    };
    private LinearLayout effectsSubBar;
    private final java.util.List<TextView> effectsSubButtons = new java.util.ArrayList<>();
    private static final int EFFECTS_TOP_INDEX = 1;
    private static final int[] EFFECT_PAGES = {1, 2, 3, 4, 5, 8, 9};
    private static final String[] EFFECT_LABELS = {"GENERIC", "MXAO", "C.DOF", "SSGI", "SSRTX", "SHADOW", "SPLAT"};
    private int lastEffectPage = 1;
    private final java.util.List<TextView> fxDebugButtons = new java.util.ArrayList<>();
    private final java.util.List<Integer> fxDebugModes = new java.util.ArrayList<>();
    private TextView aoToggle, dofToggle, giToggle;
    private TextView camCycleButton;
    private TextView portalGunButton;
    private TextView pauseLoopToggle;
    private boolean hardPaused;
    private boolean ballBlasterOn;
    private boolean aoOn, dofOn, giOn;
    private int fxDebug = 0;
    // SSRTX screen-space ray-traced reflections (opt-in: expensive).
    private boolean ssrtxOn = false;
    private TextView ssrtxToggle;
    // Gameplay enhancements (default on; toggled from the ENH tab).
    private boolean ragdollOn = true, hitRagdollOn = true, ppcamOn = true;
    private TextView ragdollToggle, hitRagdollToggle, ppcamToggle;
    // Portal Test Chamber + uncapped test execution (debug tooling only).
    private boolean uncappedTestOn = false;
    private TextView uncappedTestToggle;
    // Realtime shadow map (SHADOW tab).
    private int shadowRes = 1024;
    private int shadowDebug = 0;
    private TextView shadowResButton, shadowDebugButton, shadowEnableToggle;
    private boolean shadowOn = false;   // dynamic shadows off by default
    private boolean stereoOn = true;
    private boolean testOn = false;
    private boolean tintOn, zbufOn, wireOn, perfOn, swapOn, sameOn, nocullOn, flatOn, analogOn;
    private int splatMode;  // 0 off, 1 Mario, 2 trees, 3 all objects
    private int splatShape; // 0 radial gradient, 1 ragged paint strokes
    private TextView splatTargetButton;
    private TextView splatShapeButton;
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
    private int overlayAlphaPct = 33;
    // true = the assistant is driving the app: debug line turns red.
    private boolean busyOn;
    private final java.util.List<TextView> fitButtons = new java.util.ArrayList<>();
    private int fitMode = 0;
    private int modeIndex = 0;
    private int refreshIdx = 0;

    /** Cycle the panel refresh rate: default -> 60 -> 120 Hz. */
    private int fpsMode = 2;   // 120 Hz render rate by default
    private boolean interpOn = true;
    // Debug panel: slider drag mode, tab styling, changelog.
    private LinearLayout tuningPanelView;
    private final java.util.List<View> sliderRows = new java.util.ArrayList<>();
    private TextView hideButton;
    private int selectedTab = 0;
    private int selectedPage = 0;
    private LinearLayout enhColumnTarget;
    private SeekBar toeSlider;
    private TextView toeLabel;
    private int toeValuePct = 0;
    private TextView camStatusView;
    private int sameMode = 0;
    private int camMode2 = 0;   // 0 vanilla, 1 bettercam, 2 puppycam, 3 custom, 4 first person
    private TextView[] camModePills = new TextView[5];
    private float panelDownY = -1;
    private static final int PANEL_BG = 0xCC101010;
    private static final String CHANGELOG =
            "v1.0-alpha3 - what is new\n"
          + "Portal gun: spinning pickup, held decimated model, LB/RB fire,\n"
          + "  ENH EQUIP/DROP toggle, P1/P2 touch buttons, portal sounds.\n"
          + "Aim reticle: now a small thin crosshair at the real raycast hit\n"
          + "  point in the world, so its stereo depth is the surface depth.\n"
          + "Portal Test Chamber (ENH): momentum launch + second debug star,\n"
          + "  through-portal view on by default, smooth portal camera blend,\n"
          + "  per-object occluder fade (only the blocking platform/wall fades).\n"
          + "Movement kit (ENH): Titanfall-style wall-run, grapple and a fuel\n"
          + "  jetpack, with fuel-tank and refill-pump upgrades. L = use.\n"
          + "PTC annex: widened chamber with a 30-cube Box3D pyramid and a\n"
          + "  ball blaster (ENH) that fires physics spheres (Z to fire);\n"
          + "  punches/kicks shove cubes, Mario lands and stands on upward\n"
          + "  cube faces, cube and platform faces use height gradients.\n"
          + "  A green reset pad near the pyramid rebuilds the stack. Jump\n"
          + "  on it. SPLAT EVERYTHING now includes the chamber geometry.\n"
          + "  Occluding platforms fade smoothly, and the on-screen analog\n"
          + "  C-stick rotates the vanilla camera too.\n"
          + "Splat: opacity/softness now apply (blending fix), and a Density\n"
          + "  slider fills large wall/floor triangles with interpolated points;\n"
          + "  a SHAPE toggle swaps the radial dab for ragged directional\n"
          + "  paint-stroke brushes (three procedural variants).\n"
          + "Stereo defaults: IPD 300 px and depth 50% (previously 0 px / 10%).\n"
          + "Portal POV: corrected the portal camera transform (column-vector\n"
          + "  matrices) and added flat-mode overlay + visibility selection;\n"
          + "  the Leia composite pass still needs the overlay wired in.\n"
          + "Splat prototype: point-sprite rendering for Mario/trees/all\n"
          + "  objects (SPLAT toggle or adb); merged from feat/splat-prototype.\n"
          + "Camera: CAMERA-tab distance/height affect vanilla and custom;\n"
          + "  custom orbit Y-invert; FPV X-invert; FOV up to 400%.\n"
          + "  C-up twice enters first person; C-up or Select/Back exits;\n"
          + "  no standard camera SFX in FPV; TOEAUTO raycasts forward there.\n"
          + "Camera gestures (on by default, CAMERA tab): two-finger pinch = FOV,\n"
          + "  two-finger drag Y = camera distance, drag X = camera height;\n"
          + "  ignores fingers spread across both stick clusters.\n"
          + "Stereo: Aspect % in 0.1 steps, default 83.4% with a live ratio\n"
          + "  label; 3D width/height calibration and a magenta ASPECT TEST\n"
          + "  square; 3D defaults 50% width / 100% height for the Leia panel.\n"
          + "Debug: inline -/+ buttons on every slider; ASPECT TEST; branch\n"
          + "  name shown in About, Changelog and the bottom status line.\n"
          + "Existing: 120 Hz interpolated rendering (30 Hz sim), UI after\n"
          + "  post, MXAO/DOF/SSGI/SSRTX/shadows, head-track + gyro, free cam,\n"
          + "  stay in level, level select, cheats, log export, about/credits.";

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
        // Idempotent: normally installed by Sm64Application before this runs.
        DebugLog.install(getApplicationContext());
        leiaT0 = SystemClock.uptimeMillis();
        weaverLog("onCreate after SDLActivity; " + DebugLog.oneLineSummary(this));

        // UI preferences (SharedPreferences persists across launches).
        try {
            overlayAlphaPct = getSharedPreferences(PREFS_NAME, MODE_PRIVATE)
                    .getInt(PREF_OVERLAY_OPACITY, 33);
        } catch (Throwable t) {
            overlayAlphaPct = 33;
        }
        if (overlayAlphaPct < 0) overlayAlphaPct = 0;
        if (overlayAlphaPct > 100) overlayAlphaPct = 100;
        Log.i(TAG, "overlay opacity restored: " + overlayAlphaPct + "%");

        try {
            initLeia();
        } catch (Throwable t) {
            // Never brick the game: fall back to SDL's normal flat output.
            weaverError("Leia init FAILED, staying flat (possible black screen cause)", t);
            SDLActivity.failOverride();
        }
    }

    private void initLeia() throws Exception {
        weaverLog("initLeia begin");
        logWeaverEnvironment();

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
        } catch (Throwable ignored) {
        }
        weaverLog("InitArgs ready: enableFaceTracking=true permissionCheck=false logLevel=Error");

        weaverLog("LeiaSDK.createSDK() begin");
        sdk = LeiaSDK.createSDK(args);
        weaverLog("LeiaSDK.createSDK() returned; initialized=" + sdk.isInitialized()
                + " faceTrackingInService=" + LeiaSDK.isFaceTrackingInService());
        sdk.enableBacklight(true);
        try {
            weaverLog("enableBacklight(true) -> " + sdk.getBacklight());
        } catch (Throwable t) {
            weaverLog("enableBacklight(true) done (getBacklight failed: " + t + ")");
        }
        startCameraTracking();
        weaverLog("startCameraTracking done");

        // Make SDL wait for our surface rather than grabbing its own.
        SDLActivity.mWaitForOverride = true;
        weaverLog("SDLActivity.mWaitForOverride=true");

        asset = new InputViewsAsset();
        // Explicit size: without it the interlacer assumes a 1x1 video and
        // stretches a single texel across the panel. 2 x (1920x1200) = SBS.
        weaverLog("InputViewsAsset.CreateEmptySurfaceForVideo(" + SBS_W + "x" + SBS_H + ") begin");
        asset.CreateEmptySurfaceForVideo(3840, 1200, new SurfaceTextureReadyCallback() {
            @Override
            public void onSurfaceTextureReady(SurfaceTexture texture) {
                weaverLog("SurfaceTextureReady callback: thread=" + Thread.currentThread().getName()
                        + " texture=" + texture);
                if (texture == null) {
                    weaverLog("SurfaceTextureReady got NULL texture; cannot weave");
                    return;
                }
                // The producer buffer size must match what the interlacer is
                // told the source is, otherwise it samples a magnified crop.
                texture.setDefaultBufferSize(SBS_W, SBS_H);
                weaverLog("SurfaceTexture default buffer set to " + SBS_W + "x" + SBS_H);
                // The first woven frame sometimes comes up black until the view
                // is reconfigured; nudge it once the surface exists.
                // The first woven frame comes up black until the view is
                // re-attached; mimic the FLAT toggle the user found works.
                leiaView.postDelayed(new Runnable() {
                    @Override public void run() {
                        weaverLog("reattach nudge: hiding leiaView");
                        leiaView.setVisibility(View.GONE);
                        leiaView.postDelayed(new Runnable() {
                            @Override public void run() {
                                leiaView.setVisibility(View.VISIBLE);
                                reapplySourceSize();
                                weaverLog("reattach nudge: visible + reapplySourceSize done");
                            }
                        }, 400);
                    }
                }, 1500);
                try {
                    SDLActivity.setOverrideSurface(new Surface(texture));
                    weaverLog("SDL override surface set from Leia SurfaceTexture");
                } catch (Throwable t) {
                    weaverError("setOverrideSurface failed", t);
                }
            }
        });
        weaverLog("asset created; surface request queued");

        leiaView = new InterlacedSurfaceView(this);
        leiaView.setClickable(false);
        leiaView.setFocusable(false);
        weaverLog("InterlacedSurfaceView created");

        // Attach the view before binding the asset: the interlacer only creates
        // the SurfaceTexture once the view is in a window.
        ViewGroup layout = mLayout;
        weaverLog("mLayout=" + layout);
        if (layout != null) {
            layout.addView(leiaView, new RelativeLayout.LayoutParams(
                    RelativeLayout.LayoutParams.MATCH_PARENT,
                    RelativeLayout.LayoutParams.MATCH_PARENT));
            weaverLog("leiaView attached; parent now " + layout.getWidth() + "x"
                    + layout.getHeight());

            // Controls on their own layer, above the interlaced view but below
            // the debug panel. Screen-space: never per-eye, never letterboxed.
            touchOverlay = new TouchOverlay(this);
            layout.addView(touchOverlay, new RelativeLayout.LayoutParams(
                    RelativeLayout.LayoutParams.MATCH_PARENT,
                    RelativeLayout.LayoutParams.MATCH_PARENT));
            weaverLog("TouchOverlay attached");
            try {
                nativeHideInGameControls(1);
                Log.i(TAG, "overlay controls active; in-game sprites hidden");
            } catch (Throwable t) {
                Log.w(TAG, "hide in-game controls failed", t);
            }

            stereoPictogram = new StereoPictogramView();
            stereoPictogram.setClickable(false);
            stereoPictogram.setFocusable(false);
            stereoPictogram.setVisibility(View.GONE);
            RelativeLayout.LayoutParams pictoParams = new RelativeLayout.LayoutParams(
                    dp(250), dp(175));
            pictoParams.addRule(RelativeLayout.ALIGN_PARENT_TOP);
            pictoParams.addRule(RelativeLayout.ALIGN_PARENT_RIGHT);
            pictoParams.topMargin = dp(10);
            pictoParams.rightMargin = dp(10);
            layout.addView(stereoPictogram, pictoParams);

            RelativeLayout.LayoutParams panelParams = new RelativeLayout.LayoutParams(
                    RelativeLayout.LayoutParams.MATCH_PARENT,
                    RelativeLayout.LayoutParams.WRAP_CONTENT);
            panelParams.addRule(RelativeLayout.ALIGN_PARENT_BOTTOM);
            layout.addView(buildTuningPanel(), panelParams);
            weaverLog("debug tuning panel attached");
            // Apply the persisted opacity after both the overlay and its slider
            // exist (SeekBar.setProgress happens before the listener is set).
            applyOverlayAlpha(overlayAlphaPct);
            updateToeSliderState();
            try {
                if (touchOverlay != null) {
                    touchOverlay.setAnalogMode(true);   // analog camera stick by default
                }
            } catch (Throwable t) {
                Log.w(TAG, "analog default failed", t);
            }
        } else {
            weaverError("mLayout is null; Leia view was not attached", null);
        }
        leiaView.setViewAsset(asset);
        weaverLog("setViewAsset(asset) done");
        // Size the surface to the view once laid out (mpv does this on layout).
        leiaView.addOnLayoutChangeListener(new View.OnLayoutChangeListener() {
            @Override
            public void onLayoutChange(View v, int l, int t, int r, int b,
                                       int ol, int ot, int orr, int ob) {
                weaverLog("leiaView layout " + ol + "," + ot + "," + orr + "," + ob
                        + " -> " + l + "," + t + "," + r + "," + b);
                resizeLeiaSurface(v.getWidth(), v.getHeight());
            }
        });
        weaverLog("layout change listener installed");
        // The view may not be laid out yet at this point; retry until it has
        // a real size, then create the surface at that exact geometry.
        leiaView.postDelayed(new Runnable() {
            @Override
            public void run() {
                if (leiaView.getWidth() > 0) {
                    weaverLog("initial layout ready: " + leiaView.getWidth() + "x"
                            + leiaView.getHeight() + "; sizing Leia surface");
                    resizeLeiaSurface(leiaView.getWidth(), leiaView.getHeight());
                } else {
                    weaverLog("initial layout not ready; retrying in 300ms");
                    leiaView.postDelayed(this, 300);
                }
            }
        }, 200);

        leiaView.setSingleViewMode(false);
        weaverLog("setSingleViewMode(false); initLeia complete");
    }

    private void weaverLog(String message) {
        long elapsed = leiaT0 == 0 ? 0 : (SystemClock.uptimeMillis() - leiaT0);
        DebugLog.note("WEAVER +" + elapsed + "ms " + message);
    }

    private void weaverError(String message, Throwable t) {
        long elapsed = leiaT0 == 0 ? 0 : (SystemClock.uptimeMillis() - leiaT0);
        Log.e(TAG, "WEAVER +" + elapsed + "ms " + message, t);
    }

    /** Android/device/firmware versions at the moment the weaver starts. */
    private void logWeaverEnvironment() {
        try {
            weaverLog("android release=" + Build.VERSION.RELEASE
                    + " sdk=" + Build.VERSION.SDK_INT
                    + " securityPatch=" + Build.VERSION.SECURITY_PATCH);
            weaverLog("lume firmware display=" + Build.DISPLAY
                    + " id=" + Build.ID
                    + " incremental=" + Build.VERSION.INCREMENTAL);
            weaverLog("device manufacturer=" + Build.MANUFACTURER
                    + " model=" + Build.MODEL
                    + " device=" + Build.DEVICE
                    + " product=" + Build.PRODUCT
                    + " hardware=" + Build.HARDWARE);
            StringBuilder abis = new StringBuilder();
            for (String abi : Build.SUPPORTED_ABIS) {
                if (abis.length() > 0) abis.append(',');
                abis.append(abi);
            }
            weaverLog("supportedAbis=" + abis);
        } catch (Throwable t) {
            weaverError("environment log failed", t);
        }
        try {
            PackageInfo pi = getPackageManager().getPackageInfo(getPackageName(), 0);
            weaverLog("app " + getPackageName() + " version=" + pi.versionName
                    + " code=" + pi.getLongVersionCode());
        } catch (Throwable t) {
            weaverError("app version log failed", t);
        }
        try {
            Display d = getWindowManager().getDefaultDisplay();
            DisplayMetrics dm = new DisplayMetrics();
            d.getRealMetrics(dm);
            weaverLog("display real=" + dm.widthPixels + "x" + dm.heightPixels
                    + " density=" + dm.densityDpi + "dpi refresh=" + d.getRefreshRate()
                    + "Hz mode=" + (d.getMode() == null ? "?" : d.getMode().getModeId()));
            Display.Mode[] modes = d.getSupportedModes();
            StringBuilder sb = new StringBuilder();
            for (int i = 0; i < modes.length; i++) {
                if (i > 0) sb.append(", ");
                sb.append(modes[i].getModeId()).append('=')
                        .append(Math.round(modes[i].getRefreshRate())).append("Hz/")
                        .append(modes[i].getPhysicalWidth()).append('x')
                        .append(modes[i].getPhysicalHeight());
            }
            weaverLog("display modes: " + sb);
        } catch (Throwable t) {
            weaverError("display log failed", t);
        }
    }

    /** Debug sliders: they push values straight into the native stereo renderer. */
    private boolean debugPanelOpen() {
        return tabContent != null && tabContent.getVisibility() == View.VISIBLE;
    }

    /** Collapse the tuning panel (used by the hide button, gestures, taps outside). */
    private boolean panelAnimating = false;

    void expandDebugPanel() {
        if (tabContent == null || debugPanelOpen() || panelAnimating) {
            return;
        }
        if (hideButton != null) {
            hideButton.setText("HIDE");
        }
        panelAnimating = true;
        tabContent.setVisibility(View.VISIBLE);
        final float off = Math.max(tabContent.getHeight(), dp(120));
        tabContent.setTranslationY(off);
        tabContent.setAlpha(0f);
        tabContent.animate().translationY(0f).alpha(1f).setDuration(180)
                .withEndAction(() -> {
                    panelAnimating = false;
                    updateStereoPictogramVisibility();
                }).start();
    }

    void collapseDebugPanelIfOpen() {
        if (!debugPanelOpen() || panelAnimating) {
            return;
        }
        if (hideButton != null) {
            hideButton.setText("SHOW");
        }
        panelAnimating = true;
        final float off = Math.max(tabContent.getHeight(), dp(120));
        tabContent.animate().translationY(off).alpha(0f).setDuration(160)
                .withEndAction(() -> {
                    tabContent.setVisibility(View.GONE);
                    tabContent.setTranslationY(0f);
                    tabContent.setAlpha(1f);
                    updateStereoPictogramVisibility();
                    panelAnimating = false;
                }).start();
    }

    /**
     * Panel gestures: a tap outside the open panel collapses it, and a downward
     * swipe started on the panel (but not on a slider) collapses it too.
     */
    private void handleDebugPanelGestures(android.view.MotionEvent ev) {
        if (!debugPanelOpen()) {
            // Collapsed: a swipe up on the bar expands the panel.
            android.graphics.Rect barRect = new android.graphics.Rect();
            switch (ev.getActionMasked()) {
                case android.view.MotionEvent.ACTION_DOWN: {
                    boolean onBar = false;
                    if (tuningPanelView != null) {
                        tuningPanelView.getGlobalVisibleRect(barRect);
                        onBar = barRect.contains((int) ev.getRawX(), (int) ev.getRawY());
                    }
                    panelDownY = onBar ? ev.getRawY() : -1;
                    break;
                }
                case android.view.MotionEvent.ACTION_MOVE:
                    if (panelDownY >= 0 && panelDownY - ev.getRawY() > dp(70)) {
                        panelDownY = -1;
                        expandDebugPanel();
                    }
                    break;
                case android.view.MotionEvent.ACTION_UP:
                case android.view.MotionEvent.ACTION_CANCEL:
                    panelDownY = -1;
                    break;
            }
            return;
        }
        switch (ev.getActionMasked()) {
            case android.view.MotionEvent.ACTION_DOWN: {
                boolean inside = false;
                android.graphics.Rect r = new android.graphics.Rect();
                if (tuningPanelView != null) {
                    tuningPanelView.getGlobalVisibleRect(r);
                    inside = r.contains((int) ev.getRawX(), (int) ev.getRawY());
                }
                if (!inside) {
                    collapseDebugPanelIfOpen();
                    panelDownY = -1;
                    break;
                }
                // Don't treat vertical slider drags as a collapse swipe.
                boolean onSlider = false;
                for (int i = 0; i < sliderRows.size(); i++) {
                    View row = sliderRows.get(i);
                    if (row.getVisibility() == View.VISIBLE) {
                        row.getGlobalVisibleRect(r);
                        if (r.contains((int) ev.getRawX(), (int) ev.getRawY())) {
                            onSlider = true;
                            break;
                        }
                    }
                }
                panelDownY = onSlider ? -1 : ev.getRawY();
                break;
            }
            case android.view.MotionEvent.ACTION_MOVE:
                if (panelDownY >= 0 && ev.getRawY() - panelDownY > dp(70)) {
                    panelDownY = -1;
                    collapseDebugPanelIfOpen();
                }
                break;
            case android.view.MotionEvent.ACTION_UP:
            case android.view.MotionEvent.ACTION_CANCEL:
                panelDownY = -1;
                break;
        }
    }

    @Override
    public boolean dispatchTouchEvent(android.view.MotionEvent ev) {
        try {
            handleDebugPanelGestures(ev);
        } catch (Throwable ignored) {
        }
        return super.dispatchTouchEvent(ev);
    }

    private LinearLayout buildTuningPanel() {
        LinearLayout panel = new LinearLayout(this);
        panel.setOrientation(LinearLayout.VERTICAL);
        tuningPanelView = panel;
        panel.setBackgroundColor(PANEL_BG);
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
        hideButton = hide;
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
        reset.setOnClickListener(v -> debugReset());

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

        TextView camBtn = new TextView(this);
        camCycleButton = camBtn;
        camBtn.setText("CAM: " + camModeName(camMode2));
        camBtn.setTextColor(0xFFCCCCCC);
        camBtn.setTextSize(14);
        camBtn.setPadding(pad, pad / 2, pad, pad / 2);
        header.addView(camBtn, abs(620, 0, 370, 64));
        camBtn.setOnClickListener(v -> cycleCamMode());

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
        addTabButton(tabBar, "STEREO", 0);
        addTabButton(tabBar, "EFFECTS", -1);
        addTabButton(tabBar, "CAMERA", 6);
        addTabButton(tabBar, "ENH", 7);
        addTabButton(tabBar, "CHANGELOG", 10);
        addTabButton(tabBar, "EXPORT", 11);
        addTabButton(tabBar, "ABOUT", 12);
        panel.addView(tabBar);
        addEffectsSubBar(panel);

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
        addSlider("IPD / baseline", -300, 300, 300, " px", 0);
        addSlider("Zero parallax", -100, 100, 40, " px", 1);
        addSlider("Depth", -200, 200, 50, " %", 2);
        addSlider("UI depth", -60, 60, 0, " px", 38);
        addSlider("UI size", 50, 200, 160, " %", 58);
        addSlider("UI box width", 50, 200, 100, " %", 59);
        addSlider("Toe in/out", -100, 100, 0, " %", 17);
        addSlider("Aspect % (in)", 10, 4000, 834, "", 57);
        addSlider("Eye margin%", 50, 100, 100, " %", 14);
        addIntSlider("Ctl opacity", 0, 100, overlayAlphaPct, new java.util.function.IntConsumer() {
            @Override public void accept(int v) { applyOverlayAlpha(v); }
        });
        controls.addView(makePinchToggle());
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
        String[][] tgl = {{"SWAP", "4"}, {"SAME", "6"},
                {"PERF2x", "12"}, {"TOEAUTO", "40"}};
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
                    if (t[0].equals("SAME")) {
                        sameMode = (sameMode + 1) % 3;
                        nativeSetStereoInt(6, sameMode);
                        b.setText(sameMode == 0 ? "SAME" : sameMode == 1 ? "SAME L" : "SAME R");
                        b.setTextColor(sameMode != 0 ? 0xFF66FF99 : 0xFF888888);
                        Log.i(TAG, "SAME=" + sameMode);
                        return;
                    }
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
                        if (t[0].equals("TOEAUTO")) {
                            toeAutoOn = on;
                            stereoValues.put(40, on ? 1 : 0);
                            if (stereoPictogram != null) stereoPictogram.invalidate();
                            updateToeSliderState();
                        }
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
        selectPage(0);

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
                if (splatTargetButton != null) {
                    splatMode = 0;
                    nativeSetStereoInt(60, 0);
                    splatTargetButton.setText(splatTargetLabel());
                    splatTargetButton.setTextColor(0xFF888888);
                    refreshTabStyles();
                }
                if (splatShapeButton != null) {
                    splatShape = 0;
                    nativeSetStereoInt(65, 0);
                    splatShapeButton.setText(splatShapeLabel());
                    splatShapeButton.setTextColor(0xFF888888);
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
                updateStereoPictogramVisibility();
            }
        });
        return panel;
    }

    // ---- Debug panel tabs ---------------------------------------------------

    private void addTabButton(LinearLayout bar, String label, final int pageIndex) {
        TextView b = new TextView(this);
        b.setText(label);
        b.setTextSize(15);
        b.setTextColor(0xFF888888);
        b.setPadding(dp(14), dp(6), dp(14), dp(6));
        final int idx = tabButtons.size();
        topTabPages.add(pageIndex);
        b.setOnClickListener(v -> {
            if (tabContent != null && tabContent.getVisibility() != View.VISIBLE) {
                tabContent.setVisibility(View.VISIBLE);
                if (hideButton != null) hideButton.setText("HIDE");
            }
            selectTopTab(idx);
        });
        addPressFeedback(b);
        bar.addView(b);
        tabButtons.add(b);
    }

    private void addEffectsSubBar(LinearLayout panel) {
        effectsSubBar = new LinearLayout(this);
        effectsSubBar.setOrientation(LinearLayout.HORIZONTAL);
        effectsSubBar.setPadding(dp(4), 0, dp(4), dp(2));
        for (int i = 0; i < EFFECT_LABELS.length; i++) {
            final int page = EFFECT_PAGES[i];
            TextView b = new TextView(this);
            b.setText(EFFECT_LABELS[i]);
            b.setTextSize(13);
            b.setTextColor(0xFF888888);
            b.setPadding(dp(10), dp(4), dp(10), dp(4));
            b.setOnClickListener(v -> {
                lastEffectPage = page;
                selectPage(page);
            });
            effectsSubBar.addView(b);
            effectsSubButtons.add(b);
        }
        effectsSubBar.setVisibility(View.GONE);
        panel.addView(effectsSubBar, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
    }

    private void selectTopTab(int topIndex) {
        if (topIndex == EFFECTS_TOP_INDEX) {
            selectPage(lastEffectPage);
        } else {
            selectPage(topTabPages.get(topIndex));
        }
    }

    private void updateStereoPictogramVisibility() {
        if (stereoPictogram == null) {
            return;
        }
        boolean show = selectedPage == 0 && debugPanelOpen()
                && tuningPanelView != null && tuningPanelView.getVisibility() == View.VISIBLE;
        stereoPictogram.setVisibility(show ? View.VISIBLE : View.GONE);
        pictoHandler.removeCallbacks(pictoRefresh);
        if (show) {
            pictoHandler.post(pictoRefresh);
        }
    }

    private void selectPage(int page) {
        if (page < 0 || page >= tabViews.size()) {
            return;
        }
        selectedPage = page;
        for (int i = 0; i < tabViews.size(); i++) {
            tabViews.get(i).setVisibility(i == page ? View.VISIBLE : View.GONE);
        }
        boolean isEffect = false;
        for (int p : EFFECT_PAGES) {
            if (p == page) {
                isEffect = true;
                lastEffectPage = page;
                break;
            }
        }
        if (isEffect) {
            selectedTab = EFFECTS_TOP_INDEX;
        } else {
            int top = topTabPages.indexOf(page);
            selectedTab = top >= 0 ? top : 0;
        }
        if (effectsSubBar != null) {
            effectsSubBar.setVisibility(isEffect ? View.VISIBLE : View.GONE);
        }
        refreshTabStyles();
        updateStereoPictogramVisibility();
    }

    /** Which top-level tabs show "feature enabled" in green. */
    private boolean tabFeatureOn(int i) {
        switch (i) {
            case 1: return aoOn || dofOn || giOn || ssrtxOn || shadowOn
                    || splatMode != 0;                               // EFFECTS
            case 2: return hdtOn || gyroOn;                              // CAMERA
            default: return false;
        }
    }

    private boolean effectOn(int i) {
        switch (i) {
            case 1: return aoOn;     // MXAO
            case 2: return dofOn;    // C.DOF
            case 3: return giOn;     // SSGI
            case 4: return ssrtxOn;  // SSRTX
            case 5: return shadowOn; // SHADOW
            case 6: return splatMode != 0; // SPLAT
            default: return false;   // GENERIC
        }
    }

    /** Selected tab: white + underline. Enabled feature: green. */
    private void refreshTabStyles() {
        for (int i = 0; i < tabButtons.size(); i++) {
            TextView b = tabButtons.get(i);
            boolean sel = (i == selectedTab);
            b.setTextColor(sel ? 0xFFFFFFFF : (tabFeatureOn(i) ? 0xFF44FF66 : 0xFF888888));
            int flags = b.getPaintFlags();
            if (sel) {
                flags |= android.graphics.Paint.UNDERLINE_TEXT_FLAG;
            } else {
                flags &= ~android.graphics.Paint.UNDERLINE_TEXT_FLAG;
            }
            b.setPaintFlags(flags);
        }
        for (int i = 0; i < effectsSubButtons.size(); i++) {
            TextView b = effectsSubButtons.get(i);
            boolean sel = (EFFECT_PAGES[i] == selectedPage);
            b.setTextColor(sel ? 0xFFFFFFFF : (effectOn(i) ? 0xFF44FF66 : 0xFF888888));
            int flags = b.getPaintFlags();
            if (sel) {
                flags |= android.graphics.Paint.UNDERLINE_TEXT_FLAG;
            } else {
                flags &= ~android.graphics.Paint.UNDERLINE_TEXT_FLAG;
            }
            b.setPaintFlags(flags);
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

    private String splatShapeLabel() {
        return splatShape == 1 ? "SPLAT SHAPE: PAINT STROKES"
                               : "SPLAT SHAPE: RADIAL";
    }

    private String splatTargetLabel() {
        switch (splatMode) {
            case 1: return "SPLAT TARGET: MARIO";
            case 2: return "SPLAT TARGET: TREES";
            case 3: return "SPLAT TARGET: ALL OBJECTS";
            default: return "SPLAT TARGET: OFF";
        }
    }

    private void buildFxTabs() {
        // --- Generic stereo debug views ---
        beginTab();
        addStereoToggle("ZBUF", 15, false);
        addStereoToggle("WIRE", 16, false);

        // --- MXAO-style ambient occlusion ---
        beginTab();
        aoToggle = addFxToggle("MXAO ENABLE", 1, aoOn);
        addFxDebugButton("SHOW AO", 1);
        addFxSlider("Intensity", 0, 300, 100, " %", 2);
        addFxSlider("Radius", 10, 500, 120, " u", 3);
        addFxSlider("Samples", 4, 32, 12, "", 4);
        addFxSlider("Bias", 0, 100, 20, " u", 5);
        addFxSlider("Power", 50, 400, 150, " %", 6);

        // --- Cinematic DOF ---
        beginTab();
        dofToggle = addFxToggle("CINEMATIC DOF", 11, dofOn);
        addFxToggle("AUTO FOCUS", 16, true);   // tracks Mario by default
        addFxDebugButton("SHOW CoC", 2);
        addFxSlider("Focus", 50, 5000, 800, " u", 12);
        addFxSlider("Aperture", 0, 300, 100, " %", 13);
        addFxSlider("Max blur", 0, 40, 12, " px", 14);
        addFxSlider("Samples", 4, 32, 16, "", 15);

        // --- Screen-space GI (SSGI/RTGI-style) ---
        beginTab();
        giToggle = addFxToggle("SSGI ENABLE", 21, giOn);
        addFxDebugButton("SHOW GI", 3);
        addFxSlider("Intensity", 0, 300, 100, " %", 22);
        addFxSlider("Radius", 10, 500, 150, " u", 23);
        addFxSlider("Steps", 2, 12, 6, "", 24);
        addFxSlider("Rays", 2, 8, 4, "", 25);

        // --- SSRTX: screen-space ray-traced reflections ---
        beginTab();
        ssrtxToggle = addFxToggle("SSRTX ENABLE", 70, ssrtxOn);
        addFxDebugButton("SHOW SSRTX", 4);
        addFxSlider("Intensity", 0, 300, 100, " %", 71);
        addFxSlider("Distance", 50, 2000, 600, " u", 72);
        addFxSlider("Steps", 2, 48, 20, "", 73);
        addFxSlider("Rays", 1, 8, 1, "", 74);
        addFxSlider("Thickness", 5, 300, 80, " u", 75);
        addFxSlider("Roughness", 0, 100, 0, " %", 76);
        addFxSlider("Edge fade", 0, 100, 100, " %", 77);

        // --- Camera tracking / sensors ---
        beginTab();
        addCamModePills(controls);
        nativeSetCamMode2(camMode2);

        // Tracking toggles + recentre in one 3-column row, above the mode
        // selector and all sliders.
        LinearLayout camTop = new LinearLayout(this);
        camTop.setOrientation(LinearLayout.HORIZONTAL);
        camTop.setPadding(0, dp(6), 0, dp(2));
        camHeadToggle = makeCamToggle("HEAD TRACK", 41, hdtOn);
        camGyroToggle = makeCamToggle("GYRO CAM", 42, gyroOn);
        TextView recentre = makeCamButton("RECENTRE", () -> recentreGyro());
        camTop.addView(camHeadToggle, new LinearLayout.LayoutParams(0,
                ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        camTop.addView(camGyroToggle, new LinearLayout.LayoutParams(0,
                ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        camTop.addView(recentre, new LinearLayout.LayoutParams(0,
                ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        controls.addView(camTop, 0);

        controls.addView(makePinchToggle(), 0);
        addCamToggle("WALL NOCLIP", 52, true);

        addCamSlider("Cam distance", 1, 1500, 700, " u", 46);
        addCamSlider("FOV %", 1, 400, 100, " %", 20);
        addCamSlider("3D height", 50, 150, 100, " %", 50);
        addCamSlider("3D width", 50, 150, 50, " %", 51);
        addCamSlider("Cam height", 0, 400, 110, " u", 47);
        addCamSlider("Cam recentre", 0, 200, 50, " %", 48);
        addCamSlider("Cam sensitivity", 10, 300, 100, " %", 49);
        addCamSlider("HDT strength", 0, 200, hdtStrength, " %", 43);
        addCamSlider("Pivot height", 0, 200, gyroPivot, " cm", 45);
        addCamSlider("Gyro strength", 0, 200, gyroStrength, " %", 44);
        addCamButton("ASPECT TEST", () -> nativePtcCommand("aspecttest"));
        camStatusView = new TextView(this);
        camStatusView.setTextSize(14);
        camStatusView.setTextColor(0xFFAACCFF);
        camStatusView.setPadding(dp(4), dp(10), dp(4), dp(4));
        controls.addView(camStatusView);

        // --- Gameplay enhancements: sub-tabbed ---
        beginTab();
        {
            final String[] pageNames = { "MOVE", "PORTAL", "BALL", "SYS" };
            // The sub-tab strip lives in tabContent (outside the scroll view) so
            // it stays put while the page content scrolls.
            LinearLayout tabRow = new LinearLayout(this);
            tabRow.setOrientation(LinearLayout.HORIZONTAL);
            if (enhSubBar != null && enhSubBar.getParent() instanceof ViewGroup) {
                ((ViewGroup) enhSubBar.getParent()).removeView(enhSubBar);
            }
            enhSubBar = tabRow;
            tabContent.addView(tabRow, 0, new LinearLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
            enhPages = new LinearLayout[pageNames.length];
            enhPageTabs = new TextView[pageNames.length];
            enhPageColumns = new LinearLayout[pageNames.length][3];
            for (int i = 0; i < pageNames.length; i++) {
                final int page = i;
                TextView tab = new TextView(this);
                tab.setText(pageNames[i]);
                tab.setTextSize(16);
                tab.setPadding(dp(10), dp(8), dp(10), dp(8));
                tab.setTextColor(i == 0 ? 0xFF66FF99 : 0xFF888888);
                tab.setOnClickListener(v -> showEnhPage(page));
                tabRow.addView(tab);
                enhPageTabs[i] = tab;

                LinearLayout pageLayout = new LinearLayout(this);
                pageLayout.setOrientation(LinearLayout.VERTICAL);
                LinearLayout row = new LinearLayout(this);
                row.setOrientation(LinearLayout.HORIZONTAL);
                LinearLayout left = new LinearLayout(this);
                left.setOrientation(LinearLayout.VERTICAL);
                LinearLayout right = new LinearLayout(this);
                right.setOrientation(LinearLayout.VERTICAL);
                LinearLayout third = new LinearLayout(this);
                third.setOrientation(LinearLayout.VERTICAL);
                row.addView(left, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
                row.addView(right, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
                row.addView(third, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
                pageLayout.addView(row);
                pageLayout.setVisibility(i == 0 ? View.VISIBLE : View.GONE);
                controls.addView(pageLayout, new LinearLayout.LayoutParams(
                        ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
                enhPages[i] = pageLayout;
                enhPageColumns[i][0] = left;
                enhPageColumns[i][1] = right;
                enhPageColumns[i][2] = third;
            }
        }

        enhColumnTarget = enhPageColumns[0][0];
        addEnhSection("MOVEMENT / PHYSICS");
        ragdollToggle = addEnhToggle("DEATH RAGDOLL", 0, ragdollOn);
        hitRagdollToggle = addEnhToggle("HIT RAGDOLL", 1, hitRagdollOn);
        // PUPPYCAM+ is hidden: the CAMERA tab's PUPpycam mode supersedes it.
        addEnhToggle("WALLRUN", 3, true);
        addEnhToggle("GRAPPLE (L)", 4, true);
        addEnhToggle("JETPACK (L)", 5, false);
        final TextView fuelUp = new TextView(this);
        fuelUp.setText("FUEL TANK L" + (nativeGetFuelTier() + 1));
        fuelUp.setTextSize(16);
        fuelUp.setTextColor(0xFF66CCFF);
        fuelUp.setPadding(dp(4), dp(8), dp(4), dp(8));
        fuelUp.setOnClickListener(v -> {
            nativeMovekitUpgrade(0);
            fuelUp.setText("FUEL TANK L" + (nativeGetFuelTier() + 1));
        });
        controls.addView(fuelUp);

        final TextView refillUp = new TextView(this);
        refillUp.setText("REFILL PUMP L" + (nativeGetRefillTier() + 1));
        refillUp.setTextSize(16);
        refillUp.setTextColor(0xFF66CCFF);
        refillUp.setPadding(dp(4), dp(8), dp(4), dp(8));
        refillUp.setOnClickListener(v -> {
            nativeMovekitUpgrade(1);
            refillUp.setText("REFILL PUMP L" + (nativeGetRefillTier() + 1));
        });
        enhParent().addView(refillUp);
        enhColumnTarget = enhPageColumns[3][0];
        addEnhSection("SESSION");
        addEnhButton("LEVEL SELECT", this::openLevelSelect);
        addEnhButton("KILL MARIO (TEST)", this::debugKill);
        enhColumnTarget = enhPageColumns[0][1];
        addEnhSection("RENDER / SIM");
        uncappedTestToggle = new TextView(this);
        uncappedTestToggle.setText("UNCAPPED TEST SIM: " + (uncappedTestOn ? "ON" : "OFF"));
        uncappedTestToggle.setTextSize(16);
        uncappedTestToggle.setTextColor(uncappedTestOn ? 0xFF66FF99 : 0xFF888888);
        uncappedTestToggle.setPadding(dp(4), dp(8), dp(4), dp(8));
        uncappedTestToggle.setOnClickListener(v -> debugUncapped(uncappedTestOn ? 0 : 1));
        enhParent().addView(uncappedTestToggle);
        TextView interp = new TextView(this);
        interpButton = interp;
        interp.setText(interpOn ? "INTERP ON" : "INTERP OFF");
        interp.setTextSize(16);
        interp.setTextColor(interpOn ? 0xFF66FF99 : 0xFF888888);
        interp.setPadding(dp(4), dp(8), dp(4), dp(8));
        interp.setOnClickListener(v -> {
            interpOn = !interpOn;
            nativeSetHfrInterp(interpOn ? 1 : 0);
            interp.setText(interpOn ? "INTERP ON" : "INTERP OFF");
            interp.setTextColor(interpOn ? 0xFF66FF99 : 0xFF888888);
            Log.i(TAG, "INTERP " + interpOn);
        });
        enhParent().addView(interp);

        final TextView stayToggle = new TextView(this);
        stayToggle.setText("STAY IN LEVEL: " + (stayOn ? "ON" : "OFF"));
        stayToggle.setTextSize(16);
        stayToggle.setTextColor(stayOn ? 0xFF66FF99 : 0xFF888888);
        stayToggle.setPadding(dp(4), dp(8), dp(4), dp(8));
        stayToggle.setOnClickListener(v -> {
            stayOn = !stayOn;
            nativeSetStereoInt(30, stayOn ? 1 : 0);
            stayToggle.setText("STAY IN LEVEL: " + (stayOn ? "ON" : "OFF"));
            stayToggle.setTextColor(stayOn ? 0xFF66FF99 : 0xFF888888);
            Log.i(TAG, "STAY=" + stayOn);
        });
        enhParent().addView(stayToggle);

        /* PORTAL page: three balanced columns so the page rarely scrolls.
         * Column 0: ninja run + portal regression tests.
         * Column 1: live test scene (level + core knobs).
         * Column 2: remaining knobs + movement annexes. */
        enhColumnTarget = enhPageColumns[1][0];
        addEnhSection("NINJA CHAIN");
        addEnhButton("RUN NINJA CHAIN", () -> {
            debugLevel(38);
            new android.os.Handler(android.os.Looper.getMainLooper()).postDelayed(
                    () -> nativePtcCommand("chain"), 2500);
        });
        addEnhButton("STOP NINJA CHAIN", () -> nativePtcCommand("chainoff"));

        addEnhSection("PORTAL TESTS");
        addEnhButton("WARP TO PTC", () -> debugLevel(38));
        addEnhButton("PORTAL SMOKE TEST", () -> debugPtc("run"));
        addEnhButton("MOMENTUM TEST", () -> debugPtc("momentum"));
        addEnhButton("MARIO PORTAL WALK LOOP", () -> debugPtc("marioportalwalkloop"));
        addEnhButton("STOP PORTAL WALK LOOP", () -> debugPtc("marioportalwalkstop"));
        addEnhButton("RESET TEST", () -> debugPtc("reset"));
        addEnhButton("CALIBRATE JUMPS", () -> debugPtc("calibrate"));
        portalGunButton = addEnhButton(portalGunLabel(), this::togglePortalGun);

        enhColumnTarget = enhPageColumns[1][1];
        addEnhSection("TEST SCENE (LIVE)");
        addEnhButton("EMPTY TEST LEVEL", () -> debugLevel(37));
        addEnhButton("GOOMBA DEMO", () -> nativePtcCommand("goomba"));
        addEnhButton("RELOAD ptc.cfg", () -> nativePtcCommand("reload"));
        addEnhKnobToggle("TEST SCENE", "scene.on", true);
        addEnhKnobToggle("TEST CUBES", "scene.cubes", true);
        addEnhKnobToggle("DECAL DEPTH", "decal.depth", true);
        addEnhKnobCycle("DECAL OFFSET", "decal.offset",
                new float[] { 4f, 8f, 12f, 20f, 30f }, false);

        enhColumnTarget = enhPageColumns[1][2];
        addEnhKnobCycle("SELECT CONE", "select.maxdeg",
                new float[] { 30f, 45f, 60f, 75f, 90f }, true);
        addEnhKnobToggle("SELECT FACING", "select.facing", true);
        addEnhKnobCycle("RELAY EYE", "eye", new float[] { 0f, 1f, 2f }, true);
        addEnhKnobToggle("RELAY SOLID", "relay.solid", false);
        addEnhKnobToggle("RELAY BLIT", "relay.blit", false);
        addEnhKnobToggle("SOURCE FAN", "fan.src", false);

        addEnhSection("MOVE ANNEXES");
        addEnhButton("WALL-RUN ANNEX", () -> nativePtcCommand("wallrun"));
        addEnhButton("JETPACK ANNEX", () -> nativePtcCommand("jetpack"));
        addEnhButton("GRAPPLE TEST", () -> nativePtcCommand("grapple"));
        addEnhButton("CORNER (W)", () -> nativePtcCommand("cornerportal"));
        addEnhButton("CORNER (S)", () -> nativePtcCommand("cornerportal2"));

        enhColumnTarget = enhPageColumns[2][0];
        addEnhSection("BALL / CUBES");
        addEnhButton("RESET TO LOGO", this::resetToLogo);
        addEnhButton("REINIT LEIA SURFACE", this::debugReinit);
        addEnhButton("BALL BLASTER (TOGGLE)", () -> {
            ballBlasterOn = !ballBlasterOn;
            nativePtcCommand("ballequip");
            Log.i(TAG, "BALL BLASTER=" + ballBlasterOn);
        });
        enhColumnTarget = enhPageColumns[2][1];
        addEnhButton("RESET CUBES", () -> nativePtcCommand("ballreset"));
        addEnhButton("WARP TO CUBES", () -> nativePtcCommand("annex"));
        addEnhButton("WARP TO RESET PAD", () -> nativePtcCommand("resetpad"));
        enhColumnTarget = enhPageColumns[2][2];
        addEnhButton("BUTT SLAM TEST", () -> nativePtcCommand("slam"));
        addEnhButton("FIRE TEST BALL", () -> nativePtcCommand("balltest"));
        enhColumnTarget = enhPageColumns[3][1];
        addEnhSection("SYSTEM");
        pauseLoopToggle = new TextView(this);
        pauseLoopToggle.setText("PAUSE GAME LOOP: OFF");
        pauseLoopToggle.setTextSize(16);
        pauseLoopToggle.setTextColor(0xFF888888);
        pauseLoopToggle.setPadding(dp(4), dp(10), dp(4), dp(10));
        pauseLoopToggle.setOnClickListener(v -> {
            hardPaused = !hardPaused;
            nativeSetHardPause(hardPaused ? 1 : 0);
            pauseLoopToggle.setText("PAUSE GAME LOOP: " + (hardPaused ? "ON" : "OFF"));
            pauseLoopToggle.setTextColor(hardPaused ? 0xFF66FF99 : 0xFF888888);
        });
        enhParent().addView(pauseLoopToggle);
        addEnhButton("STEP ONE TICK", () -> {
            nativeSetHardPause(1);
            hardPaused = true;
            if (pauseLoopToggle != null) {
                pauseLoopToggle.setText("PAUSE GAME LOOP: ON");
                pauseLoopToggle.setTextColor(0xFF66FF99);
            }
            nativeStepTicks(1);
        });
        enhColumnTarget = null;

        // --- Realtime dynamic shadows (quality / performance) ---
        beginTab();
        shadowEnableToggle = addShadowToggle("SHADOW ENABLE", 0, false);
        shadowResButton = addShadowButton("RES 1024", () -> {
            shadowRes = shadowRes <= 512 ? 1024 : (shadowRes <= 1024 ? 2048 : 512);
            if (shadowResButton != null) shadowResButton.setText("RES " + shadowRes);
            nativeSetShadow(1, shadowRes);
            Log.i(TAG, "shadow res " + shadowRes);
        });
        shadowDebugButton = addShadowButton("SHADOW VIEW OFF", () -> {
            shadowDebug = (shadowDebug + 1) % 3;
            if (shadowDebugButton != null) {
                shadowDebugButton.setText("SHADOW VIEW "
                        + (shadowDebug == 0 ? "OFF" : shadowDebug == 1 ? "FACTOR" : "MAP"));
            }
            nativeSetShadow(11, shadowDebug);
            Log.i(TAG, "shadow debug " + shadowDebug);
        });
        addShadowSlider("Strength", 0, 100, 65, " %", 3);
        addShadowSlider("Bias", 0, 200, 25, " u", 4);
        addShadowSlider("Normal bias", 0, 300, 45, " u", 5);
        addShadowSlider("Softness", 0, 8, 2, " tx", 6);
        addShadowSlider("PCF taps", 1, 16, 9, "", 7);
        addShadowSlider("Range", 1500, 9000, 4500, " u", 8);
        addShadowSlider("Sun yaw", 0, 359, 45, "deg", 9);
        addShadowSlider("Sun pitch", 5, 89, 52, "deg", 10);
        addShadowSlider("Update every", 1, 4, 1, " fr", 2);
        // Push the current values once so the native defaults match the labels.
        nativeSetShadow(1, shadowRes);

        // --- Splat rendering (first slice, plan-cool-additions §5.6) ---
        beginTab();
        splatTargetButton = new TextView(this);
        splatTargetButton.setText(splatTargetLabel());
        splatTargetButton.setTextSize(16);
        splatTargetButton.setTextColor(splatMode != 0 ? 0xFF66FF99 : 0xFF888888);
        splatTargetButton.setPadding(dp(4), dp(10), dp(4), dp(10));
        splatTargetButton.setOnClickListener(v -> {
            splatMode = (splatMode + 1) % 4;
            nativeSetStereoInt(60, splatMode);
            splatTargetButton.setText(splatTargetLabel());
            splatTargetButton.setTextColor(splatMode != 0 ? 0xFF66FF99 : 0xFF888888);
            refreshTabStyles();
            Log.i(TAG, "SPLAT target=" + splatMode);
        });
        controls.addView(splatTargetButton);
        splatShapeButton = new TextView(this);
        splatShapeButton.setText(splatShapeLabel());
        splatShapeButton.setTextSize(16);
        splatShapeButton.setTextColor(splatShape != 0 ? 0xFF66FF99 : 0xFF888888);
        splatShapeButton.setPadding(dp(4), dp(10), dp(4), dp(10));
        splatShapeButton.setOnClickListener(v -> {
            splatShape ^= 1;
            nativeSetStereoInt(65, splatShape);
            splatShapeButton.setText(splatShapeLabel());
            splatShapeButton.setTextColor(splatShape != 0 ? 0xFF66FF99 : 0xFF888888);
            Log.i(TAG, "SPLAT shape=" + splatShape);
        });
        controls.addView(splatShapeButton);
        addSlider("Splat size", 1, 64, 14, " px", 61);
        addSlider("Opacity", 0, 100, 100, " %", 62);
        addSlider("Softness", 0, 100, 70, " %", 63);
        addSlider("Density", 0, 100, 35, " %", 64);
        TextView splatHint = new TextView(this);
        splatHint.setTextSize(13);
        splatHint.setTextColor(0xFFAACCFF);
        splatHint.setPadding(dp(4), dp(10), dp(4), dp(4));
        splatHint.setText("Point-sprite pass over tagged object geometry."
                + " Mario is the verified target; trees currently disappear"
                + " at distance (see SPLAT-PROTOTYPE.md). Knobs 60-64;"
                + " Density interpolates points across large walls/floors.");
        controls.addView(splatHint);
        nativeSetStereoInt(60, splatMode);
        nativeSetStereoInt(61, 14);
        nativeSetStereoInt(62, 100);
        nativeSetStereoInt(63, 70);
        nativeSetStereoInt(64, 35);
        nativeSetStereoInt(65, splatShape);

        // --- Changelog ---
        beginTab();
        TextView log = new TextView(this);
        log.setTextSize(12);
        log.setTextColor(0xFFCCEECC);
        log.setTypeface(android.graphics.Typeface.MONOSPACE);
        String stamp = "?";
        try {
            stamp = com.vdavid003.sm64port.BuildConfig.BUILD_STAMP;
        } catch (Throwable ignored) {
        }
        log.setText("sm64ex-android - Lume Pad 2 fork\n"
                + "version " + BUILD_NAME + "\n"
                + "branch  " + buildBranch() + "\n"
                + "built   " + stamp + "\n"
                + "------------------------------------------\n"
                + CHANGELOG);
        controls.addView(log);

        // --- Debug log export (user bug reports) ---
        beginTab();
        TextView exportHint = new TextView(this);
        exportHint.setTextSize(13);
        exportHint.setTextColor(0xFFCCEECC);
        exportHint.setPadding(dp(4), dp(4), dp(4), dp(8));
        exportHint.setText("Capture starts with the app process: Android/device info,"
                + " Leia CNSDK + firmware versions, and the Leia weaver boot stages.\n\n"
                + "EXPORT opens the system save-file picker, then shows the share sheet"
                + " so the log can be sent straight back.");
        controls.addView(exportHint);
        addEnhButton("EXPORT LOG (SAVE + SHARE)", this::exportLogToPicker);
        addEnhButton("SHARE LOG", this::shareDebugLog);
        addEnhButton("REFRESH INFO", this::refreshDebugLogInfo);
        debugLogInfoView = new TextView(this);
        debugLogInfoView.setTextSize(11);
        debugLogInfoView.setTextColor(0xFFAACCFF);
        debugLogInfoView.setTypeface(android.graphics.Typeface.MONOSPACE);
        debugLogInfoView.setPadding(dp(4), dp(8), dp(4), dp(4));
        controls.addView(debugLogInfoView);
        refreshDebugLogInfo();

        // --- About / fork credits ---
        beginTab();
        TextView about = new TextView(this);
        about.setTextSize(12);
        about.setTextColor(0xFFDDEEFF);
        about.setLinkTextColor(0xFF66CCFF);
        about.setPadding(dp(4), dp(4), dp(4), dp(10));
        about.setMovementMethod(LinkMovementMethod.getInstance());
        about.setText(Html.fromHtml(aboutHtml(), Html.FROM_HTML_MODE_LEGACY));
        controls.addView(about);

        TextView aboutEnv = new TextView(this);
        aboutEnv.setTextSize(10);
        aboutEnv.setTextColor(0xFFAACCFF);
        aboutEnv.setPadding(dp(4), dp(10), dp(4), dp(4));
        aboutEnv.setText("This device right now:\n" + DebugLog.oneLineSummary(this)
                + "\n\nFull Android, device, Lume firmware, Leia service and CNSDK"
                + " details are written to the debug log on the EXPORT tab.");
        controls.addView(aboutEnv);
    }

    private String buildBranch() {
        try {
            String b = com.vdavid003.sm64port.BuildConfig.GIT_BRANCH;
            return (b == null || b.isEmpty()) ? "unknown" : b;
        } catch (Throwable t) {
            return "unknown";
        }
    }

    private String aboutHtml() {
        String stamp = "?";
        try {
            stamp = com.vdavid003.sm64port.BuildConfig.BUILD_STAMP;
        } catch (Throwable ignored) {
        }
        return "<b>sm64ex-android &mdash; Lume Pad 2 fork</b><br>"
                + "version " + BUILD_NAME + " &middot; branch " + buildBranch()
                + " &middot; built " + stamp + "<br><br>"
                + "<b>Features added by this fork</b><br>"
                + "&bull; Leia Lume Pad 2 glasses-free 3D output: SDL renders a 2x1 SBS"
                + " frame into a Leia surface, the CNSDK interlacer/weaver shows it on the"
                + " lenticular panel.<br>"
                + "&bull; Stereo tuning (IPD, zero parallax, depth, toe, FOV, aspect) and"
                + " per-eye framebuffers.<br>"
                + "&bull; 120 Hz interpolated rendering while simulation stays at 30 Hz.<br>"
                + "&bull; Screen-space MXAO, cinematic DOF, SSGI and SSRTX reflections, plus"
                + " a realtime dynamic shadow map, with per-effect debug tabs.<br>"
                + "&bull; Leia head-tracked parallax + gyro camera canting.<br>"
                + "&bull; Touch overlay, controller rumble, portal-gun prototype, level"
                + " select, cheats menu, stay-in-level after stars, and debug tooling"
                + " (tabs + adb command channel).<br>"
                + "&bull; On-device debug log export (this tab) for bug reports.<br><br>"
                + "<b>Credits / upstream repositories</b><br>"
                + "&bull; Base Android port: <a href=\"https://github.com/MegaBitmap/sm64ex-android\">"
                + "MegaBitmap/sm64ex-android</a><br>"
                + "&bull; Original PC port: <a href=\"https://github.com/sm64pc/sm64ex\">"
                + "sm64pc/sm64ex</a> and the Super Mario 64 decompilation project.<br>"
                + "&bull; Android/SDL lineage: <a href=\"https://github.com/VDavid003/sm64-port-android\">"
                + "VDavid003/sm64-port-android</a> and "
                + "<a href=\"https://github.com/libsdl-org/SDL\">libsdl-org/SDL</a>.<br>"
                + "&bull; Death/hit ragdolls and Puppycam+ improvements ported from "
                + "<a href=\"https://github.com/Epic0522/Super-Mario-64-3ds-port---Ultimate\">"
                + "Epic0522/Super-Mario-64-3ds-port---Ultimate</a>.<br>"
                + "&bull; Leia CNSDK, lenticular display and Lume firmware: "
                + "<a href=\"https://github.com/LeiaInc\">Leia Inc.</a> "
                + "(<a href=\"https://github.com/LeiaInc/CNSDKGettingStartedGLAndroid\">"
                + "CNSDK Android sample</a>).<br>"
                + "&bull; SSRTX is inspired by ReShade's SSR/SSRT shaders: "
                + "<a href=\"https://github.com/crosire/reshade\">crosire/reshade</a>.<br>"
                + "&bull; This fork: <a href=\"https://github.com/jakedowns/sm64ex-android\">"
                + "jakedowns/sm64ex-android</a>.<br><br>"
                + "Everything else (Lume display path, stereo/HFR, screen-space FX,"
                + " shadows, portal chamber and debug tooling) is original work in this"
                + " fork.";
    }

    private void refreshDebugLogInfo() {
        if (debugLogInfoView == null) return;
        File f = DebugLog.getLogFile(this);
        StringBuilder sb = new StringBuilder();
        sb.append("LOG FILE\n");
        sb.append(f == null ? "(not available)" : f.getAbsolutePath()).append('\n');
        if (f != null && f.isFile()) {
            sb.append(String.format(Locale.US, "size: %.1f KB\n", f.length() / 1024.0));
        } else {
            sb.append("size: (not written yet)\n");
        }
        sb.append("\nThis report includes:\n");
        sb.append("- app/build/Android/device details\n");
        sb.append("- Lume build + Leia service versions\n");
        sb.append("- Leia CNSDK version\n");
        sb.append("- display modes and refresh rates\n");
        sb.append("- Leia weaver/CNSDK boot stage markers\n");
        sb.append("- recent logcat (main + crash buffers)");
        debugLogInfoView.setText(sb.toString());
    }

    private void exportLogToPicker() {
        if (exportInProgress) return;
        try {
            String name = "sm64-debug-"
                    + new SimpleDateFormat("yyyyMMdd-HHmmss", Locale.US).format(new Date())
                    + ".txt";
            Intent intent = new Intent(Intent.ACTION_CREATE_DOCUMENT);
            intent.addCategory(Intent.CATEGORY_OPENABLE);
            intent.setType("text/plain");
            intent.putExtra(Intent.EXTRA_TITLE, name);
            startActivityForResult(intent, REQ_EXPORT_LOG);
        } catch (Throwable t) {
            Log.e(TAG, "export log picker failed", t);
            Toast.makeText(this, "Save picker unavailable: " + t, Toast.LENGTH_LONG).show();
            shareDebugLog();
        }
    }

    private void shareDebugLog() {
        new Thread(new Runnable() {
            @Override
            public void run() {
                try {
                    final File report = DebugLog.buildExportFile(sm64portActivity.this);
                    runOnUiThread(() -> {
                        try {
                            startActivity(DebugLog.buildShareIntent(sm64portActivity.this, report));
                        } catch (Throwable t) {
                            Log.e(TAG, "share log failed", t);
                            Toast.makeText(sm64portActivity.this,
                                    "Share failed: " + t, Toast.LENGTH_LONG).show();
                        }
                    });
                } catch (Throwable t) {
                    Log.e(TAG, "build export failed", t);
                    runOnUiThread(() -> Toast.makeText(sm64portActivity.this,
                            "Log export failed: " + t, Toast.LENGTH_LONG).show());
                }
            }
        }, "sm64-log-export").start();
    }

    private void exportLogToUri(Uri dest) {
        if (dest == null || exportInProgress) return;
        exportInProgress = true;
        Toast.makeText(this, "Preparing debug log...", Toast.LENGTH_SHORT).show();
        new Thread(new Runnable() {
            @Override
            public void run() {
                try {
                    final File report = DebugLog.buildExportFile(sm64portActivity.this);
                    try (InputStream in = new FileInputStream(report);
                         OutputStream out = getContentResolver().openOutputStream(dest, "wt")) {
                        if (out == null) throw new java.io.IOException("no output stream");
                        byte[] buf = new byte[16 * 1024];
                        int n;
                        while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
                        out.flush();
                    }
                    runOnUiThread(() -> {
                        exportInProgress = false;
                        refreshDebugLogInfo();
                        Toast.makeText(sm64portActivity.this,
                                "Debug log saved. Opening share sheet...", Toast.LENGTH_LONG).show();
                        try {
                            startActivity(DebugLog.buildShareIntent(sm64portActivity.this, report));
                        } catch (Throwable t) {
                            Log.e(TAG, "share after save failed", t);
                        }
                    });
                } catch (Throwable t) {
                    Log.e(TAG, "save export failed", t);
                    runOnUiThread(() -> {
                        exportInProgress = false;
                        Toast.makeText(sm64portActivity.this,
                                "Saving log failed: " + t, Toast.LENGTH_LONG).show();
                    });
                }
            }
        }, "sm64-log-save").start();
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        if (requestCode == REQ_EXPORT_LOG) {
            if (resultCode == RESULT_OK && data != null && data.getData() != null) {
                exportLogToUri(data.getData());
            }
            return;
        }
        super.onActivityResult(requestCode, resultCode, data);
    }

    // ---- camera-tracking widgets (stereo-int channel, keys 41..44) ----

    private static final String[] CAM_MODE_SHORT = {
            "VANILLA", "BETTER", "PUPPY", "CUSTOM", "FIRST", "FREE"
    };

    private void addCamModePills(LinearLayout parent) {
        TextView label = new TextView(this);
        label.setText("CAMERA MODE");
        label.setTextSize(12);
        label.setTextColor(0xFF888888);
        label.setPadding(dp(4), dp(10), dp(4), dp(4));
        // beginTab() already put the slider grid in the content view; insert
        // the mode selector above it instead of below all sliders.
        parent.addView(label, 0);
        int trackIndex = 1;

        LinearLayout track = new LinearLayout(this);
        track.setOrientation(LinearLayout.HORIZONTAL);
        GradientDrawable bg = new GradientDrawable();
        bg.setColor(0xFF171B22);
        bg.setCornerRadius(dp(18));
        bg.setStroke(dp(1), 0xFF39404E);
        track.setBackground(bg);
        track.setPadding(dp(3), dp(3), dp(3), dp(3));

        camModePills = new TextView[CAM_MODE_SHORT.length];
        for (int i = 0; i < CAM_MODE_SHORT.length; i++) {
            TextView pill = new TextView(this);
            pill.setText(CAM_MODE_SHORT[i]);
            pill.setTextSize(11);
            pill.setGravity(Gravity.CENTER);
            pill.setPadding(dp(4), dp(8), dp(4), dp(8));
            final int mode = i;
            pill.setOnClickListener(v -> setCamMode2(mode));
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                    0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f);
            track.addView(pill, lp);
            camModePills[i] = pill;
        }
        parent.addView(track, trackIndex, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        updateCamModePills();
    }

    private void setCamMode2(int mode) {
        camMode2 = mode;
        nativeSetCamMode2(mode);
        if (mode != 5) {
            nativeSetFreecamMove(0);
        }
        updateCamModePills();
        Log.i(TAG, "camera mode -> " + camModeLabel());
    }

    int getCamMode2() {
        return camMode2;
    }

    void debugSetCamMode2(int mode) {
        runOnUiThread(() -> setCamMode2(mode));
    }

    void debugSetFreecamMove(int on) {
        nativeSetFreecamMove(on != 0 ? 1 : 0);
        Log.i(TAG, "FREECAM MOVE " + on);
    }

    /** Redundant cycler in the persistent debug header, next to FPS. */
    private void cycleCamMode() {
        setCamMode2((camMode2 + 1) % CAM_MODE_SHORT.length);
    }

    private static String camModeLetter(int mode) {
        int m = Math.max(0, Math.min(CAM_MODE_SHORT.length - 1, mode));
        return String.valueOf((char) ('A' + m));
    }

    private void updateCamModePills() {
        if (camModePills == null) {
            return;
        }
        if (camCycleButton != null && camMode2 >= 0 && camMode2 < CAM_MODE_SHORT.length) {
            camCycleButton.setText("CAM: " + camModeName(camMode2));
        }
        for (int i = 0; i < camModePills.length; i++) {
            TextView pill = camModePills[i];
            if (pill == null) {
                continue;
            }
            GradientDrawable bg = new GradientDrawable();
            bg.setCornerRadius(dp(15));
            if (i == camMode2) {
                bg.setColor(0xFF2E6FD8);
                pill.setTextColor(Color.WHITE);
                pill.setTypeface(null, Typeface.BOLD);
            } else {
                bg.setColor(Color.TRANSPARENT);
                pill.setTextColor(0xFF9AA3B2);
                pill.setTypeface(null, Typeface.NORMAL);
            }
            pill.setBackground(bg);
        }
    }

    private static String camModeName(int mode) {
        switch (mode) {
            case 1: return "BETTERCAM";
            case 2: return "PUPPYCAM";
            case 3: return "CUSTOM";
            case 4: return "FIRST PERSON";
            default: return "VANILLA";
        }
    }

    private String camModeLabel() {
        return "CAM MODE: " + camModeName(camMode2);
    }

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
                stereoValues.put(key, v);
                if (stereoPictogram != null) stereoPictogram.invalidate();
                nativeSetStereoInt(key, v);
                if (key == 43) hdtStrength = v;
                if (key == 44) gyroStrength = v;
                if (key == 45) gyroPivot = v;
            }
            @Override public void onStartTrackingTouch(SeekBar sb) {}
            @Override public void onStopTrackingTouch(SeekBar sb) {}
        });
        addToGrid(label, bar);
        keyBars.put(key, bar);
        keyMins.put(key, min);
        stereoValues.put(key, initial);
        nativeSetStereoInt(key, initial);
    }

    /** Current slider value (used by the two-finger camera gestures). */
    int getCamValue(int key) {
        Integer v = stereoValues.get(key);
        if (v != null) return v;
        switch (key) {
            case 20: return 100;   // FOV %
            case 46: return 700;   // cam distance
            case 47: return 110;   // cam height
            default: return 0;
        }
    }

    /** Drive a slider from a gesture; label and native value update via its listener. */
    void setCamValue(int key, int value) {
        SeekBar bar = keyBars.get(key);
        Integer min = keyMins.get(key);
        if (bar != null && min != null) {
            int p = value - min;
            if (p < 0) p = 0;
            if (p > bar.getMax()) p = bar.getMax();
            bar.setProgress(p);
        } else {
            stereoValues.put(key, value);
            nativeSetStereoInt(key, value);
        }
    }

    /** The two-finger camera gesture enable toggle (also mirrored in CAMERA). */
    private TextView makePinchToggle() {
        final TextView t = new TextView(this);
        t.setText("CAMERA GESTURES (PINCH/SWIPE): "
                + (TouchOverlay.camGesturesEnabled ? "ON" : "OFF"));
        t.setTextSize(15);
        t.setGravity(Gravity.CENTER);
        t.setPadding(dp(4), dp(8), dp(4), dp(8));
        t.setTextColor(TouchOverlay.camGesturesEnabled ? 0xFF66FF99 : 0xFF888888);
        t.setOnClickListener(v -> {
            TouchOverlay.camGesturesEnabled = !TouchOverlay.camGesturesEnabled;
            t.setText("CAMERA GESTURES (PINCH/SWIPE): "
                    + (TouchOverlay.camGesturesEnabled ? "ON" : "OFF"));
            t.setTextColor(TouchOverlay.camGesturesEnabled ? 0xFF66FF99 : 0xFF888888);
            Log.i(TAG, "PINCH_CAM=" + TouchOverlay.camGesturesEnabled);
        });
        return t;
    }

    private TextView makeCamToggle(String name, final int key, boolean initial) {
        final TextView b = new TextView(this);
        b.setText(name + ": " + (initial ? "ON" : "OFF"));
        b.setTextSize(15);
        b.setGravity(Gravity.CENTER);
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
            refreshTabStyles();
            Log.i(TAG, name + "=" + on);
        });
        nativeSetStereoInt(key, initial ? 1 : 0);
        return b;
    }

    private TextView addCamToggle(String name, final int key, boolean initial) {
        TextView b = makeCamToggle(name, key, initial);
        controls.addView(b);
        return b;
    }

    private TextView makeCamButton(String name, final Runnable action) {
        TextView b = new TextView(this);
        b.setText(name);
        b.setTextSize(15);
        b.setGravity(Gravity.CENTER);
        b.setTextColor(0xFF66CCFF);
        b.setPadding(dp(4), dp(10), dp(4), dp(10));
        b.setOnClickListener(v -> action.run());
        return b;
    }

    private LinearLayout enhSubBar;
    private LinearLayout[] enhPages;
    private TextView[] enhPageTabs;
    private LinearLayout[][] enhPageColumns;

    private void showEnhPage(int page) {
        if (enhPages == null) {
            return;
        }
        for (int i = 0; i < enhPages.length; i++) {
            enhPages[i].setVisibility(i == page ? View.VISIBLE : View.GONE);
            enhPageTabs[i].setTextColor(i == page ? 0xFF66FF99 : 0xFF888888);
        }
    }

    private void addCamButton(String name, final Runnable action) {
        controls.addView(makeCamButton(name, action));
    }

    // ---- gameplay enhancement widgets (nativeSetEnh channel) ----

    private TextView addEnhToggle(String name, final int key, boolean initial) {
        final TextView b = new TextView(this);
        b.setText(name + ": " + (initial ? "ON" : "OFF"));
        b.setTextSize(16);
        b.setTextColor(initial ? 0xFF66FF99 : 0xFF888888);
        b.setPadding(dp(4), dp(8), dp(4), dp(8));
        b.setOnClickListener(v -> {
            boolean on = b.getCurrentTextColor() != 0xFF66FF99;
            b.setText(name + ": " + (on ? "ON" : "OFF"));
            b.setTextColor(on ? 0xFF66FF99 : 0xFF888888);
            nativeSetEnh(key, on ? 1 : 0);
            if (key == 0) ragdollOn = on;
            if (key == 1) hitRagdollOn = on;
            if (key == 2) ppcamOn = on;
            Log.i(TAG, name + "=" + on);
        });
        enhParent().addView(b);
        nativeSetEnh(key, initial ? 1 : 0);
        return b;
    }

    private TextView addStereoToggle(String name, final int key, boolean initial) {
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
            Log.i(TAG, name + "=" + on);
        });
        controls.addView(b);
        nativeSetStereoInt(key, initial ? 1 : 0);
        return b;
    }

    private void addPressFeedback(TextView v) {
        v.setOnTouchListener((view, e) -> {
            int a = e.getActionMasked();
            if (a == android.view.MotionEvent.ACTION_DOWN) {
                view.setAlpha(0.55f);
            } else if (a == android.view.MotionEvent.ACTION_UP
                    || a == android.view.MotionEvent.ACTION_CANCEL) {
                view.setAlpha(1f);
            }
            return false;
        });
    }

    private TextView addEnhButton(String name, final Runnable action) {
        TextView b = new TextView(this);
        b.setText(name);
        b.setTextSize(16);
        b.setTextColor(0xFFFF9966);
        b.setPadding(dp(4), dp(10), dp(4), dp(10));
        b.setOnClickListener(v -> action.run());
        addPressFeedback(b);
        enhParent().addView(b);
        return b;
    }

    /** Section header for the ENH tab, so long lists stay readable. */
    private TextView addEnhSection(String title) {
        TextView h = new TextView(this);
        h.setText(title);
        h.setTextSize(15);
        h.setTextColor(0xFF66CCFF);
        h.setPadding(dp(4), dp(14), dp(4), dp(4));
        h.setBackgroundColor(0x22FFFFFF);
        enhParent().addView(h);
        return h;
    }

    /** ENH toggle that drives a runtime PTC knob (`ptc knob <name> 1|0`). */
    private TextView addEnhKnobToggle(String name, final String knob, boolean initial) {
        final TextView b = new TextView(this);
        b.setText(name + ": " + (initial ? "ON" : "OFF"));
        b.setTextSize(16);
        b.setTextColor(initial ? 0xFF66FF99 : 0xFF888888);
        b.setPadding(dp(4), dp(8), dp(4), dp(8));
        b.setOnClickListener(v -> {
            boolean on = b.getCurrentTextColor() != 0xFF66FF99;
            b.setText(name + ": " + (on ? "ON" : "OFF"));
            b.setTextColor(on ? 0xFF66FF99 : 0xFF888888);
            nativePtcCommand("knob " + knob + " " + (on ? 1 : 0));
            Log.i(TAG, "KNOB " + knob + "=" + (on ? 1 : 0));
        });
        addPressFeedback(b);
        enhParent().addView(b);
        return b;
    }

    /** ENH control that cycles a runtime PTC knob through fixed values. */
    private TextView addEnhKnobCycle(String name, final String knob, final float[] values,
                                     final boolean integerFormat) {
        final TextView b = new TextView(this);
        final int[] idx = {0};
        Runnable render = () -> b.setText(name + ": "
                + (integerFormat ? String.valueOf((int) values[idx[0]])
                                 : String.format(java.util.Locale.US, "%.1f", values[idx[0]])));
        render.run();
        b.setTextSize(16);
        b.setTextColor(0xFFFFCC66);
        b.setPadding(dp(4), dp(8), dp(4), dp(8));
        b.setOnClickListener(v -> {
            idx[0] = (idx[0] + 1) % values.length;
            nativePtcCommand("knob " + knob + " " + values[idx[0]]);
            render.run();
            Log.i(TAG, "KNOB " + knob + "=" + values[idx[0]]);
        });
        addPressFeedback(b);
        enhParent().addView(b);
        return b;
    }

    private LinearLayout enhParent() {
        return enhColumnTarget != null ? enhColumnTarget : controls;
    }

    /** Debug-only: request a death (ragdoll if enabled, vanilla otherwise). */
    void debugKill() {
        nativeDebugKill();
        Log.i(TAG, "KILL MARIO requested");
    }

    /** Open the decomp debug level select (also L+R+START in pause/title). */
    void openLevelSelect() {
        nativeLevelSelect();
        Log.i(TAG, "LEVEL SELECT requested");
    }

    /** Open level select, or warp straight to `level` when n >= 0. */
    void debugLevel(int level) {
        nativeDebugLevel(level);
        Log.i(TAG, "LEVEL " + level + " requested");
    }

    /** Portal Test Chamber command: run/reset/calibrate/place/stop. */
    void debugPtc(String action) {
        nativePtcCommand(action == null ? "" : action);
        Log.i(TAG, "PTC " + action);
    }

    /** Debug-only: run fixed 30 Hz sim ticks without display pacing. */
    void debugUncapped(int on) {
        uncappedTestOn = on != 0;
        nativeSetUncappedTest(uncappedTestOn ? 1 : 0);
        if (uncappedTestToggle != null) {
            uncappedTestToggle.setText("UNCAPPED TEST SIM: " + (uncappedTestOn ? "ON" : "OFF"));
            uncappedTestToggle.setTextColor(uncappedTestOn ? 0xFF66FF99 : 0xFF888888);
        }
        Log.i(TAG, "UNCAPPED_TEST=" + uncappedTestOn);
    }

    /** Fast-forward: queue N simulation ticks for the game thread. */
    void debugRunTicks(int n) {
        nativeRunTicks(n);
        Log.i(TAG, "RUNTICKS " + n + " requested");
    }

    /** Trailer presentation: hide the touch overlay and collapse the tuning panel. */
    void debugHideHud(int on) {
        forceHudHidden = on != 0;
        runOnUiThread(() -> {
            if (forceHudHidden) {
                collapseDebugPanelIfOpen();
            }
            if (touchOverlay != null) {
                touchOverlay.setControlsHidden(forceHudHidden);
                touchOverlay.invalidate();
            }
            if (tuningPanelView != null) {
                tuningPanelView.setVisibility(forceHudHidden ? View.GONE : View.VISIBLE);
            }
        });
        Log.i(TAG, "HUD hidden=" + forceHudHidden);
    }

    /** Trailer capture: tap the game's PCM output into files/audio_rec.wav. */
    void debugSetAudioRec(int on) {
        nativeSetAudioRec(on != 0 ? 1 : 0);
        Log.i(TAG, "AUDIOREC " + (on != 0 ? "start" : "stop"));
    }

    /** Hard pause: skip the level script (all simulation) but keep rendering. */
    void debugSetPause(int on) {
        hardPaused = on != 0;
        nativeSetHardPause(hardPaused ? 1 : 0);
        if (pauseLoopToggle != null) {
            pauseLoopToggle.setText("PAUSE GAME LOOP: " + (hardPaused ? "ON" : "OFF"));
            pauseLoopToggle.setTextColor(hardPaused ? 0xFF66FF99 : 0xFF888888);
        }
        Log.i(TAG, "PAUSE " + hardPaused);
    }

    /** Advance exactly N simulation ticks while hard-paused (frame stepping). */
    void debugStep(int n) {
        if (n < 1) n = 1;
        hardPaused = true;
        nativeSetHardPause(1);
        nativeStepTicks(n);
        Log.i(TAG, "STEP " + n);
    }

    /** HFR interpolation on/off (off gives one crisp state per captured frame). */
    void debugInterp(int on) {
        nativeSetHfrInterp(on != 0 ? 1 : 0);
        Log.i(TAG, "INTERP " + (on != 0 ? 1 : 0));
    }

    /** Log the trace of the last runticks batch, one line per log record. */
    void debugTrace() {
        String trace = nativePtcTrace();
        if (trace == null || trace.trim().isEmpty()) {
            Log.i(TAG, "PTC trace: (empty)");
            return;
        }
        Log.i(TAG, "PTC trace begin");
        int lines = 0;
        for (String line : trace.split("\n")) {
            if (!line.isEmpty()) {
                Log.i(TAG, line);
                lines++;
            }
        }
        Log.i(TAG, "PTC trace end lines=" + lines);
    }

    // ---- realtime shadow widgets (nativeSetShadow channel) ----

    private void addShadowSlider(String name, int min, int max, int initial, String unit,
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
                nativeSetShadow(key, v);
                if (key == 11) shadowDebug = v;
            }
            @Override public void onStartTrackingTouch(SeekBar sb) {}
            @Override public void onStopTrackingTouch(SeekBar sb) {}
        });
        addToGrid(label, bar);
        nativeSetShadow(key, initial);
    }

    private TextView addShadowToggle(String name, final int key, boolean initial) {
        final TextView b = new TextView(this);
        b.setText(name + ": " + (initial ? "ON" : "OFF"));
        b.setTextSize(16);
        b.setTextColor(initial ? 0xFF66FF99 : 0xFF888888);
        b.setPadding(dp(4), dp(8), dp(4), dp(8));
        b.setOnClickListener(v -> {
            boolean on = b.getCurrentTextColor() != 0xFF66FF99;
            b.setText(name + ": " + (on ? "ON" : "OFF"));
            b.setTextColor(on ? 0xFF66FF99 : 0xFF888888);
            nativeSetShadow(key, on ? 1 : 0);
            if (key == 0) {
                shadowOn = on;
            }
            refreshTabStyles();
            Log.i(TAG, name + "=" + on);
        });
        controls.addView(b);
        nativeSetShadow(key, initial ? 1 : 0);
        return b;
    }

    private TextView addShadowButton(String name, final Runnable action) {
        final TextView b = new TextView(this);
        b.setText(name);
        b.setTextSize(16);
        b.setTextColor(0xFF66CCFF);
        b.setPadding(dp(4), dp(10), dp(4), dp(10));
        b.setOnClickListener(v -> action.run());
        enhParent().addView(b);
        return b;
    }

    private void applyEnhState(TextView b, String name, boolean on) {
        b.setText(name + ": " + (on ? "ON" : "OFF"));
        b.setTextColor(on ? 0xFF66FF99 : 0xFF888888);
    }

    private void applyShadowEnable(TextView b, boolean on) {
        b.setText("SHADOW ENABLE: " + (on ? "ON" : "OFF"));
        b.setTextColor(on ? 0xFF66FF99 : 0xFF888888);
        shadowOn = on;
        refreshTabStyles();
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
        float dPitch = wrapPi(o[1] - gyroRef[1]); // tilt (SensorManager axis)
        float dRoll = wrapPi(o[2] - gyroRef[2]);  // twist (SensorManager axis)

        // The tablet is mounted landscape while SensorManager's orientation is
        // relative to its natural portrait axes, so its pitch arrives on the
        // roll channel (and vice versa). Swap them before handing the angles to
        // the renderer; the game side expects (pitch, yaw, roll).
        try {
            nativeSetGyro(dRoll, dAz, -dPitch);
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
        else if (key == 70) ssrtxOn = on;
        refreshTabStyles();
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
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;
        overlayAlphaPct = pct;
        if (touchOverlay != null) {
            touchOverlay.setOverlayAlpha(pct / 100f);
        }
        try {
            getSharedPreferences(PREFS_NAME, MODE_PRIVATE)
                    .edit().putInt(PREF_OVERLAY_OPACITY, pct).apply();
        } catch (Throwable t) {
            Log.w(TAG, "could not persist overlay opacity", t);
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

    /** Ratio + decimal percent for the Aspect slider, which is in 0.1% steps. */
    private static String aspectLabelTenths(int tenths) {
        double r = tenths / 1000.0;
        double[] vals = {1.0, 16.0 / 9.0, 4.0 / 3.0, 3.0 / 2.0, 21.0 / 9.0,
                         9.0 / 16.0, 3.0 / 4.0, 0.84};
        String[] names = {"1:1", "16:9", "4:3", "3:2", "21:9",
                          "9:16", "3:4", "21:25"};
        String ratio = null;
        for (int i = 0; i < vals.length; i++) {
            if (Math.abs(r - vals[i]) < 0.005) {
                ratio = names[i];
                break;
            }
        }
        if (ratio == null) {
            int w = tenths, h = 1000;
            int a = w, b = h;
            while (b != 0) {
                int t = a % b;
                a = b;
                b = t;
            }
            w /= a;
            h /= a;
            ratio = (w <= 200 && h <= 200) ? (w + ":" + h)
                    : String.format(java.util.Locale.US, "%.2f:1", r);
        }
        return String.format(java.util.Locale.US, "%.1f%% (%s)", tenths / 10.0, ratio);
    }

    private void addSlider(String name, int min, int max, int initial, String unit,
                           final int which) {
        final TextView label = new TextView(this);
        label.setTextColor(Color.WHITE);
        label.setTextSize(14);
        label.setText(which == 57
                ? name + ": " + aspectLabelTenths(initial)
                : name + ": " + initial + unit);

        SeekBar bar = new SeekBar(this);
        bar.setMax(max - min);
        bar.setProgress(initial - min);
        bar.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override
            public void onProgressChanged(SeekBar seekBar, int progress, boolean fromUser) {
                int value = progress + min;
                label.setText(which == 57
                        ? name + ": " + aspectLabelTenths(value)
                        : name + ": " + value + unit);
                if (which == 17) toeValuePct = value;
                stereoValues.put(which, value);
                if (stereoPictogram != null) stereoPictogram.invalidate();
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
        keyBars.put(which, bar);
        keyMins.put(which, min);
        if (which == 17) {
            toeSlider = bar;
            toeLabel = label;
        }
        barDefaults.add(new BarDefault(bar, initial - min));
        addToGrid(label, bar);
        stereoValues.put(which, initial);
        if (which >= 0 && which < 8) {
            nativeSetStereo(which, initial);
        } else if (which == 11) {
            applySourceWidthPercent(initial);
        } else {
            nativeSetStereoInt(which, initial);
        }
        if (stereoPictogram != null) stereoPictogram.invalidate();
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

    /** TOEAUTO owns the toe; grey the slider out and say why. */
    private void updateToeSliderState() {
        if (toeSlider == null || toeLabel == null) {
            return;
        }
        if (toeAutoOn) {
            toeSlider.setEnabled(false);
            toeSlider.setAlpha(0.35f);
            toeLabel.setAlpha(0.55f);
            toeLabel.setText("Toe in/out (toeauto on): -");
        } else {
            toeSlider.setEnabled(true);
            toeSlider.setAlpha(1f);
            toeLabel.setAlpha(1f);
            toeLabel.setText("Toe in/out: " + toeValuePct + " %");
        }
    }

    private void enterSliderDrag(View keep) {
        for (int i = 0; i < sliderRows.size(); i++) {
            View v = sliderRows.get(i);
            v.setVisibility(v == keep ? View.VISIBLE : View.INVISIBLE);
        }
        animatePanelBg(0xCC101010, 0x00000000);
    }

    /** Smoothly cross-fades the panel background colour. */
    private void animatePanelBg(int from, int to) {
        if (tuningPanelView == null) {
            return;
        }
        android.animation.ValueAnimator anim = android.animation.ValueAnimator.ofArgb(from, to);
        anim.setDuration(300);
        anim.addUpdateListener(a -> {
            if (tuningPanelView != null) {
                tuningPanelView.setBackgroundColor((int) a.getAnimatedValue());
            }
        });
        anim.start();
    }

    private void exitSliderDrag() {
        for (int i = 0; i < sliderRows.size(); i++) {
            sliderRows.get(i).setVisibility(View.VISIBLE);
        }
        animatePanelBg(0x00000000, PANEL_BG);
    }

    /** Inline - / + buttons for precise one-step slider changes. */
    private TextView stepButton(String text, final SeekBar bar, final int dir) {
        TextView b = new TextView(this);
        b.setText(text);
        b.setTextSize(20);
        b.setGravity(Gravity.CENTER);
        b.setTextColor(0xFFCCDDFF);
        b.setPadding(0, 0, 0, 0);
        b.setOnClickListener(v -> {
            int p = bar.getProgress() + dir;
            if (p < 0) p = 0;
            if (p > bar.getMax()) p = bar.getMax();
            bar.setProgress(p);
        });
        return b;
    }

    private void addToGrid(TextView label, SeekBar bar) {
        android.widget.LinearLayout col = new android.widget.LinearLayout(this);
        col.setOrientation(LinearLayout.VERTICAL);
        col.setPadding(0, 0, dp(8), 0);
        col.addView(label);

        LinearLayout barRow = new LinearLayout(this);
        barRow.setOrientation(LinearLayout.HORIZONTAL);
        barRow.setGravity(Gravity.CENTER_VERTICAL);
        TextView minus = stepButton("-", bar, -1);
        TextView plus = stepButton("+", bar, +1);
        barRow.addView(minus, new LinearLayout.LayoutParams(dp(34),
                ViewGroup.LayoutParams.WRAP_CONTENT));
        barRow.addView(bar, new LinearLayout.LayoutParams(0,
                ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        barRow.addView(plus, new LinearLayout.LayoutParams(dp(34),
                ViewGroup.LayoutParams.WRAP_CONTENT));
        col.addView(barRow);
        sliderRows.add(col);
        // While a slider is being dragged, hide every other slider row and make
        // the panel background transparent so the scene stays visible.
        final View colRef = col;
        bar.setOnTouchListener((v, ev) -> {
            switch (ev.getActionMasked()) {
                case android.view.MotionEvent.ACTION_DOWN:
                    enterSliderDrag(colRef);
                    break;
                case android.view.MotionEvent.ACTION_UP:
                case android.view.MotionEvent.ACTION_CANCEL:
                    exitSliderDrag();
                    break;
            }
            return false;
        });
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
            weaverLog("resize skipped w=" + w + " h=" + h + " asset=" + (asset != null));
            return;
        }
        if (!surfaceValid) {
            weaverLog("CreateEmptySurfaceForVideo " + w + "x" + h + " (surfaceValid=false)");
            try {
                // Video asset = the binding that CONSUMES a SurfaceTexture (the
                // picture asset samples its own texture id instead, which needs
                // rendering inside Leia's GL context).
                asset.CreateEmptySurfaceForVideo(w, h, new SurfaceTextureReadyCallback() {
                    @Override
                    public void onSurfaceTextureReady(SurfaceTexture texture) {
                        weaverLog("resize surface ready " + w + "x" + h + " texture=" + texture);
                        if (texture == null) {
                            weaverLog("resize surface ready got NULL texture");
                            return;
                        }
                        texture.setDefaultBufferSize(w, h);
                        SDLActivity.setOverrideSurface(new Surface(texture));
                        // mpv's Enable3D(): backlight and tracking together,
                        // once the video surface exists.
                        try {
                            sdk.enableBacklight(true);
                            sdk.enableFaceTracking(true);
                            weaverLog("ENABLE3D backlight=" + sdk.getBacklight()
                                    + " faceTracking=" + sdk.isFaceTrackingEnabled());
                        } catch (Throwable t) {
                            weaverError("enable3d failed", t);
                        }
                        weaverLog("SDL_OVERRIDE_SET (resize)");
                    }
                });
                surfaceValid = true;
            } catch (Throwable t) {
                weaverError("CreateEmptySurfaceForVideo (resize) failed", t);
            }
        } else if (leiaTexture != null) {
            leiaTexture.setDefaultBufferSize(w, h);
            weaverLog("resize surface buffer " + w + "x" + h + " (existing texture)");
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
                    fpsView.setText(nativeGetFps() + " fps");
                }
                if (camStatusView != null) {
                    camStatusView.setText("backlight=" + (sdk != null && sdk.getBacklight())
                            + "  facetrack=" + (sdk != null && sdk.isFaceTrackingStarted())
                            + "  cam=" + (nativeGetCamMode() != 0 ? "free" : "vanilla"));
                }
                if (touchOverlay != null) {
                    if (forceHudHidden) {
                        touchOverlay.setControlsHidden(true);
                    } else {
                        long sinceTouch = System.currentTimeMillis() - TouchOverlay.lastTouchMs;
                        int hwAge = nativeHwInputAgeMs();
                        if (hwAge < 1200 && sinceTouch > 1200) {
                            touchOverlay.setControlsHidden(true);
                        } else if (sinceTouch < 1200 || hwAge > 2500) {
                            touchOverlay.setControlsHidden(false);
                        }
                    }
                    touchOverlay.invalidate();   // refresh gun reticle / P1-P2
                }
                if (portalGunButton != null) {
                    String want = portalGunLabel();
                    if (!want.contentEquals(portalGunButton.getText())) {
                        portalGunButton.setText(want);
                    }
                }
                debugView.setTextColor(busyOn ? 0xFFFF5555 : 0xFFAAFFAA);
                String note = nativeGetDebugNote();
                debugView.setText((note == null || note.isEmpty() ? "" : note + "\n")
                        + (busyOn ? "BUSY - hands off  |  " : "FREE  |  ")
                        + BUILD_NAME + "  " + buildBranch());
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
    /** Gameplay enhancement toggles: 0 ragdoll, 1 hit ragdoll, 2 puppycam+. */
    private native void nativeSetEnh(int which, int value);
    private native int nativeGetEnh(int which);
    /** Realtime shadow map knobs (see SHADOW tab / gfx_opengl_ssfx_set). */
    private native void nativeSetShadow(int which, int value);
    private native int nativeGetShadow(int which);

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
    private native void nativeSetDebugNote(String text);
    private native String nativeGetDebugNote();

    void debugNote(String text) {
        nativeSetDebugNote(text);
    }

    static native void nativeOverlayInput(int active, int sx, int sy, int buttons);
    static native void nativeSetFpsMode(int mode);
    static native int nativeGetFpsMode();
    static native void nativeSetHfrInterp(int on);
    static native int nativeGetHfrInterp();
    static native int nativeGetHfrMtxCount();
    static native int nativeGetCamMode();
    static native void nativeSetCamMode2(int mode);
    static native int nativeGetCamMode2();
    static native void nativeSetFreecamMove(int on);
    static native void nativeFreecamNudge(float d);
    static native void nativeFreecamY(float d);
    static native int nativeGetMovekitState();
    static native int nativeGetJetpackFuel();
    static native void nativeMovekitUpgrade(int which);
    static native int nativeGetFuelTier();
    static native int nativeGetRefillTier();
    static native void nativeToggleCamMode();
    static native void nativeSetHeadPose(float x, float y, float z, int valid);
    static native void nativeSetGyro(float pitch, float yaw, float roll);
    static native int nativeGetHeadPose(int axis);
    static native void nativeDebugKill();
    static native void nativeLevelSelect();
    static native void nativeDebugLevel(int level);
    static native void nativeShootPortal(int which);
    static native int nativeGetPortalGun();
    static native void nativeToolToggle();
    static native int nativeGetTool();
    static native int nativeGetPortalCount();
    static native int nativeGetPortalDebug();
    static native int nativeGetMarioPos(int axis);
    static native int nativeGetPortalField(int which, int field);
    static native void nativeGivePortalGun();
    static native int nativeHwInputAgeMs();
    static native void nativeSetHardPause(int on);
    static native void nativeSetAudioRec(int on);
    static native void nativeStepTicks(int n);
    static native void nativeResetToIntro();
    static native void nativeDropPortalGun();
    static native void nativePtcCommand(String action);
    static native String nativePtcStatus();
    static native int nativePtcPhase();
    static native int nativePtcStar();
    static native void nativeRunTicks(int n);
    static native String nativePtcTrace();
    static native void nativeSetUncappedTest(int on);
    static native int nativeGetUncappedTest();
    static native void nativeOverlayStick2(int rx, int ry, int active);
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
        DebugLog.flush();
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
                if (touchOverlay != null) {
                    if (forceHudHidden) {
                        touchOverlay.setControlsHidden(true);
                    } else {
                        long sinceTouch = System.currentTimeMillis() - TouchOverlay.lastTouchMs;
                        int hwAge = nativeHwInputAgeMs();
                        if (hwAge < 1200 && sinceTouch > 1200) {
                            touchOverlay.setControlsHidden(true);
                        } else if (sinceTouch < 1200 || hwAge > 2500) {
                            touchOverlay.setControlsHidden(false);
                        }
                    }
                    touchOverlay.invalidate();   // refresh gun reticle / P1-P2
                }
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

    void debugResetToLogo() {
        resetToLogo();
    }

    private void resetToLogo() {
        hardPaused = false;
        nativeSetHardPause(0);
        nativeResetToIntro();
        if (pauseLoopToggle != null) {
            pauseLoopToggle.setText("PAUSE GAME LOOP: OFF");
            pauseLoopToggle.setTextColor(0xFF888888);
        }
        Log.i(TAG, "RESET TO LOGO");
    }

    void debugGiveGun() {
        nativeGivePortalGun();
        Log.i(TAG, "PORTAL gun given");
    }

    private String portalGunLabel() {
        try {
            return nativeGetPortalGun() != 0 ? "DROP PORTAL GUN" : "EQUIP PORTAL GUN";
        } catch (Throwable t) {
            return "EQUIP PORTAL GUN";
        }
    }

    /** ENH button: equip when empty-handed, drop when already holding it. */
    private void togglePortalGun() {
        boolean has;
        try {
            has = nativeGetPortalGun() != 0;
        } catch (Throwable t) {
            has = false;
        }
        if (has) {
            nativeDropPortalGun();
            Log.i(TAG, "PORTAL gun dropped");
        } else {
            nativeGivePortalGun();
            Log.i(TAG, "PORTAL gun given");
        }
        if (portalGunButton != null) {
            portalGunButton.setText(portalGunLabel());
        }
    }

    void debugFx(int k, int v) {
        nativeSetSSFX(k, v);
        Log.i(TAG, "FX set " + k + "=" + v);
    }

    void debugShadow(int k, int v) {
        nativeSetShadow(k, v);
        Log.i(TAG, "SHADOW set " + k + "=" + v);
    }

    /** Pulse raw overlay pad input (headless menu/gameplay driving). */
    void debugPad(int sx, int sy, int buttons, int ms) {
        final int dur = ms <= 0 ? 120 : ms;
        runOnUiThread(() -> {
            nativeOverlayInput(1, sx, sy, buttons);
            new android.os.Handler(getMainLooper()).postDelayed(
                    () -> nativeOverlayInput(1, sx, sy, 0), dur);
            Log.i(TAG, "PAD sx=" + sx + " sy=" + sy + " btn=0x"
                    + Integer.toHexString(buttons) + " ms=" + dur);
        });
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
            case "SSRTX":
                runOnUiThread(() -> {
                    ssrtxOn = !ssrtxOn;
                    if (ssrtxToggle != null) applyFxToggle(ssrtxToggle, "SSRTX ENABLE", 70, ssrtxOn);
                    else nativeSetSSFX(70, ssrtxOn ? 1 : 0);
                });
                break;
            case "FXDBG":
                runOnUiThread(() -> {
                    fxDebug = (fxDebug + 1) % 5;
                    nativeSetSSFX(31, fxDebug);
                    Log.i(TAG, "fxDebug=" + fxDebug);
                });
                break;
            case "RAGDOLL":
                runOnUiThread(() -> {
                    ragdollOn = !ragdollOn;
                    nativeSetEnh(0, ragdollOn ? 1 : 0);
                    if (ragdollToggle != null) applyEnhState(ragdollToggle, "DEATH RAGDOLL", ragdollOn);
                    Log.i(TAG, "RAGDOLL=" + ragdollOn);
                });
                break;
            case "HITRAG":
                runOnUiThread(() -> {
                    hitRagdollOn = !hitRagdollOn;
                    nativeSetEnh(1, hitRagdollOn ? 1 : 0);
                    if (hitRagdollToggle != null) applyEnhState(hitRagdollToggle, "HIT RAGDOLL", hitRagdollOn);
                    Log.i(TAG, "HITRAG=" + hitRagdollOn);
                });
                break;
            case "PPCAM":
                runOnUiThread(() -> {
                    ppcamOn = !ppcamOn;
                    nativeSetEnh(2, ppcamOn ? 1 : 0);
                    if (ppcamToggle != null) applyEnhState(ppcamToggle, "PUPPYCAM+", ppcamOn);
                    Log.i(TAG, "PPCAM=" + ppcamOn);
                });
                break;
            case "SHADOW":
                runOnUiThread(() -> {
                    boolean on = nativeGetShadow(0) == 0;
                    nativeSetShadow(0, on ? 1 : 0);
                    if (shadowEnableToggle != null) applyShadowEnable(shadowEnableToggle, on);
                    Log.i(TAG, "SHADOW=" + on);
                });
                break;
            case "SHADOWDBG":
                runOnUiThread(() -> {
                    shadowDebug = (shadowDebug + 1) % 3;
                    nativeSetShadow(11, shadowDebug);
                    if (shadowDebugButton != null) {
                        shadowDebugButton.setText("SHADOW VIEW "
                                + (shadowDebug == 0 ? "OFF" : shadowDebug == 1 ? "FACTOR" : "MAP"));
                    }
                    Log.i(TAG, "SHADOWDBG=" + shadowDebug);
                });
                break;
            case "TINT": nativeSetStereoInt(7, tintOn ? 0 : 1); tintOn = !tintOn; break;
            case "ZBUF": nativeSetStereoInt(15, zbufOn ? 0 : 1); zbufOn = !zbufOn; break;
            case "WIRE": nativeSetStereoInt(16, wireOn ? 0 : 1); wireOn = !wireOn; break;
            case "PERF2x": nativeSetStereoInt(12, perfOn ? 0 : 1); perfOn = !perfOn; break;
            case "SWAP": nativeSetStereoInt(4, swapOn ? 0 : 1); swapOn = !swapOn; break;
            case "SAME": nativeSetStereoInt(6, sameOn ? 0 : 1); sameOn = !sameOn; break;
            case "NOCULL": nativeSetStereoInt(19, nocullOn ? 0 : 1); nocullOn = !nocullOn; break;
            case "SPLAT":
                runOnUiThread(() -> {
                    splatMode = (splatMode + 1) % 4;
                    nativeSetStereoInt(60, splatMode);
                    Log.i(TAG, "SPLAT mode=" + splatMode
                            + (splatMode == 0 ? " off"
                               : splatMode == 1 ? " mario"
                               : splatMode == 2 ? " trees" : " all objects"));
                });
                break;
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
                updateToeSliderState();
                Log.i(TAG, "TOEAUTO=" + toeAutoOn);
                break;
            case "WALLRUN":
                nativeSetEnh(3, (nativeGetMovekitState() & 1) != 0 ? 0 : 1);
                Log.i(TAG, "WALLRUN -> mv=" + nativeGetMovekitState());
                break;
            case "GRAPPLE":
                nativeSetEnh(4, (nativeGetMovekitState() & 2) != 0 ? 0 : 1);
                Log.i(TAG, "GRAPPLE -> mv=" + nativeGetMovekitState());
                break;
            case "JETPACK":
                nativeSetEnh(5, (nativeGetMovekitState() & 4) != 0 ? 0 : 1);
                Log.i(TAG, "JETPACK -> mv=" + nativeGetMovekitState());
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
            case "UNCAP":
                runOnUiThread(() -> debugUncapped(uncappedTestOn ? 0 : 1));
                break;
            case "PTC":
                debugPtc("run");
                break;
            case "3D": runOnUiThread(() -> setStereoMode(!stereoOn)); break;
            case "TEST": runOnUiThread(this::showStaticTestImage); break;
            default: Log.w(TAG, "unknown toggle " + name);
        }
    }

    /** Debug-only: switch the panel to a requested physical refresh rate. */
    void debugDisplayHz(int hz) {
        runOnUiThread(() -> {
            Display d = getWindowManager().getDefaultDisplay();
            Display.Mode best = null;
            for (Display.Mode m : d.getSupportedModes()) {
                if (best == null
                        || Math.abs(m.getRefreshRate() - hz) < Math.abs(best.getRefreshRate() - hz)) {
                    best = m;
                }
            }
            if (best != null) {
                WindowManager.LayoutParams lp = getWindow().getAttributes();
                lp.preferredDisplayModeId = best.getModeId();
                getWindow().setAttributes(lp);
                Log.i(TAG, "DISPLAY_HZ requested=" + hz + " mode=" + best.getModeId()
                        + " hz=" + best.getRefreshRate());
            }
        });
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
            // Optional effects off as well (user request).
            if (aoToggle != null) applyFxToggle(aoToggle, "MXAO ENABLE", 1, false);
            if (dofToggle != null) applyFxToggle(dofToggle, "CINEMATIC DOF", 11, false);
            if (giToggle != null) applyFxToggle(giToggle, "SSGI ENABLE", 21, false);
            if (ssrtxToggle != null) applyFxToggle(ssrtxToggle, "SSRTX ENABLE", 70, false);
            wireOn = false;
            zbufOn = false;
            nativeSetStereoInt(16, 0);
            nativeSetStereoInt(15, 0);
            refreshTabStyles();
            Log.i(TAG, "RESET applied (effects off)");
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
                    + " ssrtx=" + nativeGetSSFX(70) + "/" + nativeGetSSFX(71)
                    + " ssrd=" + nativeGetSSFX(72) + " ssrs=" + nativeGetSSFX(73)
                    + " ssrr=" + nativeGetSSFX(74) + " ssrt=" + nativeGetSSFX(75)
                    + " dofauto=" + nativeGetSSFX(16) + " dofz=" + nativeGetSSFX(17)
                    + " stay=" + stayOn + " uipost=" + uiPostOn
                    + " cam=" + nativeGetCamMode() + " cam2=" + nativeGetCamMode2()
                    + " mv=" + nativeGetMovekitState() + " fuel=" + nativeGetJetpackFuel()
                    + " ftank=" + nativeGetFuelTier() + " frefill=" + nativeGetRefillTier()
                    + " ctlA=" + overlayAlphaPct
                    + " toeauto=" + toeAutoOn
                    + " gun=" + nativeGetPortalGun() + " portals=" + nativeGetPortalCount()
                    + " pdbg=" + nativeGetPortalDebug()
                    + " pos=" + nativeGetMarioPos(0) + "," + nativeGetMarioPos(1)
                    + "," + nativeGetMarioPos(2)
                    + " p0=" + nativeGetPortalField(0, 0) + "," + nativeGetPortalField(0, 1)
                    + "," + nativeGetPortalField(0, 2)
                    + " p1=" + nativeGetPortalField(1, 0) + "," + nativeGetPortalField(1, 1)
                    + "," + nativeGetPortalField(1, 2)
                    + " hdt=" + (hdtOn ? 1 : 0) + "/" + hdtStrength
                    + " gyro=" + (gyroOn ? 1 : 0) + "/" + gyroStrength
                    + " pivot=" + gyroPivot
                    + " head=" + nativeGetHeadPose(0) + "," + nativeGetHeadPose(1)
                    + "," + nativeGetHeadPose(2)
                    + " uipar=" + uiParallaxOn + " uibuf=" + uiBufOn
                    + " ragdoll=" + nativeGetEnh(0) + " hitrag=" + nativeGetEnh(1)
                    + " ppcam=" + nativeGetEnh(2)
                    + " shadow=" + nativeGetShadow(0) + "/" + nativeGetShadow(1)
                    + " str=" + nativeGetShadow(3) + " bias=" + nativeGetShadow(4)
                    + " soft=" + nativeGetShadow(6) + " taps=" + nativeGetShadow(7)
                    + " range=" + nativeGetShadow(8) + " dbg=" + nativeGetShadow(11));
            Log.i(TAG, nativePtcStatus());
            Log.i(TAG, "UNCAPPED_TEST=" + nativeGetUncappedTest());
        });
    }

    @Override
    protected void onDestroy() {
        DebugLog.flush();
        if (instance == this) instance = null;
        // asset lifecycle handled by the SDK (no close() in this AAR)
        LeiaSDK.shutdownSDK();
        super.onDestroy();
    }

    private class StereoPictogramView extends View {
        private final Paint line = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint txt = new Paint(Paint.ANTI_ALIAS_FLAG);

        StereoPictogramView() {
            super(sm64portActivity.this);
            txt.setColor(0xFFB8C0D0);
            txt.setTextSize(dp(9));
        }

        @Override
        protected void onDraw(Canvas c) {
            int w = getWidth();
            int h = getHeight();
            if (w <= 0 || h <= 0) return;
            c.drawColor(0xFF0E1117);

            line.setStyle(Paint.Style.STROKE);
            line.setStrokeWidth(1f);
            line.setColor(0xFFFF2FA8);
            c.drawRect(0.5f, 0.5f, w - 1f, h - 1f, line);

            int ipd = stereoValues.containsKey(0) ? stereoValues.get(0) : 300;
            int zero = stereoValues.containsKey(1) ? stereoValues.get(1) : 40;
            int depthPct = stereoValues.containsKey(2) ? stereoValues.get(2) : 50;
            int fov = stereoValues.containsKey(20) ? stereoValues.get(20) : 100;
            int dof = nativeGetSSFX(17);
            if (dof < 50) dof = 970;

            /* Left-side depth gauge: yellow line + marker mapped to the
             * Depth slider (-200..200 %). */
            {
                float gx = dp(9);
                float gtop = dp(12);
                float gbot = h - dp(22);
                float gt = (depthPct + 200f) / 400f;
                float gy = gbot - gt * (gbot - gtop);
                line.setColor(0x77FFD040);
                line.setStrokeWidth(2f);
                c.drawLine(gx, gtop, gx, gbot, line);
                fill.setStyle(Paint.Style.FILL);
                fill.setColor(0xFFFFD040);
                c.drawCircle(gx, gy, 3.5f * getResources().getDisplayMetrics().density, fill);
                txt.setColor(0xFFFFD040);
                c.drawText("depth range: " + depthPct + "%", gx + dp(7), gtop + dp(8), txt);
            }

            float eyeY = h - dp(24);
            float cx = w * 0.5f;
            float sep = dp(10) + (Math.min(ipd, 300) / 300f) * (w * 0.52f);
            float lx = cx - sep * 0.5f;
            float rx = cx + sep * 0.5f;
            float rad = dp(6);

            float zt = (zero + 100f) / 200f;
            float planeY = eyeY - dp(30) - zt * (h - dp(90));

            float toe = toeAutoOn
                    ? (float) Math.atan((Math.max(ipd, 1) * 0.5f) / Math.max(dof, 1))
                    : ((float) (stereoValues.containsKey(17) ? stereoValues.get(17) : 0) / 100f) * 0.25f;
            float focusY = eyeY - dp(40);
            if (toe > 0.001f) {
                float d = (sep * 0.5f) / (float) Math.tan(toe);
                if (d > h * 3f) d = h * 3f;
                focusY = eyeY - d;
            }
            if (focusY < dp(12)) focusY = dp(12);

            float halfFov = (fov / 100f) * 0.42f;
            float topY = dp(6);
            float[][] eyeX = {{lx, 0x2233CCFF}, {rx, 0x22FF8844}};
            fill.setStyle(Paint.Style.FILL);
            for (float[] exv : eyeX) {
                float ex = exv[0];
                float dx = cx - ex;
                float dy = topY - eyeY;
                float theta = (float) Math.atan2(dx, -dy);
                float R = (eyeY - topY) / Math.max(0.25f, (float) Math.cos(theta));
                float a1 = theta - halfFov;
                float a2 = theta + halfFov;
                float x1 = ex + R * (float) Math.sin(a1);
                float y1 = eyeY - R * (float) Math.cos(a1);
                float x2 = ex + R * (float) Math.sin(a2);
                float y2 = eyeY - R * (float) Math.cos(a2);
                fill.setColor((int) exv[1]);
                Path fp = new Path();
                fp.moveTo(ex, eyeY);
                fp.lineTo(x1, y1);
                fp.lineTo(x2, y2);
                fp.close();
                c.drawPath(fp, fill);
            }

            line.setColor(0xFFFF2FA8);
            line.setStrokeWidth(1.2f * getResources().getDisplayMetrics().density);
            c.drawLine(lx, eyeY, cx, focusY, line);
            c.drawLine(rx, eyeY, cx, focusY, line);

            line.setColor(0xFFFFD040);
            line.setStrokeWidth(1f);
            c.drawLine(dp(6), planeY, w - dp(6), planeY, line);
            txt.setColor(0xFFFFD040);
            c.drawText("zero parallax " + zero + "px", dp(8), planeY - dp(3), txt);

            /* UI layer depth: the debug "UI depth" slider (px of per-eye
             * shift).  Mapped top (far/behind) to the eye line (in front). */
            int uiDepth = stereoValues.containsKey(38) ? stereoValues.get(38) : 0;
            {
                float yTop = dp(10);
                float uy = eyeY - ((uiDepth + 60f) / 120f) * (eyeY - yTop);
                line.setColor(0xFF66D0FF);
                line.setStrokeWidth(1f);
                c.drawLine(dp(6), uy, w - dp(6), uy, line);
                txt.setColor(0xFF66D0FF);
                c.drawText("UI depth " + (uiDepth >= 0 ? "+" : "") + uiDepth + "px",
                        dp(8), uy - dp(3), txt);
            }

            fill.setStyle(Paint.Style.FILL);
            fill.setColor(0xFFFF2FA8);
            c.drawCircle(cx, focusY, dp(3), fill);
            txt.setColor(0xFFB8C0D0);
            c.drawText("focus", cx + dp(6), focusY + dp(3), txt);

            /* Yellow distance line from the eye midpoint to the focus point;
             * same camera-to-Mario distance used by C.DOF auto focus / auto toe. */
            line.setColor(0xFFFFD040);
            line.setStrokeWidth(2f);
            c.drawLine(cx, eyeY, cx, focusY, line);
            txt.setColor(0xFFFFD040);
            c.drawText("cam->Mario " + dof + " u", cx + dp(6),
                    (eyeY + focusY) * 0.5f, txt);

            fill.setColor(0xFFE8F0FF);
            c.drawCircle(lx, eyeY, rad, fill);
            c.drawCircle(rx, eyeY, rad, fill);
            txt.setColor(0xFF10131A);
            c.drawText("L", lx - dp(2), eyeY + dp(3), txt);
            c.drawText("R", rx - dp(2), eyeY + dp(3), txt);

            txt.setColor(0xFFB8C0D0);
            c.drawText(String.format(Locale.US,
                    "IPD %dpx   toe %.1f deg   FOV %d%%   focus %d",
                    ipd, Math.toDegrees(toe), fov, dof), dp(8), h - dp(6), txt);
        }
    }
}

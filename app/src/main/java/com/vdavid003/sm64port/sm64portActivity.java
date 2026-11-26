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
    /* Debug panel docking: false = bottom strip (default), true = left half. */
    private static final String PREF_PANEL_VERTICAL = "panel_vertical";
    /* QUICK DEV: cheap boot for the device loop (DEBUG -> SYS, off by default). */
    private static final String PREF_QUICK_DEV = "quick_dev";
    private static final String PREF_QUICK_DEV_LEVEL = "quick_dev_level";
    private static final String PREF_QUICK_DEV_MUTE = "quick_dev_mute";
    /* Caption ownership: true = Java overlay (crisp type), false = game font. */
    private static final String PREF_CAPTION_JAVA = "caption_java";
    private static final String PREF_CAPTION_SIZE = "caption_size_pct";
    private static final String PREF_CAPTION_OPACITY = "caption_opacity_pct";
    private static final int QUICK_DEV_DEFAULT_LEVEL = 38; // Portal Test Chamber
    private static final int LEVEL_MAX_PORT = 38;
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
    /* Mirrors of the toggles used for the tab indicator dots. */
    private boolean mbOn, lightCountOn, probeOn;
    private boolean gameHudOn = true, debugCapsOn = true, touchCtlOn = true;
    /* Performance monitor: on by default, like the FPS counter and graph. */
    private boolean perfMonitorOn = true, frameGraphOn = true, fpsCounterOn = true;
    private FrameGraphView frameGraph;
    /* Feedback outbox: one-tap capture under the FPS meter (see FeedbackOutbox). */
    private LinearLayout feedbackBar;
    private TextView feedbackButton;
    private int feedbackPollTick;
    private final float[] frameSamples = new float[120];
    private int frameSampleCount, frameSamplePos;
    private final android.os.Handler perfHandler =
            new android.os.Handler(android.os.Looper.getMainLooper());
    private final java.util.Map<String, Float> perfResults = new java.util.LinkedHashMap<>();
    private final java.util.Map<String, Long> perfTimes = new java.util.LinkedHashMap<>();
    private TextView perfTable;
    private int enhSelected;
    private LinearLayout layerList;
    private final java.util.List<String> layerOrder = new java.util.ArrayList<>();
    private android.widget.ScrollView scrollerRef;
    private TextView debugView;
    private LinearLayout tabContent;
    private final java.util.List<View> tabViews = new java.util.ArrayList<>();
    private final java.util.List<TextView> tabButtons = new java.util.ArrayList<>();
    private final java.util.List<TextView> subTabButtons = new java.util.ArrayList<>();
    private final java.util.List<TextView> tabDots = new java.util.ArrayList<>();
    private final java.util.List<Integer> topTabPages = new java.util.ArrayList<>();
    private LinearLayout stripsBox, fxStrip, cameraStrip, debugStripRow, aboutStrip;
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
    /* Top-level tabs and their page groups. Pages keep their historical
     * indices: 0 stereo, 1 views, 2 MXAO, 3 C.DOF, 4 SSGI, 5 SSRTX,
     * 6 camera settings, 7 debug, 8 shadow, 9 splat, 10 changelog,
     * 11 logs, 12 about general, 13 light, 14 crepuscular rays, 15 grade. */
    private static final int[] FX_PAGES = {0, 2, 3, 4, 5, 14, 8, 9, 13, 1, 15};
    private static final String[] FX_LABELS =
            {"CAMERA", "MXAO", "C.DOF", "SSGI", "SSRTX", "RAYS", "SHADOW", "SPLAT",
             "LIGHT", "VIEWS", "GRADE"};
    private static final int[] CAMERA_PAGES = {6, 0};
    private static final String[] CAMERA_LABELS = {"SETTINGS", "STEREO"};
    private static final int[] ABOUT_PAGES = {12, 10};
    private static final String[] ABOUT_LABELS = {"GENERAL", "CHANGELOG"};
    private static final String[] TOP_LABELS = {"FX", "DEBUG", "ABOUT"};
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
    private boolean raysOn = false;
    private TextView ssrtxToggle, raysToggle;
    /* Occluder punch-through mode (fx 274): 0 OFF, 1 DROPOUT, 2 ALPHA,
     * 3 CUTOUT.  The button also has to survive refreshTabStyles(). */
    private int occlMode = 0;
    private TextView occlButton;
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
    /* QUICK DEV boot shortcut (see DEBUG -> SYS). */
    private boolean quickDevOn, quickDevMute;
    private int quickDevLevel = QUICK_DEV_DEFAULT_LEVEL;
    private TextView quickDevToggle, quickDevLevelButton, quickDevMuteToggle;
    /* Captions: Java overlay owns the text by default (DEBUG -> SYS). */
    private CaptionOverlay captionOverlay;
    private boolean captionJavaOn = true;
    private int captionSizePct = 100, captionOpacityPct = 90;
    private TextView captionToggle;
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
    /* On-screen control layout editor (DEBUG > SYS > TOUCH LAYOUT). */
    private TextView layoutEditButton, layoutProfileButton, layoutLayoutStatus;
    // true = the assistant is driving the app: debug line turns red.
    private boolean busyOn;
    private final java.util.List<TextView> fitButtons = new java.util.ArrayList<>();
    private int fitMode = 0;
    private int modeIndex = 0;
    private int refreshIdx = 0;

    /** Cycle the panel refresh rate: default -> 60 -> 120 Hz. */
    private int fpsMode = 2;   // 120 Hz render rate by default
    private boolean interpOn = true;
    // Slow-motion debug controls (fx 241-243).
    private boolean simInterpOn = true;
    // Debug panel: slider drag mode, tab styling, changelog.
    private LinearLayout tuningPanelView;
    private LinearLayout topTabBar;
    /** Holds the first-order tab strip and the pages: tabs above pages in the
     *  bottom dock, tabs as a left column beside the pages in the vertical dock. */
    private LinearLayout panelNavRow;
    /** Weighted spacer that right-aligns SRC/RESET in the horizontal tab row. */
    private View topTabSpacer;
    /** Debug panel orientation: false = pinned across the bottom, true = left half. */
    private boolean panelVertical = false;
    /** Drawer header and its fixed-position children (re-laid out per orientation). */
    private android.widget.AbsoluteLayout panelHeader;
    private int lastHeaderWidth = -1;
    private TextView srcButton;
    private TextView orientButton;
    /** Every page ScrollView, so its max height can change with the orientation. */
    private final java.util.List<PageScroll> pageScrollers = new java.util.ArrayList<>();
    /** Insertion cursor for toggles that belong above the page's slider grid. */
    private int aboveGridIndex;
    /** Page-local tabs (LIGHT sub-pages) exposed to headless UI scripts. */
    private final java.util.List<Object[]> namedPanelButtons = new java.util.ArrayList<>();

    private void registerPanelButton(String name, TextView v) {
        namedPanelButtons.add(new Object[] { name, v });
    }

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
    private int camMode2 = 0;   // 0 vanilla, 1 bettercam, 2 puppycam, 3 custom, 4 first person, 5 free
    private static final String PREF_CAM_SENS_BAND = "cam_sens_band";
    private static final String PREF_CAM_SENS_TRIM = "cam_sens_trim";
    private static final String PREF_CAM_GROUND_CLIP = "cam_ground_clip";
    private static final float[] CAM_SENS_MULT = { 0.6f, 1.0f, 1.6f };
    private static final String[] CAM_SENS_NAMES = { "LOW", "MED", "HIGH" };
    private static final String[] CAM_GROUND_NAMES = { "COLLIDE", "CLIP" };
    private int camSensBand = 1;     // 0 LOW 0.6x, 1 MED 1.0x, 2 HIGH 1.6x
    private int camSensTrim = 100;   // slider percent inside the band
    private int camGroundClip = 0;   // 0 COLLIDE (default), 1 CLIP through floors/ceilings
    private TextView camSensValue;
    private TextView[] camSensPills = new TextView[CAM_SENS_NAMES.length];
    private TextView[] camGroundPills = new TextView[CAM_GROUND_NAMES.length];
    private TextView[] camModePills = new TextView[6];
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
    private String fpsLabel() {
        return fpsMode == 0 ? "30" : fpsMode == 1 ? "60"
                : fpsMode == 2 ? "120" : "\u221E";
    }

    private void cycleFpsMode() {
        fpsMode = (fpsMode + 1) % 4;
        try {
            nativeSetFpsMode(fpsMode);
            String label = fpsLabel();
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
        DebugLog.extraInfo = this::runtimeStateReport;
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

        /* Cam-stick curve, deadzone, R-follow settings: restore and push to
         * native before the panel is built (the panel sliders read them). */
        try {
            StickCurve.load(this);
            Log.i(TAG, "camstick restored: " + StickCurve.describe());
        } catch (Throwable t) {
            Log.w(TAG, "camstick restore failed", t);
        }
        /* Panel docking is a user preference and must survive a restart. */
        try {
            panelVertical = getSharedPreferences(PREFS_NAME, MODE_PRIVATE)
                    .getBoolean(PREF_PANEL_VERTICAL, false);
        } catch (Throwable t) {
            panelVertical = false;
        }
        Log.i(TAG, "panel orientation restored: " + (panelVertical ? "V" : "H"));

        /* QUICK DEV boots before the SDL thread starts native main(), so this is
         * the safe place to hand the persisted toggle to the engine. */
        try {
            android.content.SharedPreferences sp =
                    getSharedPreferences(PREFS_NAME, MODE_PRIVATE);
            quickDevOn = sp.getBoolean(PREF_QUICK_DEV, false);
            quickDevLevel = sp.getInt(PREF_QUICK_DEV_LEVEL, QUICK_DEV_DEFAULT_LEVEL);
            if (quickDevLevel < 1 || quickDevLevel > LEVEL_MAX_PORT) {
                quickDevLevel = QUICK_DEV_DEFAULT_LEVEL;
            }
            quickDevMute = sp.getBoolean(PREF_QUICK_DEV_MUTE, false);
        } catch (Throwable t) {
            quickDevOn = false;
            quickDevMute = false;
            quickDevLevel = QUICK_DEV_DEFAULT_LEVEL;
        }
        nativeSetQuickDev(quickDevOn ? 1 : 0, quickDevLevel);
        nativeSetQuickDevMute(quickDevMute ? 1 : 0);
        Log.i(TAG, "QUICK_DEV restored: on=" + quickDevOn + " level=" + quickDevLevel
                + " mute=" + quickDevMute);

        /* Caption ownership + overlay size/opacity. Pushed before native
         * main() starts, so the very first frame already obeys the choice. */
        try {
            android.content.SharedPreferences sp =
                    getSharedPreferences(PREFS_NAME, MODE_PRIVATE);
            captionJavaOn = sp.getBoolean(PREF_CAPTION_JAVA, true);
            captionSizePct = sp.getInt(PREF_CAPTION_SIZE, 100);
            captionOpacityPct = sp.getInt(PREF_CAPTION_OPACITY, 90);
        } catch (Throwable t) {
            captionJavaOn = true;
            captionSizePct = 100;
            captionOpacityPct = 90;
        }
        captionSizePct = Math.max(50, Math.min(200, captionSizePct));
        captionOpacityPct = Math.max(20, Math.min(100, captionOpacityPct));
        nativeSetCaptionMode(captionJavaOn ? 1 : 0);
        Log.i(TAG, "CAPTIONS restored: " + (captionJavaOn ? "JAVA" : "GAME FONT")
                + " size=" + captionSizePct + "% opacity=" + captionOpacityPct + "%");

        /* Camera sensitivity picker: band + slider trim, both persisted. */
        try {
            android.content.SharedPreferences sp =
                    getSharedPreferences(PREFS_NAME, MODE_PRIVATE);
            camSensBand = sp.getInt(PREF_CAM_SENS_BAND, 1);
            camSensTrim = sp.getInt(PREF_CAM_SENS_TRIM, 100);
        } catch (Throwable t) {
            camSensBand = 1;
            camSensTrim = 100;
        }
        if (camSensBand < 0) camSensBand = 0;
        if (camSensBand > 2) camSensBand = 2;
        if (camSensTrim < 10) camSensTrim = 10;
        if (camSensTrim > 300) camSensTrim = 300;
        Log.i(TAG, "CAM_SENS restored: band=" + CAM_SENS_NAMES[camSensBand]
                + " trim=" + camSensTrim + "%");

        /* Camera vs ground: COLLIDE (default) or CLIP through floors/ceilings.
         * Pushed to native by addCamGroundClipRow() when the panel is built. */
        try {
            camGroundClip = getSharedPreferences(PREFS_NAME, MODE_PRIVATE)
                    .getInt(PREF_CAM_GROUND_CLIP, 0);
        } catch (Throwable t) {
            camGroundClip = 0;
        }
        if (camGroundClip != 1) camGroundClip = 0;
        Log.i(TAG, "CAM_GROUND restored: " + CAM_GROUND_NAMES[camGroundClip]);

        try {
            initLeia();
        } catch (Throwable t) {
            // Never brick the game: fall back to SDL's normal flat output.
            weaverError("Leia init FAILED, staying flat (possible black screen cause)", t);
            SDLActivity.failOverride();
            /* The 2D fallback still gets the caption layer: a plain Android
             * view composites over SDL's SurfaceView in the same window. */
            if (mLayout instanceof ViewGroup) {
                runOnUiThread(() -> installCaptionOverlay((ViewGroup) mLayout));
            }
        }
    }

    /** Add the Java caption view once. Position is handled inside the view
     * from the native caption coordinates, so it needs no layout params. */
    private void installCaptionOverlay(ViewGroup layout) {
        if (captionOverlay != null || layout == null) {
            return;
        }
        captionOverlay = new CaptionOverlay(this);
        captionOverlay.setJavaMode(captionJavaOn);
        captionOverlay.setSizePct(captionSizePct);
        captionOverlay.setOpacityPct(captionOpacityPct);
        captionOverlay.setVisibility(forceHudHidden ? View.GONE : View.VISIBLE);
        layout.addView(captionOverlay, new RelativeLayout.LayoutParams(
                RelativeLayout.LayoutParams.MATCH_PARENT,
                RelativeLayout.LayoutParams.MATCH_PARENT));
        weaverLog("CaptionOverlay attached (" + (captionJavaOn ? "JAVA" : "GAME FONT")
                + " size=" + captionSizePct + "% opacity=" + captionOpacityPct + "%)");
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
        // Leia's default logging floods logcat and rotates out our markers, so
        // it is Error by default; the LOGS tab can switch it to Trace and
        // restart the app to capture a full verbose launch.
        final boolean verboseLeia = DebugLog.isVerbose(this);
        try {
            args.faceTrackingServerLogLevel = verboseLeia
                    ? com.leia.core.LogLevel.Trace
                    : com.leia.core.LogLevel.Error;
        } catch (Throwable ignored) {
        }
        weaverLog("InitArgs ready: enableFaceTracking=true permissionCheck=false logLevel="
                + (verboseLeia ? "Trace" : "Error"));

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
                leiaTexture = texture;
                texture.setDefaultBufferSize(SBS_W, SBS_H);
                weaverLog("SurfaceTexture default buffer set to " + SBS_W + "x" + SBS_H);
                // Kick the interlacer without detaching the SurfaceView (hiding
                // it blacks the display on newer firmware). pause/resume +
                // requestRender restarts the weave in place.
                leiaView.postDelayed(new Runnable() {
                    @Override public void run() {
                        try {
                            leiaView.onPause();
                            leiaView.onResume();
                            leiaView.requestRender();
                            reapplySourceSize();
                            weaverLog("interlacer kick: pause/resume + requestRender");
                        } catch (Throwable t) {
                            weaverError("interlacer kick failed", t);
                        }
                    }
                }, 1200);
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

            /* Caption text layer: above the game surface, below the touch
             * overlay and the debug panel. Screen-space, never per-eye. */
            installCaptionOverlay(layout);

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
            tuningPanelView.addOnLayoutChangeListener((v, l, t, r, b, ol, ot, orr, ob) -> {
                int nw = r - l;
                if (nw != lastHeaderWidth) {
                    lastHeaderWidth = nw;
                    layoutPanelHeader();
                }
            });
            applyPanelOrientation(false);
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

    /** Height of the panel with the drawer content in the given state. */
    private int measurePanel(boolean withContent) {
        if (panelVertical && withContent) {
            // The vertical dock is pinned top-to-bottom, so its open height is
            // the display height; there is nothing content-dependent to measure.
            setPanelBodyVisible(true);
            return panelFullHeight();
        }
        setPanelBodyVisible(withContent);
        android.view.ViewGroup.LayoutParams lp = tuningPanelView.getLayoutParams();
        int old = lp.height;
        lp.height = android.view.ViewGroup.LayoutParams.WRAP_CONTENT;
        int w = tuningPanelView.getWidth();
        if (w <= 0) w = getResources().getDisplayMetrics().widthPixels;
        tuningPanelView.measure(
                View.MeasureSpec.makeMeasureSpec(w, View.MeasureSpec.EXACTLY),
                View.MeasureSpec.makeMeasureSpec(0, View.MeasureSpec.UNSPECIFIED));
        int h = tuningPanelView.getMeasuredHeight();
        lp.height = old;
        return h;
    }

    /** Pixel height the vertical dock's MATCH_PARENT resolves against. */
    private int panelFullHeight() {
        int h = mLayout != null ? mLayout.getHeight() : 0;
        if (h <= 0) h = getResources().getDisplayMetrics().heightPixels;
        return h;
    }

    /** Show/hide the drawer body and keep the nav geometry in sync with it. */
    private void setPanelBodyVisible(boolean visible) {
        int vis = visible ? View.VISIBLE : View.GONE;
        if (tabContent != null) tabContent.setVisibility(vis);
        if (topTabBar != null) topTabBar.setVisibility(vis);
        if (panelNavRow != null) panelNavRow.setVisibility(vis);
        updateNavGeometry();
    }

    /** Animate the black panel background between two heights (ease-in-out). */
    private void animatePanelHeight(final int from, final int to, final boolean opening) {
        final android.view.ViewGroup.LayoutParams lp = tuningPanelView.getLayoutParams();
        lp.height = from;
        tuningPanelView.requestLayout();
        android.animation.ValueAnimator va =
                android.animation.ValueAnimator.ofInt(from, to);
        va.setDuration(240);
        va.setInterpolator(new android.view.animation.AccelerateDecelerateInterpolator());
        va.addUpdateListener(a -> {
            lp.height = (Integer) a.getAnimatedValue();
            tuningPanelView.requestLayout();
        });
        va.addListener(new android.animation.AnimatorListenerAdapter() {
            @Override
            public void onAnimationEnd(android.animation.Animator animation) {
                lp.height = (opening && panelVertical)
                        ? android.view.ViewGroup.LayoutParams.MATCH_PARENT
                        : android.view.ViewGroup.LayoutParams.WRAP_CONTENT;
                if (!opening) {
                    setPanelBodyVisible(false);
                } else {
                    setPanelBodyVisible(true);
                    tabContent.setAlpha(1f);
                }
                tuningPanelView.requestLayout();
                updateStereoPictogramVisibility();
                panelAnimating = false;
            }
        });
        va.start();
    }

    void expandDebugPanel() {
        if (tabContent == null || debugPanelOpen() || panelAnimating) {
            return;
        }
        if (hideButton != null) {
            hideButton.setText("\u25B4");
        }
        panelAnimating = true;
        int collapsed = tuningPanelView.getHeight() > 0
                ? tuningPanelView.getHeight() : measurePanel(false);
        // Freeze the current height, then measure the opened layout without
        // letting it jump before the animation starts.
        tuningPanelView.getLayoutParams().height = collapsed;
        tabContent.setAlpha(0f);
        int expanded = measurePanel(true);
        android.view.ViewGroup.LayoutParams lp = tuningPanelView.getLayoutParams();
        lp.height = collapsed;
        tuningPanelView.requestLayout();
        tabContent.animate().alpha(1f).setDuration(240)
                .setInterpolator(new android.view.animation.AccelerateDecelerateInterpolator())
                .start();
        animatePanelHeight(collapsed, expanded, true);
    }

    void collapseDebugPanelIfOpen() {
        if (!debugPanelOpen() || panelAnimating) {
            return;
        }
        if (hideButton != null) {
            hideButton.setText("\u25BE");
        }
        panelAnimating = true;
        int expanded = tuningPanelView.getHeight() > 0
                ? tuningPanelView.getHeight() : measurePanel(true);
        int collapsed = measurePanel(false);
        // Keep the content visible while the panel shrinks.
        setPanelBodyVisible(true);
        tabContent.animate().alpha(0f).setDuration(240)
                .setInterpolator(new android.view.animation.AccelerateDecelerateInterpolator())
                .start();
        animatePanelHeight(expanded, collapsed, false);
    }

    // ---- panel docking (horizontal strip / vertical left half) --------------

    private void refreshOrientButton() {
        if (orientButton == null) return;
        orientButton.setText(panelVertical ? "\u25AF VERT" : "\u25AD HORIZ");
        orientButton.setTextColor(panelVertical ? 0xFF66FF99 : 0xFFCCDDFF);
    }

    /** Switch the dock between the bottom strip and the left half (persisted). */
    void setPanelVertical(boolean vertical) {
        if (panelVertical == vertical) return;
        panelVertical = vertical;
        try {
            getSharedPreferences(PREFS_NAME, MODE_PRIVATE).edit()
                    .putBoolean(PREF_PANEL_VERTICAL, vertical).commit();
        } catch (Throwable t) {
            Log.w(TAG, "panel orientation not saved", t);
        }
        applyPanelOrientation(true);
        Log.i(TAG, "PANEL orientation=" + (vertical ? "V" : "H"));
    }

    private void setHeaderChild(View v, int x, int y, int w, int h) {
        if (v == null) return;
        android.view.ViewGroup.LayoutParams raw = v.getLayoutParams();
        if (!(raw instanceof android.widget.AbsoluteLayout.LayoutParams)) return;
        android.widget.AbsoluteLayout.LayoutParams lp =
                (android.widget.AbsoluteLayout.LayoutParams) raw;
        lp.x = x;
        lp.y = y;
        lp.width = w;
        lp.height = h;
        v.setLayoutParams(lp);
    }

    /** Header buttons follow the dock so nothing is clipped in vertical mode. */
    private void layoutPanelHeader() {
        if (panelHeader == null || tuningPanelView == null) return;
        int w = tuningPanelView.getWidth();
        if (w <= 0) w = getResources().getDisplayMetrics().widthPixels;
        int inner = w - tuningPanelView.getPaddingLeft()
                - tuningPanelView.getPaddingRight();
        if (!panelVertical) {
            // Historical fixed positions (automation taps these).
            setHeaderChild(hideButton, 16, 0, 120, 64);
            setHeaderChild(orientButton, 150, 0, 220, 64);
            setHeaderChild(modeButton, 1580, 0, 220, 64);
            setHeaderChild(fpsButton, 1820, 0, 220, 64);
            setHeaderChild(camCycleButton, 2060, 0, 380, 64);
        } else {
            setHeaderChild(hideButton, 16, 0, 120, 64);
            setHeaderChild(orientButton, 150, 0, 260, 64);
            int right = Math.max(dp(440), inner);
            setHeaderChild(camCycleButton, right - 370, 0, 280, 64);
            setHeaderChild(fpsButton, right - 590, 0, 220, 64);
            setHeaderChild(modeButton, right - 810, 0, 220, 64);
        }
    }

    /**
     * Width of the first-order tab column in the vertical dock. At the
     * 2560x1600 panel (density 2) 110 dp = 220 px: room for "ABOUT" plus its
     * enable dot and the RESET/SRC chips, and it leaves 1008 px (504 dp) for
     * the page, which still fits the two-column control grids.
     */
    private int tabStripWidth() {
        return dp(110);
    }

    /**
     * Lay out the nav row for the current dock. Horizontal: the tabs stay a row
     * above the pages (unchanged). Vertical: the same tabs become a column down
     * the dock's left edge and the page fills (and scrolls in) the rest of the
     * row. Selection state and dots are the chips' own, so nothing is duplicated.
     */
    private void updateNavGeometry() {
        if (panelNavRow == null || topTabBar == null || tabContent == null) return;
        boolean open = debugPanelOpen();
        LinearLayout.LayoutParams nav =
                (LinearLayout.LayoutParams) panelNavRow.getLayoutParams();
        LinearLayout.LayoutParams strip =
                (LinearLayout.LayoutParams) topTabBar.getLayoutParams();
        LinearLayout.LayoutParams content =
                (LinearLayout.LayoutParams) tabContent.getLayoutParams();
        if (panelVertical) {
            panelNavRow.setOrientation(LinearLayout.HORIZONTAL);
            // Open: the row takes everything under the header (weight fills it).
            // Collapsed: it wraps to nothing, so the collapsed bar stays short.
            nav.width = LinearLayout.LayoutParams.MATCH_PARENT;
            nav.height = open ? 0 : LinearLayout.LayoutParams.WRAP_CONTENT;
            nav.weight = open ? 1f : 0f;
            strip.width = tabStripWidth();
            strip.height = LinearLayout.LayoutParams.MATCH_PARENT;
            strip.weight = 0f;
            content.width = 0;
            content.height = LinearLayout.LayoutParams.MATCH_PARENT;
            content.weight = 1f;
            topTabBar.setOrientation(LinearLayout.VERTICAL);
        } else {
            panelNavRow.setOrientation(LinearLayout.VERTICAL);
            nav.width = LinearLayout.LayoutParams.MATCH_PARENT;
            nav.height = LinearLayout.LayoutParams.WRAP_CONTENT;
            nav.weight = 0f;
            strip.width = LinearLayout.LayoutParams.MATCH_PARENT;
            strip.height = LinearLayout.LayoutParams.WRAP_CONTENT;
            strip.weight = 0f;
            content.width = LinearLayout.LayoutParams.MATCH_PARENT;
            content.height = LinearLayout.LayoutParams.WRAP_CONTENT;
            content.weight = 0f;
            topTabBar.setOrientation(LinearLayout.HORIZONTAL);
        }
        panelNavRow.setLayoutParams(nav);
        topTabBar.setLayoutParams(strip);
        tabContent.setLayoutParams(content);
        // Horizontal: the weighted spacer right-aligns SRC/RESET. Vertical:
        // no weight, so SRC/RESET stack directly under the three top tabs
        // instead of drifting to the bottom of the full-height column.
        if (topTabSpacer != null) {
            android.view.ViewGroup.LayoutParams raw = topTabSpacer.getLayoutParams();
            if (raw instanceof LinearLayout.LayoutParams) {
                LinearLayout.LayoutParams sp = (LinearLayout.LayoutParams) raw;
                sp.width = 0;
                sp.height = 1;
                sp.weight = panelVertical ? 0f : 1f;
                topTabSpacer.setLayoutParams(sp);
            }
        }
    }

    /** Page fill: vertical dock stretches the visible page over the free
     *  height and scrolls inside it; horizontal keeps the wrapped pages. */
    private void applyPageLayoutParams() {
        for (int i = 0; i < tabViews.size(); i++) {
            View v = tabViews.get(i);
            android.view.ViewGroup.LayoutParams raw = v.getLayoutParams();
            if (!(raw instanceof LinearLayout.LayoutParams)) continue;
            LinearLayout.LayoutParams lp = (LinearLayout.LayoutParams) raw;
            lp.width = LinearLayout.LayoutParams.MATCH_PARENT;
            lp.height = panelVertical ? 0 : LinearLayout.LayoutParams.WRAP_CONTENT;
            lp.weight = panelVertical ? 1f : 0f;
            v.setLayoutParams(lp);
        }
        for (int i = 0; i < pageScrollers.size(); i++) {
            pageScrollers.get(i).fillHeight = panelVertical;
        }
    }

    /** Apply the dock geometry to the panel, its pages and the header. */
    private void applyPanelOrientation(boolean relayout) {
        if (tuningPanelView == null) return;
        int pad = dp(16);
        tuningPanelView.setPadding(pad, pad / 2, panelVertical ? dp(10) : pad * 3, pad / 2);
        android.view.ViewGroup.LayoutParams raw = tuningPanelView.getLayoutParams();
        if (raw instanceof android.widget.RelativeLayout.LayoutParams) {
            android.widget.RelativeLayout.LayoutParams lp =
                    (android.widget.RelativeLayout.LayoutParams) raw;
            lp.removeRule(android.widget.RelativeLayout.ALIGN_PARENT_BOTTOM);
            lp.removeRule(android.widget.RelativeLayout.ALIGN_PARENT_LEFT);
            lp.removeRule(android.widget.RelativeLayout.ALIGN_PARENT_TOP);
            lp.removeRule(android.widget.RelativeLayout.ALIGN_PARENT_RIGHT);
            if (panelVertical) {
                lp.width = Math.max(dp(500),
                        getResources().getDisplayMetrics().widthPixels / 2);
                // Pinned top-to-bottom while open: the page scrolls inside the
                // remaining height instead of the dock hugging the page. When
                // collapsed it still wraps to the header, so it never covers
                // half the screen as an empty black slab.
                lp.height = debugPanelOpen()
                        ? android.view.ViewGroup.LayoutParams.MATCH_PARENT
                        : android.view.ViewGroup.LayoutParams.WRAP_CONTENT;
                lp.addRule(android.widget.RelativeLayout.ALIGN_PARENT_LEFT);
                lp.addRule(android.widget.RelativeLayout.ALIGN_PARENT_TOP);
            } else {
                lp.width = android.view.ViewGroup.LayoutParams.MATCH_PARENT;
                lp.height = android.view.ViewGroup.LayoutParams.WRAP_CONTENT;
                lp.addRule(android.widget.RelativeLayout.ALIGN_PARENT_BOTTOM);
            }
            tuningPanelView.setLayoutParams(lp);
        }
        updateNavGeometry();
        applyPageLayoutParams();
        for (int i = 0; i < pageScrollers.size(); i++) {
            pageScrollers.get(i).setMaxHeight(pageMaxHeight());
        }
        layoutPanelHeader();
        refreshOrientButton();
        if (relayout) {
            tuningPanelView.requestLayout();
            updateStereoPictogramVisibility();
        }
        int pw = tuningPanelView.getLayoutParams().width;
        if (pw <= 0) pw = getResources().getDisplayMetrics().widthPixels;
        int inner = pw - tuningPanelView.getPaddingLeft()
                - tuningPanelView.getPaddingRight();
        Log.i(TAG, "PANEL dock=" + (panelVertical ? "V" : "H")
                + " width=" + pw
                + " strip=" + (panelVertical ? tabStripWidth() : 0)
                + " page=" + (panelVertical ? inner - tabStripWidth() : inner)
                + " height=" + (panelVertical
                        ? (debugPanelOpen() ? "full" : "wrap") : "wrap")
                + " pageMax=" + pageMaxHeight());
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

    /** Rolling frame-time strip: one bar per frame, target-period line. */
    private class FrameGraphView extends View {
        private final android.graphics.Paint p = new android.graphics.Paint(
                android.graphics.Paint.ANTI_ALIAS_FLAG);

        FrameGraphView() {
            super(sm64portActivity.this);
            setWillNotDraw(false);
            setClickable(false);
            setFocusable(false);
        }

        @Override
        protected void onDraw(android.graphics.Canvas c) {
            if (!frameGraphOn || frameSampleCount == 0) return;
            float w = getWidth(), h = getHeight();
            p.setStyle(android.graphics.Paint.Style.FILL);
            p.setColor(0x66000000);
            c.drawRect(0, 0, w, h, p);

            float peak = 33.4f;
            for (int i = 0; i < frameSampleCount; i++) {
                if (frameSamples[i] > peak) peak = frameSamples[i];
            }
            peak = Math.min(50f, Math.max(33.4f, peak * 1.1f));
            float targetMs = currentTargetMs();

            // reference line at the current target period
            if (targetMs > 0) {
                p.setColor(0x66FFCC66);
                p.setStrokeWidth(1f);
                p.setStyle(android.graphics.Paint.Style.STROKE);
                float y = h - (targetMs / peak) * h;
                c.drawLine(0, y, w, y, p);
            }

            p.setStyle(android.graphics.Paint.Style.FILL);
            float bw = w / 120f;
            for (int i = 0; i < frameSampleCount; i++) {
                int idx = (frameSamplePos - frameSampleCount + i + 120) % 120;
                float ms = frameSamples[idx];
                float bh = Math.min(h, (ms / peak) * h);
                p.setColor(ms <= targetMs * 1.2f ? 0xFF44FF66 : 0xFFFF6644);
                c.drawRect(i * bw, h - bh, (i + 1) * bw - 0.5f, h, p);
            }
        }
    }

    private float currentTargetMs() {
        int fps = gfxRateTargetFps();
        if (fps <= 0) {
            fps = fpsMode == 0 ? 30 : fpsMode == 1 ? 60 : fpsMode == 2 ? 120 : 0;
        }
        return fps > 0 ? 1000f / fps : 1000f / 60f;
    }

    private int gfxRateTargetFps() {
        try {
            return nativeGetRateState(1);
        } catch (Throwable t) {
            return 0;
        }
    }

    private void startFrameSampling() {
        perfHandler.postDelayed(new Runnable() {
            @Override
            public void run() {
                try {
                    int us = nativeFrameTimeUs();
                    if (perfMonitorOn && us > 0) {
                        frameSamples[frameSamplePos] = us / 1000f;
                        frameSamplePos = (frameSamplePos + 1) % 120;
                        if (frameSampleCount < 120) frameSampleCount++;
                        if (frameGraph != null) frameGraph.invalidate();
                    }
                } catch (Throwable ignored) {
                }
                perfHandler.postDelayed(this, 33);
            }
        }, 500);
    }

    /** Average frame time (ms) over a short window, sampled from the renderer. */
    private void sampleAverageMs(long settleMs, long windowMs,
                                 java.util.function.Consumer<Float> done) {
        final int n = Math.max(4, (int) (windowMs / 60));
        perfHandler.postDelayed(() -> {
            final long[] sum = {0};
            final int[] count = {0};
            for (int i = 0; i < n; i++) {
                perfHandler.postDelayed(() -> {
                    int us = nativeFrameTimeUs();
                    if (us > 0) {
                        sum[0] += us;
                        count[0]++;
                    }
                }, i * 60L);
            }
            perfHandler.postDelayed(() -> done.accept(
                    count[0] > 0 ? (sum[0] / (float) count[0]) / 1000f : 0f),
                    n * 60L + 40);
        }, settleMs);
    }

    private void addPerfRow(String name, final Runnable off, final Runnable on,
                            final java.util.function.BooleanSupplier isOn) {
        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(android.view.Gravity.CENTER_VERTICAL);
        final TextView label = new TextView(this);
        label.setText(name + ": press MEASURE");
        label.setTextSize(12);
        label.setTextColor(0xFFAACCFF);
        label.setPadding(dp(4), dp(1), dp(8), dp(1));
        row.addView(label, new LinearLayout.LayoutParams(0,
                ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        TextView btn = new TextView(this);
        btn.setText("MEASURE");
        btn.setTextSize(12);
        btn.setTextColor(0xFFFFCC66);
        btn.setPadding(dp(8), dp(3), dp(8), dp(3));
        btn.setBackgroundColor(0x22FFFFFF);
        btn.setOnClickListener(v -> measureEffect(name, label, off, on, isOn));
        row.addView(btn);
        /* Behind a disclosure at the bottom: A/B timing is rarely used and it
         * must not push the page's enable toggle below the sliders. */
        LinearLayout body = beginSection("PERF (A/B MEASURE)", false);
        body.addView(row, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        endSection();
    }

    private void measureEffect(final String name, final TextView label,
                               final Runnable off, final Runnable on,
                               final java.util.function.BooleanSupplier isOn) {
        if (!perfMonitorOn) {
            label.setText(name + ": perf monitor is off");
            return;
        }
        final boolean wasOn = isOn.getAsBoolean();
        label.setText(name + ": measuring...");
        // A/B: measure with the effect off, then on, then restore the state.
        off.run();
        sampleAverageMs(500, 700, offAvg -> {
            on.run();
            sampleAverageMs(500, 700, onAvg -> {
                float delta = onAvg - offAvg;
                perfResults.put(name, delta);
                perfTimes.put(name, android.os.SystemClock.elapsedRealtime());
                label.setText(String.format(java.util.Locale.US,
                        "%s: %+.2f ms  (off %.1f / on %.1f)", name, delta, offAvg, onAvg));
                if (!wasOn) off.run();
                refreshPerfTable();
            });
        });
    }

    private void refreshPerfTable() {
        if (perfTable == null) return;
        if (perfResults.isEmpty()) {
            perfTable.setText("no measurements yet");
            return;
        }
        StringBuilder b = new StringBuilder();
        for (java.util.Map.Entry<String, Float> e : perfResults.entrySet()) {
            long ago = (android.os.SystemClock.elapsedRealtime()
                    - perfTimes.getOrDefault(e.getKey(), 0L)) / 1000L;
            b.append(String.format(java.util.Locale.US, "%-12s %+.2f ms  (%ds ago)\n",
                    e.getKey(), e.getValue(), ago));
        }
        perfTable.setText(b.toString().trim());
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
        panelHeader = header;

        // Drawer toggle: a caret that flips when the panel opens/closes.
        TextView title = new TextView(this);
        title.setText("\u25BE");
        hideButton = title;
        title.setTextColor(Color.WHITE);
        title.setTextSize(22);
        title.setPadding(dp(6), 0, dp(10), 0);
        title.setOnClickListener(v -> {
            if (tabContent != null && tabContent.getVisibility() == View.VISIBLE) {
                collapseDebugPanelIfOpen();
            } else {
                expandDebugPanel();
            }
        });
        header.addView(title, abs(16, 0, 120, 64));

        /* Docking toggle next to the caret: bottom strip (horizontal) or the
         * left half of the screen (vertical). Persisted across launches. */
        orientButton = new TextView(this);
        orientButton.setTextSize(14);
        orientButton.setTextColor(0xFFCCDDFF);
        orientButton.setGravity(Gravity.CENTER);
        orientButton.setPadding(dp(4), 0, dp(4), 0);
        orientButton.setOnClickListener(v -> setPanelVertical(!panelVertical));
        header.addView(orientButton, abs(150, 0, 220, 64));
        refreshOrientButton();

        // Right-aligned cluster: [3D|2D] [FPS] [CAM].
        modeButton = new TextView(this);
        modeButton.setText(stereoOn ? "3D" : "2D");
        modeButton.setTextColor(stereoOn ? 0xFF66FF99 : 0xFFFF9966);
        modeButton.setTextSize(16);
        modeButton.setPadding(pad, pad / 2, pad, pad / 2);
        header.addView(modeButton, abs(1580, 0, 220, 64));
        modeButton.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                setStereoMode(!stereoOn);
            }
        });

        TextView fpsBtn = new TextView(this);
        fpsButton = fpsBtn;
        fpsBtn.setText("FPS " + fpsLabel());
        fpsBtn.setTextColor(0xFFCCCCCC);
        fpsBtn.setTextSize(16);
        fpsBtn.setPadding(pad, pad / 2, pad, pad / 2);
        header.addView(fpsBtn, abs(1820, 0, 220, 64));
        fpsBtn.setOnClickListener(v -> cycleFpsMode());

        TextView camBtn = new TextView(this);
        camCycleButton = camBtn;
        camBtn.setText("CAM: " + camModeName(camMode2));
        camBtn.setTextColor(0xFFCCCCCC);
        camBtn.setTextSize(14);
        camBtn.setPadding(pad, pad / 2, pad, pad / 2);
        header.addView(camBtn, abs(2060, 0, 380, 64));
        camBtn.setOnClickListener(v -> cycleCamMode());

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
        for (int i = 0; i < TOP_LABELS.length; i++) {
            addTopTab(tabBar, TOP_LABELS[i], i);
        }
        tabBar.addView(new View(this), new LinearLayout.LayoutParams(
                0, 1, 1f));       // spacer: RESET sits on the right edge
        topTabSpacer = tabBar.getChildAt(tabBar.getChildCount() - 1);
        TextView reset = new TextView(this);
        reset.setText("RESET");
        reset.setTextColor(0xFFFFCC66);
        reset.setTextSize(14);
        reset.setPadding(dp(10), dp(6), dp(10), dp(6));
        reset.setOnClickListener(v -> debugReset());
        tabBar.addView(reset);
        topTabBar = tabBar;

        /* Nav row: a vertical stack (tabs above pages) in the horizontal dock;
         * the vertical dock turns it into a row so the first-order tabs become
         * a column down the panel's left edge and the page gets the rest. */
        panelNavRow = new LinearLayout(this);
        panelNavRow.setOrientation(LinearLayout.VERTICAL);
        panelNavRow.addView(tabBar, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT));

        tabContent = new LinearLayout(this);
        tabContent.setOrientation(LinearLayout.VERTICAL);
        panelNavRow.addView(tabContent, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT));
        panel.addView(panelNavRow, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT));
        buildStrips(tabContent);

        // Scrolling container so sliders/buttons never run off screen. It is
        // only as tall as its content (capped by pageMaxHeight()), so a page
        // that fits needs no scroll at all.
        controls = new LinearLayout(this);
        controls.setOrientation(LinearLayout.VERTICAL);
        PageScroll scroller = new PageScroll();
        scroller.addView(controls, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT));
        scrollerRef = scroller;
        tabContent.addView(scroller, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT));
        tabViews.add(scroller);
        // Collapsed by default: the open panel covers the bottom of the screen
        // and swallowed taps meant for the overlay's START button. HIDE/SHOW
        // toggles all tab pages at once (see the hide listener below).
        setPanelBodyVisible(false);

        grid = newGrid(controls);
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
        // Task rule: an enable/show toggle is the first control on the page,
        // above every slider. GESTURES is the stereo page's enable switch.
        addAboveGrid(makePinchToggle(), ViewGroup.LayoutParams.WRAP_CONTENT);
        // SKYBOX FINE-TUNE: the skybox is scene geometry, not UI, so the two UI
        // sliders above never touch it.  These four dials (fx 370-373) do, and
        // only the skybox; neutral by default, RESET restores it.
        beginSection("SKYBOX FINE-TUNE", false);
        addSlider("Sky scale X", 50, 200, 100, " %", 370);
        addSlider("Sky scale Y", 50, 200, 100, " %", 371);
        addSlider("Sky offset X", -100, 100, 0, " %", 372);
        addSlider("Sky offset Y", -100, 100, 0, " %", 373);
        {
            LinearLayout skyRow = addRow();
            TextView skyReset = new TextView(this);
            skyReset.setText("\u25B8 RESET SKYBOX");
            skyReset.setTextSize(14);
            skyReset.setTextColor(0xFFFF9966);
            skyReset.setPadding(dp(4), dp(6), dp(4), dp(6));
            skyReset.setOnClickListener(v -> {
                setCamValue(370, 100);
                setCamValue(371, 100);
                setCamValue(372, 0);
                setCamValue(373, 0);
            });
            addCompact(skyRow, skyReset);
        }
        endSection();
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

        frameGraph = new FrameGraphView();
        android.widget.RelativeLayout.LayoutParams graphParams =
                new android.widget.RelativeLayout.LayoutParams(dp(220), dp(78));
        graphParams.topMargin = dp(30);
        mLayout.addView(frameGraph, graphParams);
        startFrameSampling();
        buildFeedbackBar();

        /* Stereo toggles: a weighted row (not absolute pixels) so they stay
         * reachable in the vertical dock as well. */
        LinearLayout toggles = new LinearLayout(this);
        toggles.setOrientation(LinearLayout.HORIZONTAL);
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
            b.setGravity(Gravity.CENTER);
            b.setPadding(dp(2), dp(4), dp(2), dp(4));
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
            toggles.addView(b, new LinearLayout.LayoutParams(0, dp(34), 1f));
            Log.i(TAG, "BTN " + t[0] + " row");
            tglIndex++;
        }
        addAboveGrid(toggles, dp(34));

        buildFxTabs();
        selectPage(0);

        /* SRC lives in the tab bar next to RESET: the old header slot at
         * x=2440 was clipped by the panel's right padding and unreachable. */
        srcButton = new TextView(this);
        srcButton.setText("SRC");
        srcButton.setTextColor(0xFFCCCCCC);
        srcButton.setTextSize(14);
        srcButton.setPadding(dp(10), dp(6), dp(10), dp(6));
        tabBar.addView(srcButton, tabBar.getChildCount() - 1);
        srcButton.setOnClickListener(new View.OnClickListener() {
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

        // (drawer toggling now lives on the caret above)
        return panel;
    }

    // ---- Debug panel tabs ---------------------------------------------------

    /** Small dot used as an "enabled" indicator on tabs and sub-tabs. */
    private TextView addDot(LinearLayout row) {
        TextView dot = new TextView(this);
        dot.setText("\u25CF");
        dot.setTextSize(8);
        dot.setTextColor(0xFF555555);
        dot.setPadding(dp(2), 0, dp(6), 0);
        row.addView(dot);
        return dot;
    }

    /** Text + dot chip used for every tab row. */
    private TextView addChip(LinearLayout bar, String label, int textSize, Runnable onClick) {
        LinearLayout chip = new LinearLayout(this);
        chip.setOrientation(LinearLayout.HORIZONTAL);
        chip.setGravity(Gravity.CENTER_VERTICAL);
        TextView b = new TextView(this);
        b.setText(label);
        b.setTextSize(textSize);
        b.setTextColor(0xFF888888);
        b.setPadding(dp(12), dp(6), dp(2), dp(6));
        TextView dot = addDot(chip);
        chip.addView(b, 0);
        chip.setOnClickListener(v -> onClick.run());
        addPressFeedback(b);
        bar.addView(chip);
        tabDots.add(dot);
        return b;
    }

    private void addTopTab(LinearLayout bar, String label, final int topIndex) {
        final int idx = tabButtons.size();
        TextView b = addChip(bar, label, 15, () -> {
            if (tabContent != null && tabContent.getVisibility() != View.VISIBLE) {
                expandDebugPanel();
            }
            selectTopTab(idx);
        });
        tabButtons.add(b);
    }

    private void addSubTab(LinearLayout bar, String label, final Runnable onClick) {
        TextView b = addChip(bar, label, 13, onClick);
        subTabButtons.add(b);
    }

    /** Second/third-level strips live inside tabContent, so collapsing the
     *  drawer hides them and the collapsed bar stays short. */
    private void buildStrips(LinearLayout parent) {
        LinearLayout col = new LinearLayout(this);
        col.setOrientation(LinearLayout.VERTICAL);
        col.setPadding(dp(4), 0, dp(4), dp(2));

        fxStrip = new FlowBar();
        for (int i = 0; i < FX_LABELS.length; i++) {
            final int page = "CAMERA".equals(FX_LABELS[i]) ? 6 : FX_PAGES[i];
            addSubTab(fxStrip, FX_LABELS[i], () -> selectPage(page));
        }
        col.addView(fxStrip);

        cameraStrip = new FlowBar();
        cameraStrip.setPadding(dp(12), 0, 0, 0);
        for (int i = 0; i < CAMERA_LABELS.length; i++) {
            final int page = CAMERA_PAGES[i];
            addSubTab(cameraStrip, CAMERA_LABELS[i], () -> selectPage(page));
        }
        col.addView(cameraStrip);

        debugStripRow = new FlowBar();
        for (int i = 0; i < 4; i++) {
            final int page = i;
            addSubTab(debugStripRow, new String[]{"MOVE", "PORTAL", "BALL", "SYS"}[i], () -> {
                if (selectedPage != 7) selectPage(7);
                showEnhPage(page);
            });
        }
        addSubTab(debugStripRow, "LOGS", () -> selectPage(11));
        col.addView(debugStripRow);

        aboutStrip = new FlowBar();
        for (int i = 0; i < ABOUT_LABELS.length; i++) {
            final int page = ABOUT_PAGES[i];
            addSubTab(aboutStrip, ABOUT_LABELS[i], () -> selectPage(page));
        }
        col.addView(aboutStrip);

        for (int i = 0; i < col.getChildCount(); i++) {
            col.getChildAt(i).setVisibility(View.GONE);
        }
        parent.addView(col, 0, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        stripsBox = col;
    }

    private void selectTopTab(int topIndex) {
        switch (topIndex) {
            case 0: selectPage(lastEffectPage); break;
            case 1: selectPage(selectedPage == 11 || selectedPage == 7 ? selectedPage : 7); break;
            default: selectPage(selectedPage == 10 || selectedPage == 12 ? selectedPage : 12); break;
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
        boolean isFx = contains(FX_PAGES, page);
        boolean isCamera = contains(CAMERA_PAGES, page);
        boolean isDebug = (page == 7 || page == 11);
        boolean isAbout = contains(ABOUT_PAGES, page);
        if (isFx) lastEffectPage = page;
        selectedTab = isFx ? 0 : (isDebug ? 1 : 2);
        if (fxStrip != null) fxStrip.setVisibility(isFx ? View.VISIBLE : View.GONE);
        if (cameraStrip != null) {
            cameraStrip.setVisibility(isCamera ? View.VISIBLE : View.GONE);
        }
        if (debugStripRow != null) {
            debugStripRow.setVisibility(isDebug ? View.VISIBLE : View.GONE);
        }
        if (aboutStrip != null) {
            aboutStrip.setVisibility(isAbout ? View.VISIBLE : View.GONE);
        }
        refreshTabStyles();
        updateStereoPictogramVisibility();
    }

    private static boolean contains(int[] pages, int page) {
        for (int p : pages) {
            if (p == page) return true;
        }
        return false;
    }

    /** "This tab has a feature enabled" indicator. */
    private boolean pageOn(int page) {
        switch (page) {
            case 2: return aoOn;
            case 3: return dofOn;
            case 4: return giOn;
            case 5: return ssrtxOn;
            case 14: return raysOn;
            case 8: return shadowOn;
            case 9: return splatMode != 0;
            case 13: return mbOn || lightCountOn || probeOn;
            case 15: return nativeGetSSFX(280) != 0;
            case 1: return zbufOn || wireOn || fxDebug != 0;
            case 0: case 6: return hdtOn || gyroOn;
            case 7: return !gameHudOn || !debugCapsOn || !touchCtlOn;
            case 11: return false;
            default: return false;
        }
    }

    /** Selected tab: white + underline. Enabled feature: green. */
    private void setTabLook(TextView b, boolean selected) {
        b.setTextColor(selected ? 0xFFFFFFFF : 0xFF888888);
        int flags = b.getPaintFlags();
        if (selected) {
            flags |= android.graphics.Paint.UNDERLINE_TEXT_FLAG;
        } else {
            flags &= ~android.graphics.Paint.UNDERLINE_TEXT_FLAG;
        }
        b.setPaintFlags(flags);
    }

    private void setDot(int index, boolean on) {
        if (index < 0 || index >= tabDots.size()) return;
        tabDots.get(index).setTextColor(on ? 0xFF44FF66 : 0xFF555555);
    }

    private void refreshTabStyles() {
        for (int i = 0; i < tabButtons.size(); i++) {
            setTabLook(tabButtons.get(i), i == selectedTab);
            boolean on = false;
            if (i == 0) {
                for (int p : FX_PAGES) on |= pageOn(p);
            } else if (i == 1) {
                on = pageOn(7) || pageOn(11);
            } else {
                for (int p : ABOUT_PAGES) on |= pageOn(p);
            }
            setDot(i, on);
        }
        int k = tabButtons.size();
        for (int i = 0; i < FX_LABELS.length; i++) {
            setTabLook(subTabButtons.get(i), selectedPage == FX_PAGES[i]);
            setDot(k++, pageOn(FX_PAGES[i]));
        }
        int camStart = FX_LABELS.length;
        for (int i = 0; i < CAMERA_LABELS.length; i++) {
            setTabLook(subTabButtons.get(camStart + i), selectedPage == CAMERA_PAGES[i]);
            setDot(k++, pageOn(CAMERA_PAGES[i]));
        }
        int dbgStart = camStart + CAMERA_LABELS.length;
        for (int i = 0; i < 5; i++) {
            boolean sel = (i < 4) ? (selectedPage == 7 && enhSelected == i) : (selectedPage == 11);
            setTabLook(subTabButtons.get(dbgStart + i), sel);
            setDot(k++, i < 4 ? pageOn(7) : pageOn(11));
        }
        int aboutStart = dbgStart + 5;
        for (int i = 0; i < ABOUT_LABELS.length; i++) {
            setTabLook(subTabButtons.get(aboutStart + i), selectedPage == ABOUT_PAGES[i]);
            setDot(k++, pageOn(ABOUT_PAGES[i]));
        }
    }

    /** Creates (and appends) a new scrollable tab page and makes it current. */
    private LinearLayout beginTab() {
        PageScroll sc = new PageScroll();
        LinearLayout content = new LinearLayout(this);
        content.setOrientation(LinearLayout.VERTICAL);
        sc.addView(content, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT));
        tabContent.addView(sc, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT));
        tabViews.add(sc);
        controls = content;

        grid = newGrid(content);
        return content;
    }

    private String splatShapeLabel() {
        return "\u2248 " + (splatShape == 1 ? "SPLAT SHAPE: PAINT STROKES"
                                            : "SPLAT SHAPE: RADIAL");
    }

    private String splatTargetLabel() {
        switch (splatMode) {
            case 1: return "\u25C9 SPLAT TARGET: MARIO";
            case 2: return "\u25C9 SPLAT TARGET: TREES";
            case 3: return "\u25C9 SPLAT TARGET: ALL OBJECTS";
            default: return "\u25C9 SPLAT TARGET: OFF";
        }
    }

    private void buildFxTabs() {
        // --- VIEWS: debug view selector + a small reorderable layer stack ---
        beginTab();
        grid = newGrid(controls);
        {
            LinearLayout r = addRow();
            addDebugCell(r, "OFF", 0);
            addDebugCell(r, "AO", 1);
            addDebugCell(r, "CoC", 2);
            addDebugCell(r, "GI", 3);
            addDebugCell(r, "SSRTX", 4);
        }
        {
            LinearLayout r = addRow();
            addDebugCell(r, "LIGHTS", 5);
            addDebugCell(r, "L-ACCUM", 6);
            addDebugCell(r, "PROBE VIS", 7);
            addDebugCell(r, "L-DIST", 8);
            addDebugCell(r, "MOTION VECTORS", 9);
        }
        {
            TextView h = new TextView(this);
            h.setText("LAYERS (top draws last)");
            h.setTextSize(13);
            h.setTextColor(0xFF66CCFF);
            h.setPadding(dp(4), dp(8), dp(4), dp(2));
            controls.addView(h);
        }
        layerList = new LinearLayout(this);
        layerList.setOrientation(LinearLayout.VERTICAL);
        controls.addView(layerList);
        if (layerOrder.isEmpty()) {
            layerOrder.add("ZBUF");
            layerOrder.add("WIRE");
        }
        rebuildLayerList();
        beginSection("PERF (A/B AVERAGE)", false);
        perfTable = new TextView(this);
        perfTable.setTextSize(12);
        perfTable.setTextColor(0xFFAACCFF);
        perfTable.setTypeface(android.graphics.Typeface.MONOSPACE);
        perfTable.setPadding(dp(4), dp(2), dp(4), dp(4));
        currentParent().addView(perfTable);
        refreshPerfTable();
        {
            LinearLayout r = addRow();
            addActionCell(r, "REFRESH PERF", this::refreshPerfTable);
            addActionCell(r, "CLEAR PERF", () -> {
                perfResults.clear();
                perfTimes.clear();
                refreshPerfTable();
            });
        }
        endSection();

        // --- MXAO-style ambient occlusion ---
        beginTab();
        addPerfRow("MXAO", () -> nativeSetSSFX(1, 0), () -> nativeSetSSFX(1, 1), () -> aoOn);
        aoToggle = addFxToggle("MXAO ENABLE", 1, aoOn);
        addFxDebugButton("SHOW AO", 1);
        addFxSlider("Intensity", 0, 300, 100, " %", 2);
        addFxSlider("Radius", 10, 500, 120, " u", 3);
        addFxSlider("Samples", 4, 32, 12, "", 4);
        addFxSlider("Bias", 0, 100, 20, " u", 5);
        addFxSlider("Power", 50, 400, 150, " %", 6);
        beginSection("AO RESOLUTION (ADVANCED)", false);
        addFxCycle("AO PATH", 390, new String[] { "FULL RES", "LOW RES" },
                   nativeGetSSFX(390));
        addIntSlider("Low-res 1/n", 2, 4, 2, v -> debugFx(391, v));
        addIntSlider("Rotate per pixel 0/1", 0, 1, 0, v -> debugFx(392, v));
        addIntSlider("Upsample 0=bilat 1=bilin", 0, 1, 0, v -> debugFx(393, v));
        addIntSlider("Sky skip 0/1", 0, 1, 1, v -> debugFx(394, v));
        addIntSlider("Tap trig 0=rot 1=sin/cos", 0, 1, 0, v -> debugFx(395, v));
        addIntSlider("Stage probe 0/1/2", 0, 2, 0, v -> debugFx(396, v));
        endSection();

        // --- Cinematic DOF ---
        beginTab();
        addPerfRow("C.DOF", () -> nativeSetSSFX(11, 0), () -> nativeSetSSFX(11, 1), () -> dofOn);
        dofToggle = addFxToggle("CINEMATIC DOF", 11, dofOn);
        addFxToggle("AUTO FOCUS", 16, true);   // tracks Mario by default
        addFxDebugButton("SHOW CoC", 2);
        addFxSlider("Focus", 50, 5000, 800, " u", 12);
        addFxSlider("Aperture", 0, 300, 100, " %", 13);
        addFxSlider("Max blur", 0, 40, 12, " px", 14);
        addFxSlider("Samples", 4, 32, 16, "", 15);

        // --- Screen-space GI (SSGI/RTGI-style) ---
        beginTab();
        addPerfRow("SSGI", () -> nativeSetSSFX(21, 0), () -> nativeSetSSFX(21, 1), () -> giOn);
        giToggle = addFxToggle("SSGI ENABLE", 21, giOn);
        addFxDebugButton("SHOW GI", 3);
        addFxSlider("Intensity", 0, 300, 100, " %", 22);
        addFxSlider("Radius", 10, 500, 150, " u", 23);
        addFxSlider("Steps", 2, 12, 6, "", 24);
        addFxSlider("Rays", 2, 8, 4, "", 25);
        // Reduced-resolution GI: 4 = quarter res (default), 1 = the original
        // full-res in-shader loop for A/B.
        addFxSlider("Resolution 1/", 1, 4, 4, "", 400);
        addFxSlider("Blur", 0, 3, 1, "", 401);
        addFxToggle("Roto jitter", 402, true);
        addFxSlider("Sky skip", 0, 100, 95, " %", 404);
        addFxToggle("Normal reject", 405, false);

        // --- SSRTX: screen-space ray-traced reflections ---
        beginTab();
        addPerfRow("SSRTX", () -> nativeSetSSFX(70, 0), () -> nativeSetSSFX(70, 1), () -> ssrtxOn);
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
        addAboveGrid(camTop, ViewGroup.LayoutParams.WRAP_CONTENT,
                ViewGroup.LayoutParams.MATCH_PARENT);

        controls.addView(makePinchToggle(), 0);
        addCamSensPicker(controls);
        addAboveGrid(makePinchToggle(), ViewGroup.LayoutParams.WRAP_CONTENT,
                ViewGroup.LayoutParams.MATCH_PARENT);
        addCamToggle("WALL NOCLIP", 52, true);
        addCamGroundClipRow();

        addCamSlider("Cam distance", 1, 1500, 700, " u", 46);
        addCamSlider("FOV %", 1, 400, 100, " %", 20);
        addCamSlider("3D height", 50, 150, 100, " %", 50);
        addCamSlider("3D width", 50, 150, 50, " %", 51);
        addCamSlider("Cam height", 0, 400, 110, " u", 47);
        addCamSlider("Cam recentre", 0, 200, 50, " %", 48);
        addCamSlider("Cam sensitivity", 10, 300, camSensTrim, " %", 49);
        addCamSlider("HDT strength", 0, 200, hdtStrength, " %", 43);
        addCamSlider("Pivot height", 0, 200, gyroPivot, " cm", 45);
        addCamSlider("Gyro strength", 0, 200, gyroStrength, " %", 44);
        addCamStickControls();
        addCamSlider("Cam sensitivity", 10, 300, 100, " %", 49);
        addOcclusionControls();
        beginSection("TRACKING TUNING", false);
        addCamSlider("HDT strength", 0, 200, hdtStrength, " %", 43);
        addCamSlider("Pivot height", 0, 200, gyroPivot, " cm", 45);
        addCamSlider("Gyro strength", 0, 200, gyroStrength, " %", 44);
        endSection();
        addCamButton("ASPECT TEST", () -> nativePtcCommand("aspecttest"));
        camStatusView = new TextView(this);
        camStatusView.setTextSize(14);
        camStatusView.setTextColor(0xFFAACCFF);
        camStatusView.setPadding(dp(4), dp(10), dp(4), dp(4));
        controls.addView(camStatusView);

        // --- DEBUG: MOVE / PORTAL / BALL / SYS (+ LOGS in the strip) ---
        beginTab();
        {
            final String[] pageNames = { "MOVE", "PORTAL", "BALL", "SYS" };
            enhPages = new LinearLayout[pageNames.length];
            enhPageTabs = new TextView[pageNames.length];
            enhPageColumns = new LinearLayout[pageNames.length][3];
            for (int i = 0; i < pageNames.length; i++) {
                final int page = i;

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
        /* Wall-run jump-off shape + release window (fx 320-323). */
        addEnhSlider("Wallrun blend %", 0, 100, 65, v -> nativeSetSSFX(320, v));
        addEnhSlider("Wallrun stick %", 0, 300, 120, v -> nativeSetSSFX(321, v));
        addEnhSlider("Wallrun relax ticks", 0, 30, 5, v -> nativeSetSSFX(323, v));
        addEnhToggle("GRAPPLE (L)", 4, true);
        addEnhToggle("JETPACK (L)", 5, false);
        addEnhSection("GRAPPLE DEBUG");
        addEnhFxToggle("GRAPPLE TARGET VIEW", 310, false);
        addEnhFxToggle("GRAPPLE HUD LINE", 313, false);
        addEnhSlider("fan half-angle x0.1 deg", 0, 890, 80, v -> debugFx(311, v));
        addEnhSlider("fan rays", 2, 16, 8, v -> debugFx(312, v));
        addEnhSlider("grapple range units", 100, 6000, 3400, v -> debugFx(314, v));
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
        addEnhSection("QUICK DEV");
        quickDevToggle = new TextView(this);
        quickDevToggle.setTextSize(16);
        quickDevToggle.setPadding(dp(4), dp(8), dp(4), dp(8));
        quickDevToggle.setOnClickListener(v ->
                debugQuickDev(quickDevOn ? 0 : 1, -1, -1));
        addCompact(enhParent(), quickDevToggle);
        quickDevLevelButton = new TextView(this);
        quickDevLevelButton.setTextSize(16);
        quickDevLevelButton.setTextColor(0xFF66CCFF);
        quickDevLevelButton.setPadding(dp(4), dp(8), dp(4), dp(8));
        quickDevLevelButton.setOnClickListener(v -> {
            quickDevLevel = quickDevLevel >= LEVEL_MAX_PORT ? 1 : quickDevLevel + 1;
            debugQuickDev(-1, quickDevLevel, -1);
        });
        addCompact(enhParent(), quickDevLevelButton);
        quickDevMuteToggle = new TextView(this);
        quickDevMuteToggle.setTextSize(16);
        quickDevMuteToggle.setPadding(dp(4), dp(8), dp(4), dp(8));
        quickDevMuteToggle.setOnClickListener(v ->
                debugQuickDev(-1, -1, quickDevMute ? 0 : 1));
        addCompact(enhParent(), quickDevMuteToggle);
        {
            TextView note = new TextView(this);
            note.setText("boot shortcut: next launch (no logo/title/attract)");
            note.setTextSize(12);
            note.setTextColor(0xFF888888);
            note.setPadding(dp(4), dp(2), dp(4), dp(6));
            enhParent().addView(note);
        }
        refreshQuickDevUi();

        addEnhSection("CAPTIONS");
        captionToggle = new TextView(this);
        captionToggle.setTextSize(16);
        captionToggle.setPadding(dp(4), dp(8), dp(4), dp(8));
        captionToggle.setOnClickListener(v -> debugCaptions(-1));
        addCompact(enhParent(), captionToggle);
        addEnhSlider("Caption size %", 50, 200, captionSizePct,
                this::setCaptionSizePct);
        addEnhSlider("Caption opacity %", 20, 100, captionOpacityPct,
                this::setCaptionOpacityPct);
        {
            TextView note = new TextView(this);
            note.setText("JAVA: crisp text over the game. GAME FONT: native HUD "
                    + "glyphs. The overlay is not in a feedback shot.png.");
            note.setTextSize(12);
            note.setTextColor(0xFF888888);
            note.setPadding(dp(4), dp(2), dp(4), dp(6));
            enhParent().addView(note);
        }
        refreshCaptionsUi();
        enhColumnTarget = enhPageColumns[0][1];
        addEnhSection("RENDER / SIM");
        uncappedTestToggle = new TextView(this);
        uncappedTestToggle.setText(toggleLabel("UNCAPPED TEST SIM", uncappedTestOn));
        uncappedTestToggle.setTextSize(16);
        uncappedTestToggle.setTextColor(uncappedTestOn ? 0xFF66FF99 : 0xFF888888);
        uncappedTestToggle.setPadding(dp(4), dp(8), dp(4), dp(8));
        uncappedTestToggle.setOnClickListener(v -> debugUncapped(uncappedTestOn ? 0 : 1));
        enhParent().addView(uncappedTestToggle);
        TextView interp = new TextView(this);
        interpButton = interp;
        interp.setText(toggleLabel("INTERP", interpOn));
        interp.setTextSize(16);
        interp.setTextColor(interpOn ? 0xFF66FF99 : 0xFF888888);
        interp.setPadding(dp(4), dp(8), dp(4), dp(8));
        interp.setOnClickListener(v -> {
            interpOn = !interpOn;
            nativeSetHfrInterp(interpOn ? 1 : 0);
            interp.setText(toggleLabel("INTERP", interpOn));
            interp.setTextColor(interpOn ? 0xFF66FF99 : 0xFF888888);
            Log.i(TAG, "INTERP " + interpOn);
        });
        enhParent().addView(interp);

        /* Slow motion: sim speed divisor + sub-frame interpolation. The sim
         * advances once every N presented ticks; with interpolation on the
         * frozen ticks re-present the last display list at k/N of the step. */
        TextView simInterp = new TextView(this);
        simInterp.setText(toggleLabel("SIM INTERP", simInterpOn));
        simInterp.setTextSize(16);
        simInterp.setTextColor(simInterpOn ? 0xFF66FF99 : 0xFF888888);
        simInterp.setPadding(dp(4), dp(8), dp(4), dp(8));
        simInterp.setOnClickListener(v -> {
            simInterpOn = !simInterpOn;
            debugFx(242, simInterpOn ? 1 : 0);
            simInterp.setText(toggleLabel("SIM INTERP", simInterpOn));
            simInterp.setTextColor(simInterpOn ? 0xFF66FF99 : 0xFF888888);
            Log.i(TAG, "SIM INTERP " + simInterpOn);
        });
        enhParent().addView(simInterp);
        addEnhSlider("Sim speed 1/n", 1, 8, 1, v -> debugFx(241, v));
        addEnhSlider("Present at t=1 (diag)", 0, 1, 0, v -> debugFx(243, v));

        final TextView stayToggle = new TextView(this);
        stayToggle.setText(toggleLabel("STAY IN LEVEL", stayOn));
        stayToggle.setTextSize(16);
        stayToggle.setTextColor(stayOn ? 0xFF66FF99 : 0xFF888888);
        stayToggle.setPadding(dp(4), dp(8), dp(4), dp(8));
        stayToggle.setOnClickListener(v -> {
            stayOn = !stayOn;
            nativeSetStereoInt(30, stayOn ? 1 : 0);
            stayToggle.setText(toggleLabel("STAY IN LEVEL", stayOn));
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
        addEnhButton("CHAIN LOOP (ALL)", () -> nativePtcCommand("chainloop"));

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
        addEnhKnobToggle("RELAY LOOP", "relay.loop", true);
        addEnhKnobToggle("RELAY FLIP", "relay.flip", false);
        addEnhKnobCycle("RECURSION", "relay.recursion",
                new float[] {1f, 2f, 3f, 4f, 6f, 8f}, true, 3f);
        addEnhButton("TOOL SWAP (GUN/HOOK)", () -> nativePtcCommand("toolswap"));
        addEnhButton("PORTAL LOOP BEAT", () -> nativePtcCommand("chainportal"));

        addEnhSection("MOVE ANNEXES");
        addEnhButton("WALL-RUN ANNEX", () -> nativePtcCommand("wallrun"));
        addEnhButton("JETPACK ANNEX", () -> nativePtcCommand("jetpack"));
        addEnhButton("GRAPPLE TEST", () -> nativePtcCommand("grapple"));
        addEnhButton("CORNER (W)", () -> nativePtcCommand("cornerportal"));
        addEnhButton("CORNER (S)", () -> nativePtcCommand("cornerportal2"));

        enhColumnTarget = enhPageColumns[2][0];
        addEnhSection("BALL / CUBES");
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
        pauseLoopToggle.setText(toggleLabel("PAUSE GAME LOOP", false));
        pauseLoopToggle.setTextSize(16);
        pauseLoopToggle.setTextColor(0xFF888888);
        pauseLoopToggle.setPadding(dp(4), dp(10), dp(4), dp(10));
        pauseLoopToggle.setOnClickListener(v -> {
            hardPaused = !hardPaused;
            nativeSetHardPause(hardPaused ? 1 : 0);
            pauseLoopToggle.setText(toggleLabel("PAUSE GAME LOOP", hardPaused));
            pauseLoopToggle.setTextColor(hardPaused ? 0xFF66FF99 : 0xFF888888);
        });
        enhParent().addView(pauseLoopToggle);
        addEnhSection("DISPLAY");
        addEnhButton("RESET TO LOGO", this::resetToLogo);
        addEnhButton("REINIT LEIA SURFACE", this::debugReinit);
        addEnhButton("STEP ONE TICK", () -> {
            nativeSetHardPause(1);
            hardPaused = true;
            if (pauseLoopToggle != null) {
                pauseLoopToggle.setText(toggleLabel("PAUSE GAME LOOP", true));
                pauseLoopToggle.setTextColor(0xFF66FF99);
            }
            nativeStepTicks(1);
        });
        enhColumnTarget = enhPageColumns[3][2];
        addEnhSection("HUD");
        {
            final TextView gh = new TextView(this);
            gh.setText(toggleLabel("GAME HUD", true));
            gh.setTextSize(16);
            gh.setTextColor(0xFF66FF99);
            gh.setPadding(dp(4), dp(8), dp(4), dp(8));
            gh.setOnClickListener(v -> {
                boolean on = gh.getCurrentTextColor() != 0xFF66FF99;
                gameHudOn = !on;
                nativeSetGameHudHidden(on ? 0 : 1);
                gh.setText(toggleLabel("GAME HUD", on));
                gh.setTextColor(on ? 0xFF66FF99 : 0xFF888888);
                refreshTabStyles();
            });
            enhParent().addView(gh);
        }
        {
            final TextView cap = new TextView(this);
            cap.setText(toggleLabel("DEBUG CAPTIONS", true));
            cap.setTextSize(16);
            cap.setTextColor(0xFF66FF99);
            cap.setPadding(dp(4), dp(8), dp(4), dp(8));
            cap.setOnClickListener(v -> {
                boolean on = cap.getCurrentTextColor() != 0xFF66FF99;
                debugCapsOn = !on;
                debugFx(217, on ? 1 : 0);
                cap.setText(toggleLabel("DEBUG CAPTIONS", on));
                cap.setTextColor(on ? 0xFF66FF99 : 0xFF888888);
                refreshTabStyles();
            });
            enhParent().addView(cap);
        }
        {
            final TextView oc = new TextView(this);
            oc.setText(toggleLabel("TOUCH CONTROLS", true));
            oc.setTextSize(16);
            oc.setTextColor(0xFF66FF99);
            oc.setPadding(dp(4), dp(8), dp(4), dp(8));
            oc.setOnClickListener(v -> {
                boolean on = oc.getCurrentTextColor() != 0xFF66FF99;
                touchCtlOn = !on;
                if (touchOverlay != null) touchOverlay.setControlsHidden(!on);
                oc.setText(toggleLabel("TOUCH CONTROLS", on));
                oc.setTextColor(on ? 0xFF66FF99 : 0xFF888888);
                refreshTabStyles();
            });
            enhParent().addView(oc);
        }
        enhColumnTarget = enhPageColumns[3][2];
        addEnhSection("TOUCH LAYOUT");
        layoutEditButton = new TextView(this);
        layoutEditButton.setTextSize(16);
        layoutEditButton.setPadding(dp(4), dp(8), dp(4), dp(8));
        layoutEditButton.setOnClickListener(v -> {
            if (touchOverlay == null) return;
            touchOverlay.setEditMode(!touchOverlay.isEditMode());
            refreshLayoutUi();
        });
        addCompact(enhParent(), layoutEditButton);

        layoutProfileButton = new TextView(this);
        layoutProfileButton.setTextSize(16);
        layoutProfileButton.setTextColor(0xFF66CCFF);
        layoutProfileButton.setPadding(dp(4), dp(8), dp(4), dp(8));
        layoutProfileButton.setOnClickListener(v -> {
            if (touchOverlay == null) return;
            java.util.List<String> ps = touchOverlay.listProfiles();
            if (ps.isEmpty()) return;
            int idx = ps.indexOf(touchOverlay.activeProfile());
            touchOverlay.switchProfile(ps.get((idx + 1) % ps.size()));
            refreshLayoutUi();
        });
        addCompact(enhParent(), layoutProfileButton);

        layoutLayoutStatus = new TextView(this);
        layoutLayoutStatus.setTextSize(11);
        layoutLayoutStatus.setTextColor(0xFF888888);
        layoutLayoutStatus.setPadding(dp(4), dp(2), dp(4), dp(6));
        enhParent().addView(layoutLayoutStatus);

        addEnhButton("SAVE AS PROFILE...", () -> {
            if (touchOverlay == null) return;
            promptText("Save layout as profile", touchOverlay.activeProfile(), name -> {
                if (!touchOverlay.saveProfileAs(name)) {
                    layoutNote("could not save '" + name + "'");
                }
                refreshLayoutUi();
            });
        });
        addEnhButton("RENAME PROFILE...", () -> {
            if (touchOverlay == null) return;
            if (TouchLayoutStore.DEFAULT_PROFILE.equals(touchOverlay.activeProfile())) {
                layoutNote("the default profile cannot be renamed");
                return;
            }
            promptText("Rename profile", touchOverlay.activeProfile(), name -> {
                if (!touchOverlay.renameProfile(touchOverlay.activeProfile(), name)) {
                    layoutNote("rename failed (name in use?)");
                }
                refreshLayoutUi();
            });
        });
        addEnhButton("DELETE PROFILE", () -> {
            if (touchOverlay == null) return;
            if (TouchLayoutStore.DEFAULT_PROFILE.equals(touchOverlay.activeProfile())) {
                layoutNote("the default profile cannot be deleted");
                return;
            }
            String doomed = touchOverlay.activeProfile();
            touchOverlay.deleteProfile(doomed);
            refreshLayoutUi();
            layoutNote("deleted " + doomed);
        });
        addEnhButton("RESET LAYOUT", () -> {
            if (touchOverlay == null) return;
            touchOverlay.resetLayout();
            refreshLayoutUi();
        });
        addEnhButton("EXPORT / SHARE ACTIVE", () -> {
            if (touchOverlay == null) return;
            touchOverlay.exportProfile();
            refreshLayoutUi();
            layoutNote("exported to " + touchOverlay.externalDir());
        });
        refreshLayoutUi();
        enhColumnTarget = enhPageColumns[3][2];
        addEnhSection("PERFORMANCE");
        {
            final TextView pm = new TextView(this);
            pm.setText(toggleLabel("PERF MONITOR", true));
            pm.setTextSize(16);
            pm.setTextColor(0xFF66FF99);
            pm.setPadding(dp(4), dp(8), dp(4), dp(8));
            pm.setOnClickListener(v -> {
                boolean on = pm.getCurrentTextColor() != 0xFF66FF99;
                perfMonitorOn = on;
                pm.setText(toggleLabel("PERF MONITOR", on));
                pm.setTextColor(on ? 0xFF66FF99 : 0xFF888888);
            });
            addCompact(enhParent(), pm);

            final TextView fg = new TextView(this);
            fg.setText(toggleLabel("FRAME GRAPH", true));
            fg.setTextSize(16);
            fg.setTextColor(0xFF66FF99);
            fg.setPadding(dp(4), dp(8), dp(4), dp(8));
            fg.setOnClickListener(v -> {
                boolean on = fg.getCurrentTextColor() != 0xFF66FF99;
                frameGraphOn = on;
                if (frameGraph != null) {
                    frameGraph.setVisibility(on ? View.VISIBLE : View.GONE);
                    frameGraph.invalidate();
                }
                fg.setText(toggleLabel("FRAME GRAPH", on));
                fg.setTextColor(on ? 0xFF66FF99 : 0xFF888888);
            });
            addCompact(enhParent(), fg);

            final TextView fc = new TextView(this);
            fc.setText(toggleLabel("FPS COUNTER", true));
            fc.setTextSize(16);
            fc.setTextColor(0xFF66FF99);
            fc.setPadding(dp(4), dp(8), dp(4), dp(8));
            fc.setOnClickListener(v -> {
                boolean on = fc.getCurrentTextColor() != 0xFF66FF99;
                fpsCounterOn = on;
                if (fpsView != null) fpsView.setVisibility(on ? View.VISIBLE : View.GONE);
                fc.setText(toggleLabel("FPS COUNTER", on));
                fc.setTextColor(on ? 0xFF66FF99 : 0xFF888888);
            });
            addCompact(enhParent(), fc);
        }
        enhColumnTarget = null;

        // --- Realtime dynamic shadows (quality / performance) ---
        beginTab();
        addPerfRow("SHADOW", () -> nativeSetShadow(0, 0), () -> nativeSetShadow(0, 1), () -> shadowOn);
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
        beginSection("SUN / ADVANCED", false);
        addShadowSlider("Range", 1500, 9000, 4500, " u", 8);
        addShadowSlider("Sun yaw", 0, 359, 45, "deg", 9);
        addShadowSlider("Sun pitch", 5, 89, 52, "deg", 10);
        addShadowSlider("Update every", 1, 4, 1, " fr", 2);
        endSection();
        // Push the current values once so the native defaults match the labels.
        nativeSetShadow(1, shadowRes);

        // --- Splat rendering (first slice, plan-cool-additions §5.6) ---
        beginTab();
        addPerfRow("SPLAT", () -> nativeSetStereoInt(60, 0), () -> nativeSetStereoInt(60, 1),
                () -> splatMode != 0);
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
        addAboveGrid(splatTargetButton, 0);
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
        addAboveGrid(splatShapeButton, 0);
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
                + " Leia CNSDK + firmware versions, the Leia weaver boot stages, the"
                + " current app state (FPS/rate/FX/camera/PTC status) and recent logs.\n\n"
                + "LEIA VERBOSE switches the SDK to Trace logging and restarts so the"
                + " full launch is captured (log file grows up to 16 MB and covers all"
                + " processes, including the head-tracking service).\n\n"
                + "EXPORT opens the system save-file picker, then shows the share sheet"
                + " so the log can be sent straight back.");
        controls.addView(exportHint);
        addEnhButton("EXPORT LOG (SAVE + SHARE)", this::exportLogToPicker);
        addEnhButton("SHARE LOG", this::shareDebugLog);
        addEnhButton("REFRESH INFO", this::refreshDebugLogInfo);
        {
            final TextView vb = new TextView(this);
            vb.setText("\u25C9 LEIA VERBOSE: " + (DebugLog.isVerbose(this) ? "ON" : "OFF")
                    + "  (tap to switch + restart)");
            vb.setTextSize(16);
            vb.setTextColor(0xFF66CCFF);
            vb.setPadding(dp(4), dp(8), dp(4), dp(8));
            vb.setOnClickListener(v -> toggleVerboseAndRestart());
            controls.addView(vb);
        }
        addEnhButton("SHARE LAST REPORT", this::shareLastReport);
        addEnhButton("RESTART APP (CAPTURE LAUNCH)", this::restartApp);
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

        // --- Dynamic light / blur / probes / lens / views (second-tier tabs) ---
        beginTab();
        final String[] lightNames = { "BLUR", "LIGHTS", "PROBES", "LENS", "RATE/DN", "GLOW" };
        LinearLayout lightBar = new FlowBar();
        lightBar.setPadding(dp(4), dp(2), dp(4), dp(2));
        controls.addView(lightBar);
        final LinearLayout[] lightPages = new LinearLayout[lightNames.length];
        final TextView[] lightTabs = new TextView[lightNames.length];
        for (int i = 0; i < lightNames.length; i++) {
            final int page = i;
            TextView tab = new TextView(this);
            tab.setText(lightNames[i]);
            tab.setTextSize(15);
            tab.setPadding(dp(10), dp(6), dp(10), dp(6));
            tab.setTextColor(i == 0 ? 0xFF66FF99 : 0xFF888888);
            tab.setOnClickListener(v -> {
                for (int j = 0; j < lightPages.length; j++) {
                    lightPages[j].setVisibility(j == page ? View.VISIBLE : View.GONE);
                    lightTabs[j].setTextColor(j == page ? 0xFF66FF99 : 0xFF888888);
                }
            });
            lightBar.addView(tab);
            lightTabs[i] = tab;
            registerPanelButton("LIGHT_" + lightNames[i], tab);

            LinearLayout lp = new LinearLayout(this);
            lp.setOrientation(LinearLayout.VERTICAL);
            lp.setVisibility(i == 0 ? View.VISIBLE : View.GONE);
            controls.addView(lp);
            lightPages[i] = lp;
        }

        // BLUR: one toggle row, then the two blur sliders side by side.
        controls = lightPages[0];
        grid = newGrid(controls);
        {
            LinearLayout r = addRow();
            addToggleCell(r, "MOTION BLUR", 100, false);
            addToggleCell(r, "VERTEX VECTORS", 140, true);
            addDebugCell(r, "VECTOR HEATMAP", 9);
        }
        moveGridLast();
        addIntSlider("Blur strength", 0, 300, 40, v -> debugFx(101, v));
        addIntSlider("Sub-steps", 1, 16, 4, v -> debugFx(142, v));
        addIntSlider("Max blur px", 0, 128, 16, v -> debugFx(143, v));
        addIntSlider("Shutter deg", 0, 360, 180, v -> debugFx(141, v));
        addIntSlider("Sub-step jitter %", 0, 100, 100, v -> debugFx(146, v));
        beginSection("VECTORS & DIAGNOSTICS", false);
        addIntSlider("Depth reject %", 0, 100, 20, v -> debugFx(147, v));
        addIntSlider("Velocity clamp px", 4, 300, 60, v -> debugFx(148, v));
        addIntSlider("Velocity res 1/n", 1, 8, 4, v -> debugFx(144, v));
        addIntSlider("Vector update every", 1, 4, 1, v -> debugFx(145, v));
        addIntSlider("Vector heat gain", 20, 500, 120, v -> debugFx(118, v));
        addIntSlider("Vector bound 0/1/2", 0, 2, 1, v -> debugFx(240, v));
        addIntSlider("Heat style 0/1/2", 0, 2, 2, v -> debugFx(149, v));
        endSection();

        // LIGHTS: demo buttons, then count/intensity/shadows, then per-light RGB.
        controls = lightPages[1];
        grid = newGrid(controls);
        /* Enable toggle first, demo actions after it (Task B). */
        {
            LinearLayout r = addRow();
            addToggleCell(r, "LIGHT SHADOWS", 104, false);
        }
        {
            LinearLayout r = addRow();
            addActionCell(r, "LIGHT TEST", () -> debugPtc("lighttest"));
            addActionCell(r, "LIGHT LOOP 10s", () -> {
                debugLevel(38);
                new android.os.Handler(android.os.Looper.getMainLooper()).postDelayed(
                        () -> debugPtc("lightshow"), 2500);
            });
            addActionCell(r, "STOP LOOP", () -> debugPtc("lightshowstop"));
        }
        moveGridLast();
        addIntSlider("Light count", 0, 4, 0, v -> {
            lightCountOn = v > 0;
            refreshTabStyles();
            debugFx(103, v);
        });
        addIntSlider("Light intensity", 0, 300, 120, v -> debugFx(105, v));
        addIntSlider("Shadow steps", 0, 8, 6, v -> debugFx(106, v));
        beginSection("PER-LIGHT COLOR", false);
        addIntSlider("Light 0 R", 0, 255, 255, v -> debugFx(120, v));
        addIntSlider("Light 0 G", 0, 255, 140, v -> debugFx(121, v));
        addIntSlider("Light 0 B", 0, 255, 64, v -> debugFx(122, v));
        addIntSlider("Light 1 R", 0, 255, 64, v -> debugFx(124, v));
        addIntSlider("Light 1 G", 0, 255, 140, v -> debugFx(125, v));
        addIntSlider("Light 1 B", 0, 255, 255, v -> debugFx(126, v));
        endSection();

        // PROBES: grid shape (cols/rows/repeats), then spacing/size/quality.
        controls = lightPages[2];
        grid = newGrid(controls);
        {
            LinearLayout r = addRow();
            addToggleCell(r, "SHOW PROBES", 110, false);
        }
        moveGridLast();
        addIntSlider("Grid cols (X)", 1, 4, 2, v -> debugFx(115, v));
        addIntSlider("Grid rows (Y)", 1, 4, 2, v -> debugFx(116, v));
        addIntSlider("Grid repeats (Z)", 1, 4, 2, v -> debugFx(117, v));
        addIntSlider("Grid spacing", 100, 2000, 450, v -> debugFx(111, v));
        addIntSlider("Probe radius", 5, 120, 18, v -> debugFx(113, v));
        addIntSlider("Probe res", 2, 64, 16, v -> debugFx(114, v));

        // LENS: bokeh kernel + aperture shape.
        controls = lightPages[3];
        grid = newGrid(controls);
        {
            LinearLayout r = addRow();
            addToggleCell(r, "CINEMATIC DOF", 11, false);
            addToggleCell(r, "BOKEH KERNEL", 160, false);
        }
        moveGridLast();
        addIntSlider("Bokeh aspect %", 25, 400, 100, v -> debugFx(161, v));
        addIntSlider("Bokeh blades", 3, 12, 6, v -> debugFx(162, v));
        addIntSlider("Bokeh rotation", 0, 359, 0, v -> debugFx(163, v));
        addIntSlider("Anamorphic %", 50, 400, 100, v -> debugFx(164, v));
        addIntSlider("Bokeh highlight %", 0, 300, 100, v -> debugFx(165, v));
        addIntSlider("Cat eye %", 0, 100, 0, v -> debugFx(166, v));

        // RATE / DENOISE
        controls = lightPages[4];
        grid = newGrid(controls);
        {
            LinearLayout r = addRow();
            addToggleCell(r, "RATE LIMIT", 200, true);
            addToggleCell(r, "ADAPTIVE RATE", 202, false);
            addToggleCell(r, "SHOW CAPTIONS", 217, true);
        }
        {
            LinearLayout r = addRow();
            addToggleCell(r, "ALT DENOISE PATH", 210, false);
            addToggleCell(r, "DENOISER", 211, false);
        }
        moveGridLast();
        addIntSlider("Target FPS", 0, 120, 60, v -> debugFx(201, v));
        beginSection("DENOISER (ADVANCED)", false);
        addIntSlider("Denoise intensity", 0, 100, 60, v -> debugFx(212, v));
        addIntSlider("Denoise passes", 1, 3, 2, v -> debugFx(213, v));
        addIntSlider("Denoise radius", 1, 4, 1, v -> debugFx(214, v));
        addIntSlider("Denoise frames", 1, 8, 1, v -> debugFx(215, v));
        addIntSlider("Denoise bounces", 1, 4, 1, v -> debugFx(216, v));
        // 1 = full-res (legacy), 2-4 = filter at scene_res/n then upsample.
        addIntSlider("Denoise res 1/n", 1, 4, 4, v -> debugFx(220, v));
        endSection();

        // --- Crepuscular rays: quarter-res screen-space light shafts ---
        // Built last so the existing FX page indices (see FX_PAGES) do not
        // shift; it is its own top-level strip tab because the page index is
        // just the tabViews position.
        beginTab();
        addPerfRow("RAYS", () -> nativeSetSSFX(250, 0), () -> nativeSetSSFX(250, 1),
                   () -> raysOn);
        raysToggle = addFxToggle("CREPUSCULAR RAYS", 250, raysOn);
        {
            TextView hint = new TextView(this);
            hint.setTextSize(12);
            hint.setTextColor(0xFFAACCFF);
            hint.setPadding(dp(4), 0, dp(4), dp(6));
            hint.setText("Additive light shafts from a bright source. Source 4 is a"
                    + " synthetic sun above-left; 0-3 follow the dynamic lights"
                    + " (use LIGHT LOOP or LIGHT TEST first). A light behind the"
                    + " camera falls back to the sun.");
            controls.addView(hint);
        }
        addIntSlider("Shaft intensity %", 0, 400, 100, v -> debugFx(251, v));
        addIntSlider("March length %", 10, 200, 100, v -> debugFx(252, v));
        addIntSlider("Falloff per step %", 50, 100, 96, v -> debugFx(253, v));
        addIntSlider("Sample weight %", 0, 300, 60, v -> debugFx(254, v));
        addIntSlider("March samples 8-32", 8, 32, 16, v -> debugFx(255, v));
        addIntSlider("Brightness gate %", 0, 100, 20, v -> debugFx(256, v));
        beginSection("ADVANCED / DEBUG", false);
        addIntSlider("Source light 0-3, 4=sun", 0, 4, 4, v -> debugFx(257, v));
        addIntSlider("Start jitter %", 0, 100, 100, v -> debugFx(258, v));
        addIntSlider("Occlusion bias %", 0, 50, 10, v -> debugFx(260, v));
        addIntSlider("Debug 0=off 1=shafts 2=vis", 0, 2, 0, v -> debugFx(259, v));
        endSection();
        // GLOW (fx 261-273): emissive materials + bloom. The bloom input is the
        // half-res EMISSIVE buffer, so only geometry tagged as emitting glows;
        // a bright white wall does not. Everything is off until BLOOM is on.
        controls = lightPages[5];
        grid = newGrid(controls);
        {
            LinearLayout r = addRow();
            addToggleCell(r, "BLOOM", 261, false);
            addToggleCell(r, "EMISSIVE", 269, true);
            addToggleCell(r, "JETPACK LIGHT", 271, true);
        }
        moveGridLast();
        addIntSlider("Bloom threshold", 0, 255, 48, v -> debugFx(262, v));
        addIntSlider("Bloom intensity %", 0, 400, 120, v -> debugFx(263, v));
        addIntSlider("Bloom radius", 1, 8, 3, v -> debugFx(264, v));
        addIntSlider("Bloom passes", 1, 3, 2, v -> debugFx(265, v));
        addIntSlider("Bloom taps", 5, 9, 9, v -> debugFx(266, v));
        addIntSlider("Emissive gain %", 0, 400, 100, v -> debugFx(267, v));
        addIntSlider("Jetpack light %", 0, 400, 150, v -> debugFx(272, v));
        addIntSlider("Jetpack light radius", 60, 600, 220, v -> debugFx(273, v));
        // Debug views prove the glow source: 1 = raw emissive buffer (world
        // space, no lighting), 2 = the blurred bloom buffer, 0 = normal frame.
        {
            LinearLayout r = addRow();
            addActionCell(r, "VIEW EMISSIVE", () -> debugFx(268, 1));
            addActionCell(r, "VIEW BLOOM", () -> debugFx(268, 2));
            addActionCell(r, "VIEW OFF", () -> debugFx(268, 0));
        }

        // --- Tone mapping + colour grading (fx 280-289) ---
        // Appended last so the existing FX page indices do not shift. Everything
        // is off by default; the whole chain costs one uniform branch until the
        // master toggle is on.
        beginTab();
        addPerfRow("GRADE", () -> nativeSetSSFX(280, 0), () -> nativeSetSSFX(280, 1),
                   () -> nativeGetSSFX(280) != 0);
        addFxToggle("TONE MAP + GRADE", 280, false);
        {
            TextView hint = new TextView(this);
            hint.setTextSize(12);
            hint.setTextColor(0xFFAACCFF);
            hint.setPadding(dp(4), 0, dp(4), dp(6));
            hint.setText("Display transform applied once at the end of the FX pass"
                    + " (after AO/DOF/shadow/lights/rays, debug views stay raw)."
                    + " Look 6 reads a 2D stripe LUT from"
                    + " files/luts/grade.png: 16x16 blue slices in a 16*N x 16"
                    + " strip (256x16 = 16 slices); no file falls back to NONE.");
            controls.addView(hint);
        }
        addFxCycle("TONEMAP", 281, new String[] {
                "LINEAR", "REINHARD", "ACES", "FILMIC" }, nativeGetSSFX(281));
        addFxCycle("LOOK", 284, new String[] {
                "NONE", "SEPIA", "COOL", "WARM", "BLEACH", "CEL", "LUT" },
                nativeGetSSFX(284));
        addIntSlider("Exposure EV x100", -300, 300, 0, v -> debugFx(282, v));
        addIntSlider("White point %", 100, 400, 100, v -> debugFx(283, v));
        addIntSlider("Look strength %", 0, 100, 100, v -> debugFx(285, v));
        addIntSlider("Saturation %", 0, 200, 100, v -> debugFx(286, v));
        addIntSlider("Contrast %", -100, 100, 0, v -> debugFx(287, v));
        addIntSlider("Tint -cool/+warm", -100, 100, 0, v -> debugFx(288, v));
        addIntSlider("Split pre/post 0/1", 0, 1, 0, v -> debugFx(289, v));
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

    /** Compact runtime state written into the export header. */
    private String runtimeStateReport() {
        StringBuilder b = new StringBuilder();
        b.append("fpsMode: ").append(fpsMode).append(" (").append(fpsLabel()).append(")\n");
        try {
            b.append("rate: enabled=").append(nativeGetRateState(0))
             .append(" target=").append(nativeGetRateState(1))
             .append(" adaptive=").append(nativeGetRateState(2))
             .append(" effective=").append(nativeGetRateState(5))
             .append(" lastFrameUs=").append(nativeGetRateState(6))
             .append(" emaUs=").append(nativeGetRateState(3)).append('\n');
        } catch (Throwable t) {
            b.append("rate: unavailable (").append(t).append(")\n");
        }
        b.append("fx: ao=").append(aoOn).append(" dof=").append(dofOn)
         .append(" gi=").append(giOn).append(" ssrtx=").append(ssrtxOn)
         .append(" shadow=").append(shadowOn).append(" splat=").append(splatMode)
         .append(" mb=").append(mbOn).append(" lights=").append(lightCountOn)
         .append(" probes=").append(probeOn).append(" debugView=").append(fxDebug).append('\n');
        b.append("cameraMode: ").append(camModeLabel())
         .append(" hdt=").append(hdtOn).append(" gyro=").append(gyroOn)
         .append(" stereo3d=").append(stereoOn).append('\n');
        b.append("camstick: ").append(StickCurve.describe()).append('\n');
        b.append("gameHud=").append(gameHudOn).append(" captions=").append(debugCapsOn)
         .append(" capfont=").append(captionJavaOn ? "java" : "game")
         .append(" capsize=").append(captionSizePct).append(" capop=").append(captionOpacityPct)
         .append(" touch=").append(touchCtlOn).append('\n');
        if (!stereoValues.isEmpty()) {
            b.append("stereo:");
            for (java.util.Map.Entry<Integer, Integer> e : stereoValues.entrySet()) {
                b.append(' ').append(e.getKey()).append('=').append(e.getValue());
            }
            b.append('\n');
        }
        try {
            String st = nativePtcStatus();
            if (st != null) {
                st = st.replace('\n', ' ').trim();
                if (st.length() > 1400) st = st.substring(0, 1400) + "...";
                b.append("status: ").append(st).append('\n');
            }
        } catch (Throwable t) {
            b.append("status: unavailable (").append(t).append(")\n");
        }
        return b.toString();
    }

    private void toggleVerboseAndRestart() {
        boolean now = !DebugLog.isVerbose(this);
        // Synchronous commit: the process is killed right after this, and an
        // async apply() can be lost before the new process reads the pref.
        getSharedPreferences(PREFS_NAME, MODE_PRIVATE).edit()
                .putBoolean(DebugLog.PREF_VERBOSE, now).commit();
        android.widget.Toast.makeText(this,
                now ? "Verbose Leia logging ON - restarting" : "Verbose logging OFF - restarting",
                android.widget.Toast.LENGTH_SHORT).show();
        restartApp();
    }

    /** Restart the process so boot logs (Leia init) are captured from frame 0. */
    private void restartApp() {
        try {
            Intent i = new Intent(this, sm64portActivity.class);
            i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TASK);
            startActivity(i);
        } catch (Throwable t) {
            Log.e(TAG, "restart failed", t);
        }
        finish();
        android.os.Process.killProcess(android.os.Process.myPid());
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
        sb.append("\nLAST EXIT\n").append(DebugLog.lastExitSummary()).append('\n');
        sb.append("\nThis report includes:\n");
        sb.append("- app/build/Android/device details\n");
        sb.append("- Lume build + Leia service versions\n");
        sb.append("- Leia CNSDK version\n");
        sb.append("- display modes and refresh rates\n");
        sb.append("- Leia weaver/CNSDK boot stage markers\n");
        sb.append("- last session exit status (clean / unclean)\n");
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

    /** Share the newest already-built report, or build one if none exists. */
    private void shareLastReport() {
        new Thread(() -> {
            try {
                File report = DebugLog.lastReport(this);
                if (report == null || !report.isFile()) {
                    report = DebugLog.buildExportFile(this);
                }
                final File f = report;
                runOnUiThread(() -> {
                    try {
                        startActivity(DebugLog.buildShareIntent(this, f));
                    } catch (Throwable t) {
                        Log.e(TAG, "share last report failed", t);
                        android.widget.Toast.makeText(this,
                                "Share failed: " + t, android.widget.Toast.LENGTH_LONG).show();
                    }
                });
            } catch (Throwable t) {
                Log.e(TAG, "build last report failed", t);
            }
        }, "sm64-share-last-report").start();
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
    /* BETTER and PUPPY are temporarily disabled (hidden, not removed). */
    private static final boolean[] CAM_MODE_ENABLED = {
            true, false, false, true, true, true
    };

    private static int nextCamMode(int from, int dir) {
        int n = CAM_MODE_SHORT.length;
        for (int i = 1; i <= n; i++) {
            int m = ((from + dir * i) % n + n) % n;
            if (CAM_MODE_ENABLED[m]) return m;
        }
        return from;
    }

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
            if (!CAM_MODE_ENABLED[i]) {
                pill.setVisibility(View.GONE);
                lp.weight = 0f;
            }
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
        Log.i(TAG, "camera mode -> " + camModeName(mode) + " (" + mode + ")");
    }

    int getCamMode2() {
        return camMode2;
    }

    /** Explicit 3D/2D switch for scripts (0 = 2D, 1 = 3D). */
    void debugSetStereo(int on) {
        runOnUiThread(() -> setStereoMode(on != 0));
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
        setCamMode2(nextCamMode(camMode2, 1));
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
            /* FREE had no case, so the header claimed "CAM: VANILLA" while the
             * manual free camera was driving the view - which read as "the
             * cycler wrapped back to vanilla and the screen went black". */
            case 5: return "FREE";
            default: return "VANILLA";
        }
    }

    private String camModeLabel() {
        return "CAM MODE: " + camModeName(camMode2);
    }

    /**
     * LOW / MED / HIGH sensitivity band, pinned to the top of CAMERA > SETTINGS.
     *
     * The band is a multiplier and the "Cam sensitivity" slider trims inside
     * it, so the effective value is the product.  Both are persisted, and the
     * effective percent is spelled out because a band label alone does not say
     * what the camera will actually do.
     */
    private void addCamSensPicker(LinearLayout parent) {
        TextView label = new TextView(this);
        label.setText("SENSITIVITY");
        label.setTextSize(12);
        label.setTextColor(0xFF888888);
        label.setPadding(dp(4), dp(10), dp(4), dp(4));

        LinearLayout track = new LinearLayout(this);
        track.setOrientation(LinearLayout.HORIZONTAL);
        GradientDrawable bg = new GradientDrawable();
        bg.setColor(0xFF171B22);
        bg.setCornerRadius(dp(18));
        bg.setStroke(dp(1), 0xFF39404E);
        track.setBackground(bg);
        track.setPadding(dp(3), dp(3), dp(3), dp(3));

        camSensPills = new TextView[CAM_SENS_NAMES.length];
        for (int i = 0; i < CAM_SENS_NAMES.length; i++) {
            TextView pill = new TextView(this);
            pill.setText(CAM_SENS_NAMES[i] + " " + String.format("%.1fx", CAM_SENS_MULT[i]));
            pill.setTextSize(11);
            pill.setGravity(Gravity.CENTER);
            pill.setPadding(dp(4), dp(8), dp(4), dp(8));
            final int band = i;
            pill.setOnClickListener(v -> setCamSensBand(band));
            track.addView(pill, new LinearLayout.LayoutParams(
                    0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
            camSensPills[i] = pill;
        }

        camSensValue = new TextView(this);
        camSensValue.setTextSize(14);
        camSensValue.setTextColor(0xFFAACCFF);
        camSensValue.setPadding(dp(4), dp(6), dp(4), dp(4));

        parent.addView(label, 0);
        parent.addView(track, 1);
        parent.addView(camSensValue, 2);

        nativeSetStereoInt(330, camSensBand);
        stereoValues.put(330, camSensBand);
        updateCamSensPills();
        refreshCamSensLabel();
    }

    private int effectiveCamSens(int trim) {
        return Math.round(trim * CAM_SENS_MULT[Math.max(0, Math.min(2, camSensBand))]);
    }

    private void setCamSensBand(int band) {
        camSensBand = Math.max(0, Math.min(2, band));
        nativeSetStereoInt(330, camSensBand);
        stereoValues.put(330, camSensBand);
        updateCamSensPills();
        refreshCamSensLabel();
        saveCamSensPrefs();
        Log.i(TAG, "CAM_SENS band -> " + CAM_SENS_NAMES[camSensBand]
                + " effective=" + effectiveCamSens(camSensTrim) + "%");
    }

    private void updateCamSensPills() {
        for (int i = 0; i < camSensPills.length; i++) {
            TextView pill = camSensPills[i];
            if (pill == null) continue;
            GradientDrawable bg = new GradientDrawable();
            bg.setCornerRadius(dp(15));
            if (i == camSensBand) {
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

    private void refreshCamSensLabel() {
        if (camSensValue == null) return;
        camSensValue.setText("EFFECTIVE SENSITIVITY: " + effectiveCamSens(camSensTrim)
                + " %  (" + CAM_SENS_NAMES[camSensBand] + " "
                + String.format("%.1fx", CAM_SENS_MULT[camSensBand])
                + " x " + camSensTrim + " %)");
    }

    private void saveCamSensPrefs() {
        try {
            getSharedPreferences(PREFS_NAME, MODE_PRIVATE).edit()
                    .putInt(PREF_CAM_SENS_BAND, camSensBand)
                    .putInt(PREF_CAM_SENS_TRIM, camSensTrim)
                    .apply();
        } catch (Throwable t) {
            Log.w(TAG, "CAM_SENS prefs not saved", t);
        }
    }

    /**
     * COLLIDE / CLIP camera-vs-ground row, directly under WALL NOCLIP on
     * CAMERA > SETTINGS.
     *
     * COLLIDE (default) is the historical camera: floor and ceiling surfaces
     * push it up/in. CLIP ignores those surfaces so the camera can go under
     * the floor or above the ceiling; walls still collide. This is independent
     * of WALL NOCLIP, which hides the wall in front of Mario instead.
     * Native key: fx 360 (customcam.c), persisted here.
     */
    private void addCamGroundClipRow() {
        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.setPadding(dp(2), dp(1), dp(8), dp(1));

        TextView label = new TextView(this);
        label.setText(glyph("CAM GROUND") + " GROUND");
        label.setTextSize(13);
        label.setTextColor(0xFF888888);
        label.setPadding(dp(2), 0, dp(6), 0);
        row.addView(label, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT));

        LinearLayout track = new LinearLayout(this);
        track.setOrientation(LinearLayout.HORIZONTAL);
        GradientDrawable bg = new GradientDrawable();
        bg.setColor(0xFF171B22);
        bg.setCornerRadius(dp(14));
        bg.setStroke(dp(1), 0xFF39404E);
        track.setBackground(bg);
        track.setPadding(dp(2), dp(2), dp(2), dp(2));

        camGroundPills = new TextView[CAM_GROUND_NAMES.length];
        for (int i = 0; i < CAM_GROUND_NAMES.length; i++) {
            TextView pill = new TextView(this);
            pill.setText(CAM_GROUND_NAMES[i]);
            pill.setTextSize(11);
            pill.setGravity(Gravity.CENTER);
            pill.setPadding(dp(6), dp(5), dp(6), dp(5));
            final int mode = i;
            pill.setOnClickListener(v -> setCamGroundClip(mode, true));
            track.addView(pill, new LinearLayout.LayoutParams(
                    0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
            camGroundPills[i] = pill;
        }
        row.addView(track, new LinearLayout.LayoutParams(
                0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));

        addAboveGrid(row, 0);
        setCamGroundClip(camGroundClip, false);
    }

    private void setCamGroundClip(int mode, boolean persist) {
        camGroundClip = mode == 1 ? 1 : 0;
        nativeSetSSFX(360, camGroundClip);
        updateCamGroundPills();
        if (persist) {
            try {
                getSharedPreferences(PREFS_NAME, MODE_PRIVATE).edit()
                        .putInt(PREF_CAM_GROUND_CLIP, camGroundClip)
                        .apply();
            } catch (Throwable t) {
                Log.w(TAG, "CAM_GROUND pref not saved", t);
            }
        }
        Log.i(TAG, "CAM_GROUND " + CAM_GROUND_NAMES[camGroundClip]);
    }

    private void updateCamGroundPills() {
        if (camGroundPills == null) {
            return;
        }
        for (int i = 0; i < camGroundPills.length; i++) {
            TextView pill = camGroundPills[i];
            if (pill == null) continue;
            GradientDrawable bg = new GradientDrawable();
            bg.setCornerRadius(dp(12));
            if (i == camGroundClip) {
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

    private void addCamSlider(String name, int min, int max, int initial, String unit,
                              final int key) {
        final TextView label = new TextView(this);
        label.setTextColor(Color.WHITE);
        label.setTextSize(14);
        label.setText(glyphLabel(name, ": " + initial + unit));
        SeekBar bar = new CompactSeekBar();
        bar.setMax(max - min);
        bar.setProgress(initial - min);
        bar.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override public void onProgressChanged(SeekBar sb, int progress, boolean fromUser) {
                int v = progress + min;
                label.setText(glyphLabel(name, ": " + v + unit));
                stereoValues.put(key, v);
                if (stereoPictogram != null) stereoPictogram.invalidate();
                nativeSetStereoInt(key, v);
                if (key == 43) hdtStrength = v;
                if (key == 44) gyroStrength = v;
                if (key == 45) gyroPivot = v;
                if (key == 49) {
                    camSensTrim = v;
                    refreshCamSensLabel();
                    saveCamSensPrefs();
                }
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
        t.setText("\u25CE CAMERA GESTURES (PINCH/SWIPE): "
                + (TouchOverlay.camGesturesEnabled ? "ON" : "OFF"));
        t.setTextSize(14);
        t.setGravity(Gravity.CENTER);
        t.setPadding(dp(4), dp(8), dp(4), dp(8));
        t.setTextColor(TouchOverlay.camGesturesEnabled ? 0xFF66FF99 : 0xFF888888);
        t.setOnClickListener(v -> {
            TouchOverlay.camGesturesEnabled = !TouchOverlay.camGesturesEnabled;
            t.setText("\u25CE CAMERA GESTURES (PINCH/SWIPE): "
                    + (TouchOverlay.camGesturesEnabled ? "ON" : "OFF"));
            t.setTextColor(TouchOverlay.camGesturesEnabled ? 0xFF66FF99 : 0xFF888888);
            Log.i(TAG, "PINCH_CAM=" + TouchOverlay.camGesturesEnabled);
        });
        return t;
    }

    private TextView makeCamToggle(String name, final int key, boolean initial) {
        final TextView b = new TextView(this);
        b.setText(glyph(name) + " " + name + ": " + (initial ? "ON" : "OFF"));
        b.setTextSize(14);
        b.setGravity(Gravity.CENTER);
        b.setTextColor(initial ? 0xFF66FF99 : 0xFF888888);
        b.setPadding(dp(2), dp(3), dp(2), dp(3));
        b.setOnClickListener(v -> {
            boolean on = b.getCurrentTextColor() != 0xFF66FF99;
            b.setText(glyph(name) + " " + name + ": " + (on ? "ON" : "OFF"));
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
        b.setGravity(Gravity.LEFT | Gravity.CENTER_VERTICAL);
        addAboveGrid(b, 0);
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

    private void rebuildLayerList() {
        if (layerList == null) return;
        layerList.removeAllViews();
        for (int i = 0; i < layerOrder.size(); i++) {
            final String name = layerOrder.get(i);
            LinearLayout row = new LinearLayout(this);
            row.setOrientation(LinearLayout.HORIZONTAL);
            row.setGravity(android.view.Gravity.CENTER_VERTICAL);
            row.setPadding(0, dp(2), 0, dp(2));

            TextView handle = new TextView(this);
            handle.setText("\u283F");
            handle.setTextSize(16);
            handle.setTextColor(0xFF777777);
            handle.setPadding(dp(6), dp(4), dp(8), dp(4));
            row.addView(handle);

            TextView label = new TextView(this);
            label.setText(name);
            label.setTextSize(15);
            label.setTextColor(0xFFFFFFFF);
            row.addView(label, new LinearLayout.LayoutParams(0,
                    ViewGroup.LayoutParams.WRAP_CONTENT, 1f));

            TextView up = new TextView(this);
            up.setText("\u25B2");
            up.setTextSize(13);
            up.setTextColor(i == 0 ? 0xFF444444 : 0xFFCCCCCC);
            up.setPadding(dp(10), dp(4), dp(10), dp(4));
            up.setOnClickListener(v -> {
                int idx = layerOrder.indexOf(name);
                if (idx > 0) {
                    java.util.Collections.swap(layerOrder, idx, idx - 1);
                    rebuildLayerList();
                }
            });
            row.addView(up);

            TextView down = new TextView(this);
            down.setText("\u25BC");
            down.setTextSize(13);
            down.setTextColor(i == layerOrder.size() - 1 ? 0xFF444444 : 0xFFCCCCCC);
            down.setPadding(dp(10), dp(4), dp(10), dp(4));
            down.setOnClickListener(v -> {
                int idx = layerOrder.indexOf(name);
                if (idx >= 0 && idx < layerOrder.size() - 1) {
                    java.util.Collections.swap(layerOrder, idx, idx + 1);
                    rebuildLayerList();
                }
            });
            row.addView(down);

            final boolean isZbuf = name.equals("ZBUF");
            final TextView sw = new TextView(this);
            boolean on = isZbuf ? zbufOn : wireOn;
            sw.setText(on ? "ON" : "OFF");
            sw.setTextSize(14);
            sw.setTextColor(on ? 0xFF66FF99 : 0xFF888888);
            sw.setPadding(dp(12), dp(6), dp(12), dp(6));
            sw.setBackgroundColor(0x22FFFFFF);
            sw.setOnClickListener(v -> {
                boolean now = !(isZbuf ? zbufOn : wireOn);
                if (isZbuf) zbufOn = now; else wireOn = now;
                nativeSetStereoInt(isZbuf ? 15 : 16, now ? 1 : 0);
                sw.setText(now ? "ON" : "OFF");
                sw.setTextColor(now ? 0xFF66FF99 : 0xFF888888);
                refreshTabStyles();
            });
            row.addView(sw);

            layerList.addView(row, new LinearLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        }
    }

    private void showEnhPage(int page) {
        if (enhPages == null) {
            return;
        }
        enhSelected = page;
        for (int i = 0; i < enhPages.length; i++) {
            enhPages[i].setVisibility(i == page ? View.VISIBLE : View.GONE);
        }
        refreshTabStyles();
    }

    private void addCamButton(String name, final Runnable action) {
        controls.addView(makeCamButton(name, action));
    }

    // ---- gameplay enhancement widgets (nativeSetEnh channel) ----

    private TextView addEnhToggle(String name, final int key, boolean initial) {
        final TextView b = new TextView(this);
        b.setText(glyph(name) + " " + name + ": " + (initial ? "ON" : "OFF"));
        b.setTextSize(15);
        b.setTextColor(initial ? 0xFF66FF99 : 0xFF888888);
        b.setPadding(dp(4), dp(4), dp(4), dp(4));
        b.setOnClickListener(v -> {
            boolean on = b.getCurrentTextColor() != 0xFF66FF99;
            b.setText(glyph(name) + " " + name + ": " + (on ? "ON" : "OFF"));
            b.setTextColor(on ? 0xFF66FF99 : 0xFF888888);
            nativeSetEnh(key, on ? 1 : 0);
            if (key == 0) ragdollOn = on;
            if (key == 1) hitRagdollOn = on;
            if (key == 2) ppcamOn = on;
            Log.i(TAG, name + "=" + on);
        });
        addCompact(enhParent(), b);
        nativeSetEnh(key, initial ? 1 : 0);
        return b;
    }

    /** Toggle on the ENH page that drives an `fx` key via nativeSetSSFX
     *  (used by the grapple targeting debug view, fx 310/313). */
    private TextView addEnhFxToggle(String name, final int key, boolean initial) {
        final TextView b = new TextView(this);
        boolean on = nativeGetSSFX(key) != 0;
        b.setText(name + ": " + (on ? "ON" : "OFF"));
        b.setTextSize(16);
        b.setTextColor(on ? 0xFF66FF99 : 0xFF888888);
        b.setPadding(dp(4), dp(8), dp(4), dp(8));
        b.setOnClickListener(v -> {
            boolean now = b.getCurrentTextColor() != 0xFF66FF99;
            b.setText(name + ": " + (now ? "ON" : "OFF"));
            b.setTextColor(now ? 0xFF66FF99 : 0xFF888888);
            debugFx(key, now ? 1 : 0);
        });
        addCompact(enhParent(), b);
        if (on != initial) {
            debugFx(key, initial ? 1 : 0);
        }
        return b;
    }

    private TextView addStereoToggle(String name, final int key, boolean initial) {
        final TextView b = new TextView(this);
        b.setText(glyph(name) + " " + name + ": " + (initial ? "ON" : "OFF"));
        b.setTextSize(15);
        b.setTextColor(initial ? 0xFF66FF99 : 0xFF888888);
        b.setPadding(dp(4), dp(4), dp(4), dp(4));
        b.setOnClickListener(v -> {
            boolean on = b.getCurrentTextColor() != 0xFF66FF99;
            b.setText(glyph(name) + " " + name + ": " + (on ? "ON" : "OFF"));
            b.setTextColor(on ? 0xFF66FF99 : 0xFF888888);
            nativeSetStereoInt(key, on ? 1 : 0);
            Log.i(TAG, name + "=" + on);
        });
        addCompact(controls, b);
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
        b.setText("\u25B8 " + name);
        b.setTextSize(15);
        b.setTextColor(0xFFFF9966);
        b.setPadding(dp(4), dp(6), dp(4), dp(6));
        b.setOnClickListener(v -> action.run());
        addPressFeedback(b);
        addCompact(enhParent(), b);
        return b;
    }

    /** Compact label + bar row for the ENH/DEBUG page (the FX grid helpers are
     * bound to the FX tab's grid, so they cannot be reused there). */
    private void addEnhSlider(String name, int min, int max, int initial,
                              final java.util.function.IntConsumer sink) {
        final TextView label = new TextView(this);
        label.setTextColor(Color.WHITE);
        label.setTextSize(13);
        label.setText(glyphLabel(name, ": " + initial));
        SeekBar bar = new CompactSeekBar();
        bar.setMax(max - min);
        bar.setProgress(initial - min);
        bar.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override public void onProgressChanged(SeekBar sb, int progress, boolean fromUser) {
                int value = progress + min;
                label.setText(glyphLabel(name, ": " + value));
                sink.accept(value);
            }
            @Override public void onStartTrackingTouch(SeekBar sb) {}
            @Override public void onStopTrackingTouch(SeekBar sb) {}
        });
        /* One line per slider: label, bar, one-step buttons. */
        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.addView(label, new LinearLayout.LayoutParams(0,
                ViewGroup.LayoutParams.WRAP_CONTENT, 0.42f));
        row.addView(stepButton("-", bar, -1), new LinearLayout.LayoutParams(dp(20),
                ViewGroup.LayoutParams.MATCH_PARENT));
        row.addView(bar, new LinearLayout.LayoutParams(0,
                ViewGroup.LayoutParams.WRAP_CONTENT, 0.58f));
        row.addView(stepButton("+", bar, 1), new LinearLayout.LayoutParams(dp(20),
                ViewGroup.LayoutParams.MATCH_PARENT));
        LinearLayout p = enhParent();
        p.addView(row, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        sink.accept(initial);
    }

    /** Section header for the ENH tab, so long lists stay readable. */
    private TextView addEnhSection(String title) {
        TextView h = new TextView(this);
        h.setText(title);
        h.setTextSize(13);
        h.setTextColor(0xFF66CCFF);
        h.setPadding(dp(6), dp(7), dp(6), dp(3));
        h.setBackgroundColor(0x33FFFFFF);
        enhParent().addView(h);
        return h;
    }

    /** ENH toggle that drives a runtime PTC knob (`ptc knob <name> 1|0`). */
    private TextView addEnhKnobToggle(String name, final String knob, boolean initial) {
        final TextView b = new TextView(this);
        b.setText(glyph(name) + " " + name + ": " + (initial ? "ON" : "OFF"));
        b.setTextSize(15);
        b.setTextColor(initial ? 0xFF66FF99 : 0xFF888888);
        b.setPadding(dp(4), dp(4), dp(4), dp(4));
        b.setOnClickListener(v -> {
            boolean on = b.getCurrentTextColor() != 0xFF66FF99;
            b.setText(glyph(name) + " " + name + ": " + (on ? "ON" : "OFF"));
            b.setTextColor(on ? 0xFF66FF99 : 0xFF888888);
            nativePtcCommand("knob " + knob + " " + (on ? 1 : 0));
            Log.i(TAG, "KNOB " + knob + "=" + (on ? 1 : 0));
        });
        addPressFeedback(b);
        addCompact(enhParent(), b);
        return b;
    }

    /** ENH control that cycles a runtime PTC knob through fixed values. */
    private TextView addEnhKnobCycle(String name, final String knob, final float[] values,
                                     final boolean integerFormat) {
        return addEnhKnobCycle(name, knob, values, integerFormat, values[0]);
    }

    /** As above, but the label starts at the knob's current value. */
    private TextView addEnhKnobCycle(String name, final String knob, final float[] values,
                                     final boolean integerFormat, float initial) {
        final TextView b = new TextView(this);
        final int[] idx = {0};
        for (int i = 0; i < values.length; i++) {
            if (Math.abs(values[i] - initial) < 0.01f) { idx[0] = i; break; }
        }
        Runnable render = () -> b.setText(glyph(name) + " " + name + ": "
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
        addCompact(enhParent(), b);
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
        /* QUICK DEV remembers the last level used, so the next launch boots it. */
        if (quickDevOn && level >= 1) {
            quickDevLevel = Math.max(1, Math.min(LEVEL_MAX_PORT, level));
            try {
                getSharedPreferences(PREFS_NAME, MODE_PRIVATE).edit()
                        .putInt(PREF_QUICK_DEV_LEVEL, quickDevLevel).commit();
            } catch (Throwable ignored) {
            }
            nativeSetQuickDev(1, quickDevLevel);
            runOnUiThread(this::refreshQuickDevUi);
        }
        Log.i(TAG, "LEVEL " + level + " requested");
    }

    /**
     * QUICK DEV: persist the cheap-boot toggle and push it to the engine.
     * Negative arguments leave that field alone. The boot level is read by
     * thread5_game_loop on the next launch; mute applies immediately.
     */
    void debugQuickDev(int on, int level, int mute) {
        if (on >= 0) quickDevOn = on != 0;
        if (level >= 1) quickDevLevel = Math.max(1, Math.min(LEVEL_MAX_PORT, level));
        if (mute >= 0) quickDevMute = mute != 0;
        try {
            // commit(): the shell path restarts immediately after the broadcast.
            getSharedPreferences(PREFS_NAME, MODE_PRIVATE).edit()
                    .putBoolean(PREF_QUICK_DEV, quickDevOn)
                    .putInt(PREF_QUICK_DEV_LEVEL, quickDevLevel)
                    .putBoolean(PREF_QUICK_DEV_MUTE, quickDevMute).commit();
        } catch (Throwable t) {
            Log.w(TAG, "QUICK_DEV prefs not saved", t);
        }
        nativeSetQuickDev(quickDevOn ? 1 : 0, quickDevLevel);
        if (mute >= 0) nativeSetQuickDevMute(quickDevMute ? 1 : 0);
        Log.i(TAG, "QUICK_DEV on=" + quickDevOn + " level=" + quickDevLevel
                + " mute=" + quickDevMute + " (boot level applies next launch)");
        runOnUiThread(this::refreshQuickDevUi);
    }

    /**
     * Captions: toggle the text owner (negative = flip) and persist the
     * choice. JAVA draws on the overlay and suppresses the native glyphs,
     * GAME FONT restores the original HUD text. The strings and their
     * lifetime are the same either way (see print.c).
     */
    void debugCaptions(int on) {
        captionJavaOn = on < 0 ? !captionJavaOn : on != 0;
        nativeSetCaptionMode(captionJavaOn ? 1 : 0);
        try {
            getSharedPreferences(PREFS_NAME, MODE_PRIVATE).edit()
                    .putBoolean(PREF_CAPTION_JAVA, captionJavaOn).apply();
        } catch (Throwable t) {
            Log.w(TAG, "CAPTIONS pref not saved", t);
        }
        final boolean java = captionJavaOn;
        runOnUiThread(() -> {
            if (captionOverlay != null) {
                captionOverlay.setJavaMode(java);
            }
            refreshCaptionsUi();
        });
        Log.i(TAG, "CAPTIONS " + (captionJavaOn ? "JAVA" : "GAME FONT"));
    }

    private void setCaptionSizePct(int pct) {
        captionSizePct = Math.max(50, Math.min(200, pct));
        if (captionOverlay != null) {
            captionOverlay.setSizePct(captionSizePct);
        }
        try {
            getSharedPreferences(PREFS_NAME, MODE_PRIVATE).edit()
                    .putInt(PREF_CAPTION_SIZE, captionSizePct).apply();
        } catch (Throwable ignored) {
        }
    }

    private void setCaptionOpacityPct(int pct) {
        captionOpacityPct = Math.max(20, Math.min(100, pct));
        if (captionOverlay != null) {
            captionOverlay.setOpacityPct(captionOpacityPct);
        }
        try {
            getSharedPreferences(PREFS_NAME, MODE_PRIVATE).edit()
                    .putInt(PREF_CAPTION_OPACITY, captionOpacityPct).apply();
        } catch (Throwable ignored) {
        }
    }

    private void refreshCaptionsUi() {
        if (captionToggle != null) {
            captionToggle.setText("\u25C9 CAPTIONS: "
                    + (captionJavaOn ? "JAVA" : "GAME FONT"));
            captionToggle.setTextColor(captionJavaOn ? 0xFF66FF99 : 0xFFCCCCCC);
        }
    }

    private void refreshQuickDevUi() {
        if (quickDevToggle != null) {
            quickDevToggle.setText(toggleLabel("QUICK DEV", quickDevOn));
            quickDevToggle.setTextColor(quickDevOn ? 0xFF66FF99 : 0xFF888888);
        }
        if (quickDevLevelButton != null) {
            quickDevLevelButton.setText("\u2691 QD LEVEL: L" + quickDevLevel + " (tap to change)");
        }
        if (quickDevMuteToggle != null) {
            quickDevMuteToggle.setText(toggleLabel("QD MUTE AUDIO", quickDevMute));
            quickDevMuteToggle.setTextColor(quickDevMute ? 0xFF66FF99 : 0xFF888888);
        }
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
            uncappedTestToggle.setText(toggleLabel("UNCAPPED TEST SIM", uncappedTestOn));
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
            if (feedbackBar != null) {
                feedbackBar.setVisibility(forceHudHidden ? View.GONE : View.VISIBLE);
            }
            /* The native captions stop publishing when the HUD is skipped;
             * hide the overlay in the same step so it cannot linger. */
            if (captionOverlay != null) {
                captionOverlay.setVisibility(forceHudHidden ? View.GONE : View.VISIBLE);
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
            pauseLoopToggle.setText(toggleLabel("PAUSE GAME LOOP", hardPaused));
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
        label.setText(glyphLabel(name, ": " + initial + unit));
        SeekBar bar = new CompactSeekBar();
        bar.setMax(max - min);
        bar.setProgress(initial - min);
        bar.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override public void onProgressChanged(SeekBar sb, int progress, boolean fromUser) {
                int v = progress + min;
                label.setText(glyphLabel(name, ": " + v + unit));
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
        b.setText(glyph(name) + " " + name + ": " + (initial ? "ON" : "OFF"));
        b.setTextSize(15);
        b.setTextColor(initial ? 0xFF66FF99 : 0xFF888888);
        b.setPadding(dp(4), dp(3), dp(4), dp(3));
        b.setOnClickListener(v -> {
            boolean on = b.getCurrentTextColor() != 0xFF66FF99;
            b.setText(glyph(name) + " " + name + ": " + (on ? "ON" : "OFF"));
            b.setTextColor(on ? 0xFF66FF99 : 0xFF888888);
            nativeSetShadow(key, on ? 1 : 0);
            if (key == 0) {
                shadowOn = on;
            }
            refreshTabStyles();
            Log.i(TAG, name + "=" + on);
        });
        addAboveGrid(b, 0);
        nativeSetShadow(key, initial ? 1 : 0);
        return b;
    }

    private TextView addShadowButton(String name, final Runnable action) {
        final TextView b = new TextView(this);
        b.setText("\u25C9 " + name);
        b.setTextSize(16);
        b.setTextColor(0xFF66CCFF);
        b.setPadding(dp(4), dp(10), dp(4), dp(10));
        b.setOnClickListener(v -> action.run());
        addAboveGrid(b, 0);
        return b;
    }

    private void applyEnhState(TextView b, String name, boolean on) {
        b.setText(glyph(name) + " " + name + ": " + (on ? "ON" : "OFF"));
        b.setTextColor(on ? 0xFF66FF99 : 0xFF888888);
    }

    private void applyShadowEnable(TextView b, boolean on) {
        b.setText(glyph("shadow enable") + " SHADOW ENABLE: " + (on ? "ON" : "OFF"));
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
        label.setText(glyphLabel(name, ": " + initial + unit));

        SeekBar bar = new CompactSeekBar();
        bar.setMax(max - min);
        bar.setProgress(initial - min);
        bar.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override
            public void onProgressChanged(SeekBar seekBar, int progress, boolean fromUser) {
                int value = progress + min;
                label.setText(glyphLabel(name, ": " + value + unit));
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
        b.setText(glyph(name) + " " + name + ": " + (initial ? "ON" : "OFF"));
        b.setTextSize(15);
        b.setTextColor(initial ? 0xFF66FF99 : 0xFF888888);
        b.setPadding(dp(4), dp(3), dp(4), dp(3));
        b.setOnClickListener(v -> {
            boolean on = b.getCurrentTextColor() != 0xFF66FF99;
            applyFxToggle(b, name, key, on);
        });
        addAboveGrid(b, 0);
        nativeSetSSFX(key, initial ? 1 : 0);
        return b;
    }

    /* OCCLUSION group on CAMERA > SETTINGS, next to WALL NOCLIP: both decide
     * what happens to geometry that sits between the camera and Mario.  The
     * mode button cycles OFF / DROPOUT / ALPHA / CUTOUT (fx key 274). */
    private void addOcclusionControls() {
        final String[] names = { "OFF", "DROPOUT", "ALPHA", "CUTOUT" };
        beginSection("OCCLUSION (PUNCH-THROUGH)", false);
        occlButton = new TextView(this);
        occlButton.setTextSize(15);
        occlButton.setGravity(Gravity.CENTER);
        occlButton.setPadding(dp(4), dp(10), dp(4), dp(10));
        occlButton.setOnClickListener(v -> {
            setOcclusionMode((occlMode + 1) % names.length);
            Log.i(TAG, "OCCLUSION mode=" + occlMode + " " + names[occlMode]);
        });
        addAboveGrid(occlButton, 0);
        setOcclusionMode(occlMode);

        TextView hint = new TextView(this);
        hint.setTextSize(12);
        hint.setTextColor(0xFFAACCFF);
        hint.setPadding(dp(4), 0, dp(4), dp(6));
        hint.setTextSize(11);
        hint.setText("Discards only geometry in front of Mario inside a circle:"
                + " DROPOUT = hard 100% hole, ALPHA = partial dither, CUTOUT ="
                + " solid hole with a dithered feather. Mario always stays visible.");
        currentParent().addView(hint);

        addFxSlider("Occl radius", 20, 600, 150, " px", 275);
        addFxSlider("Occl feather", 0, 100, 35, " %", 276);
        addFxSlider("Occl dither", 0, 100, 50, " %", 277);
        addFxSlider("Occl depth bias", 0, 100, 5, "", 278);
        endSection();
    }

    private void setOcclusionMode(int mode) {
        final String[] names = { "OFF", "DROPOUT", "ALPHA", "CUTOUT" };
        occlMode = mode & 3;
        nativeSetSSFX(274, occlMode);
        if (occlButton != null) {
            occlButton.setText("OCCLUSION: " + names[occlMode]);
            occlButton.setTextColor(occlMode == 0 ? 0xFF888888 : 0xFF66CCFF);
        }
    }

    /* ------------------------------------------------------------------ *
     * CAM STICK: draggable response curve, deadzone, radial heatmap and the
     * R-shoulder spring/follow behaviours.  All state lives in StickCurve so
     * the panel, the on-screen heatmap and the native camera share it.
     * Native keys: fx 290-299 (see StickCurve / customcam.c).
     * ------------------------------------------------------------------ */
    private StickCurveView camStickView;
    private TextView camStickToggle, camStickHeatToggle, camStickDigToggle;
    private TextView camStickRButton, camStickDzLabel, camStickSpringLabel;
    private TextView camStickDampLabel;
    private boolean camStickUpdating;

    private void addCamStickControls() {
        TextView head = new TextView(this);
        head.setTextSize(15);
        head.setTextColor(0xFFAACCFF);
        head.setPadding(dp(4), dp(10), dp(4), dp(2));
        head.setText("CAM STICK: curve, deadzone, R-follow");
        controls.addView(head);

        camStickView = new StickCurveView(this);
        controls.addView(camStickView, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, dp(230)));
        {
            LinearLayout row = addRow();
            camStickToggle = addActionCell(row, "CURVE: ON", () -> {
                StickCurve.setEnabled(!StickCurve.enabled);
                refreshCamStickPanel();
                refreshTouchOverlay();
                Log.i(TAG, "CAMSTICK " + StickCurve.describe());
            });
            camStickHeatToggle = addActionCell(row, "HEATMAP: ON", () -> {
                StickCurve.setHeatmap(!StickCurve.heatmap);
                refreshCamStickPanel();
                Log.i(TAG, "CAMSTICK " + StickCurve.describe());
            });
            camStickDigToggle = addActionCell(row, "C-DIGITAL: ON", () -> {
                StickCurve.cstickDigital = !StickCurve.cstickDigital;
                StickCurve.applyCstickDigital();
                refreshCamStickPanel();
                Log.i(TAG, "CAMSTICK vanilla C-stick digital=" + StickCurve.cstickDigital);
            });
        }
        {
            LinearLayout row = addRow();
            camStickRButton = addActionCell(row, "R MODE", () -> {
                StickCurve.setRMode((StickCurve.rMode + 1) % 3);
                refreshCamStickPanel();
                Log.i(TAG, "CAMSTICK R mode=" + StickCurve.rMode);
            });
            addActionCell(row, "RESET LINEAR", () -> {
                StickCurve.resetLinear();
                refreshCamStickPanel();
                refreshTouchOverlay();
                Log.i(TAG, "CAMSTICK reset to linear");
            });
        }

        camStickDzLabel = addCamStickSlider("Stick deadzone", 0, 60,
                StickCurve.deadzonePct, " %", KEY_STICK_DZ);
        camStickSpringLabel = addCamStickSlider("R spring", 10, 300,
                StickCurve.springPct, " %", KEY_STICK_SPRING);
        camStickDampLabel = addCamStickSlider("R damping", 40, 200,
                StickCurve.dampingPct, " %", KEY_STICK_DAMP);

        TextView hint = new TextView(this);
        hint.setTextSize(12);
        hint.setTextColor(0xFFAACCFF);
        hint.setPadding(dp(4), 0, dp(4), dp(6));
        hint.setText("Drag the three green points; the ends are pinned and the"
                + " curve is clamped monotonic. RESPONSE draws the same curve as"
                + " the heatmap in the stick circle. R MODE cycles TOGGLE FOLLOW"
                + " / HOLD / SNAP+RELEASE (see the R-follow section below).");
        controls.addView(hint);
        refreshCamStickPanel();
    }

    /** fx keys owned by this feature (StickCurve.KEY_BASE + n). */
    private static final int KEY_STICK_DZ = StickCurve.KEY_BASE + 1;
    private static final int KEY_STICK_SPRING = StickCurve.KEY_BASE + 6;
    private static final int KEY_STICK_DAMP = StickCurve.KEY_BASE + 7;

    private TextView addCamStickSlider(String name, int min, int max, int initial,
                                       String unit, final int key) {
        final TextView label = new TextView(this);
        label.setTextColor(Color.WHITE);
        label.setTextSize(14);
        label.setText(name + ": " + initial + unit);
        final SeekBar bar = new SeekBar(this);
        bar.setMax(max - min);
        bar.setProgress(initial - min);
        bar.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override
            public void onProgressChanged(SeekBar sb, int progress, boolean fromUser) {
                if (!fromUser || camStickUpdating) {
                    return;
                }
                int v = progress + min;
                label.setText(name + ": " + v + unit);
                if (key == KEY_STICK_DZ) {
                    StickCurve.setDeadzone(v);
                } else if (key == KEY_STICK_SPRING) {
                    StickCurve.setSpring(v);
                } else {
                    StickCurve.setDamping(v);
                }
                refreshTouchOverlay();
            }
            @Override public void onStartTrackingTouch(SeekBar sb) {}
            @Override public void onStopTrackingTouch(SeekBar sb) {}
        });
        addToGrid(label, bar);
        return label;
    }

    /** Re-label the cam-stick controls after a programmatic change. */
    void refreshCamStickPanel() {
        camStickUpdating = true;
        if (camStickToggle != null) {
            camStickToggle.setText("CURVE: " + (StickCurve.enabled ? "ON" : "OFF"));
            camStickToggle.setTextColor(StickCurve.enabled ? 0xFF66FF99 : 0xFF888888);
        }
        if (camStickHeatToggle != null) {
            camStickHeatToggle.setText("HEATMAP: " + (StickCurve.heatmap ? "ON" : "OFF"));
            camStickHeatToggle.setTextColor(StickCurve.heatmap ? 0xFF66FF99 : 0xFF888888);
        }
        if (camStickDigToggle != null) {
            camStickDigToggle.setText("C-DIGITAL: " + (StickCurve.cstickDigital ? "ON" : "OFF"));
            camStickDigToggle.setTextColor(
                    StickCurve.cstickDigital ? 0xFF66FF99 : 0xFF888888);
        }
        if (camStickRButton != null) {
            final String[] names = { "TOGGLE FOLLOW", "HOLD TO FOLLOW", "SNAP+RELEASE" };
            camStickRButton.setText("R MODE: " + names[StickCurve.rMode]);
        }
        if (camStickDzLabel != null) {
            camStickDzLabel.setText("Stick deadzone: " + StickCurve.deadzonePct + " %");
        }
        if (camStickSpringLabel != null) {
            camStickSpringLabel.setText("R spring: " + StickCurve.springPct + " %");
        }
        if (camStickDampLabel != null) {
            camStickDampLabel.setText("R damping: " + StickCurve.dampingPct + " %");
        }
        camStickUpdating = false;
        if (camStickView != null) {
            camStickView.invalidate();
        }
    }

    /** Bridge for StickCurve.pushNative() (nativeSetSSFX is private here). */
    void stickFx(int key, int value) {
        nativeSetSSFX(key, value);
    }

    void refreshTouchOverlay() {
        if (touchOverlay != null) {
            touchOverlay.invalidate();
        }
    }

    private void applyFxToggle(TextView b, String name, int key, boolean on) {
        b.setText(glyph(name) + " " + name + ": " + (on ? "ON" : "OFF"));
        b.setTextColor(on ? 0xFF66FF99 : 0xFF888888);
        nativeSetSSFX(key, on ? 1 : 0);
        if (key == 1) aoOn = on;
        else if (key == 100) mbOn = on;
        else if (key == 110) probeOn = on;
        else if (key == 11) dofOn = on;
        else if (key == 21) giOn = on;
        else if (key == 70) ssrtxOn = on;
        else if (key == 250) raysOn = on;
        refreshTabStyles();
        Log.i(TAG, "FX toggle " + name + "=" + (on ? 1 : 0));
    }

    /** Cycle button for a small enum-style fx key (tonemap operator, look). */
    private TextView addFxCycle(String name, final int key, final String[] labels,
                                int initial) {
        final TextView b = new TextView(this);
        int start = (initial < 0 || initial >= labels.length) ? 0 : initial;
        b.setTag(start);
        b.setTextSize(15);
        b.setGravity(Gravity.CENTER);
        b.setPadding(dp(4), dp(10), dp(4), dp(10));
        b.setText(name + ": " + labels[start]);
        b.setTextColor(start == 0 ? 0xFF888888 : 0xFF66CCFF);
        b.setOnClickListener(v -> {
            int next = (((Integer) b.getTag()) + 1) % labels.length;
            b.setTag(next);
            b.setText(name + ": " + labels[next]);
            b.setTextColor(next == 0 ? 0xFF888888 : 0xFF66CCFF);
            debugFx(key, next);
            Log.i(TAG, "FX cycle " + name + "=" + next);
        });
        addCompact(controls, b);
        nativeSetSSFX(key, start);
        return b;
    }

    /** Put the current page's slider grid after the rows already added to it. */
    private void moveGridLast() {
        if (grid == null || controls == null) return;
        controls.removeView(grid);
        controls.addView(grid);
        aboveGridIndex = controls.indexOfChild(grid);
    }

    // ---- compact debug-panel layout helpers --------------------------------

    /**
     * Glyph for a control label, so a row is glanceable without reading it.
     * One shared mapping for every page (Task A iconography).
     */
    private static String glyph(String name) {
        String n = " " + name.toLowerCase(java.util.Locale.US) + " ";
        if (n.contains(" on/off") || n.contains("enable") || n.contains("show")
                || n.contains("toggle") || n.contains("visible")
                || n.contains(": on") || n.contains("auto")) return "\u25C9";  // ◉
        if (n.contains("intens") || n.contains("strength") || n.contains("gain")
                || n.contains("bright") || n.contains("glow") || n.contains("power")
                || n.contains("opacity") || n.contains("alpha")) return "\u2600"; // ☀
        if (n.contains("radius") || n.contains("distance") || n.contains("range")
                || n.contains("size") || n.contains("width") || n.contains("height")
                || n.contains("spacing") || n.contains("margin") || n.contains("length")
                || n.contains("offset") || n.contains("thickness")) return "\u21D4"; // ⇔
        if (n.contains("decay") || n.contains("falloff") || n.contains("fade")
                || n.contains("time") || n.contains("every") || n.contains("frame")
                || n.contains("shutter") || n.contains("update") || n.contains("ago")) return "\u23F1"; // ⏱
        if (n.contains("passes") || n.contains("samples") || n.contains("steps")
                || n.contains("taps") || n.contains("bounce") || n.contains("iterations")
                || n.contains("repeat")) return "\u259A"; // ▚ stair
        if (n.contains("soft") || n.contains("feather") || n.contains("curve")
                || n.contains("smooth") || n.contains("jitter") || n.contains("weight")
                || n.contains("rough") || n.contains("clamp")) return "\u2248"; // ≈
        if (n.contains("angle") || n.contains("rotation") || n.contains("yaw")
                || n.contains("pitch") || n.contains("deg") || n.contains("toe")
                || n.contains("aspect")) return "\u2220"; // ∠
        if (n.contains("color") || n.contains("colour") || n.contains("tint")
                || n.contains(" R") || n.contains(" G") || n.contains(" B")
                || n.contains("red") || n.contains("green") || n.contains("blue")) return "\u25D1"; // ◑
        if (n.contains("res") || n.contains("buffer") || n.contains("texture")
                || n.contains("bias") || n.contains("depth")) return "\u25A4"; // ▤
        if (n.contains("grid") || n.contains("cols") || n.contains("rows")) return "\u2317"; // ⌗
        if (n.contains("bloom") || n.contains("emissive") || n.contains("blades")
                || n.contains("blur")) return "\u2726"; // ✦
        if (n.contains("speed") || n.contains("velocity") || n.contains("fps")
                || n.contains("hz")) return "\u21C9"; // ⇉
        if (n.contains("camera") || n.contains("cam") || n.contains("focus")
                || n.contains("fov") || n.contains("lens") || n.contains("bokeh")) return "\u25CE"; // ◎
        if (n.contains("portal") || n.contains("relay") || n.contains("eye")
                || n.contains("stereo") || n.contains("ipd")) return "\u25C8"; // ◈
        if (n.contains("levels") || n.contains("scene") || n.contains("warp")
                || n.contains("test")) return "\u2691"; // ⚑
        return "\u00B7"; // ·
    }

    /** Label text with its glyph prefix. */
    private static String glyphLabel(String name, String value) {
        return glyph(name) + " " + name + value;
    }

    /** "glyph NAME: ON/OFF", the shared shape of every toggle row. */
    private static String toggleLabel(String name, boolean on) {
        return glyph(name) + " " + name + ": " + (on ? "ON" : "OFF");
    }

    private int pageMaxHeight() {
        int h = getResources().getDisplayMetrics().heightPixels;
        // Horizontal: the drawer keeps the upper third of the game visible.
        // Vertical: the left half can use almost the whole height.
        int raw = panelVertical ? h - dp(420) : (int) (h * 0.62f);
        // 1600px panel / density 2: keep it under the screen even with tabs.
        return Math.min(raw, h - dp(60));
    }

    /**
     * Page scroll container that is only as tall as its content needs, capped
     * at pageMaxHeight(). A page that fits therefore never scrolls; one that
     * cannot still degrades to a normal scroll container.
     */
    private final class PageScroll extends android.widget.ScrollView {
        int maxH;
        /** Vertical dock: fill the height the parent hands down and scroll
         *  inside it (the dock is pinned top-to-bottom). Horizontal keeps the
         *  wrapped, content-sized page. */
        boolean fillHeight;

        PageScroll() {
            super(sm64portActivity.this);
            setFillViewport(false);
            setOverScrollMode(OVER_SCROLL_NEVER);
            maxH = pageMaxHeight();
            pageScrollers.add(this);
        }

        @Override
        protected void onMeasure(int widthSpec, int heightSpec) {
            if (fillHeight) {
                super.onMeasure(widthSpec, heightSpec);
                return;
            }
            int cap = Math.max(dp(80), maxH);
            super.onMeasure(widthSpec,
                    MeasureSpec.makeMeasureSpec(cap, MeasureSpec.AT_MOST));
        }

        void setMaxHeight(int h) {
            maxH = h;
            requestLayout();
        }
    }

    /** Compact two-column slider grid that also anchors the toggle order. */
    private android.widget.GridLayout newGrid(LinearLayout parent) {
        android.widget.GridLayout g = new android.widget.GridLayout(this);
        g.setColumnCount(2);
        g.setOrientation(android.widget.GridLayout.HORIZONTAL);
        g.setUseDefaultMargins(false);
        g.setAlignmentMode(android.widget.GridLayout.ALIGN_BOUNDS);
        parent.addView(g);
        grid = g;
        aboveGridIndex = parent.indexOfChild(g);
        return g;
    }

    /**
     * Insert a toggle above the page's slider grid, keeping insertion order.
     * Task rule: a tab's enable/show switch is the first control on the page.
     */
    private void addAboveGrid(View v, int height) {
        addAboveGrid(v, height, ViewGroup.LayoutParams.WRAP_CONTENT);
    }

    private void addAboveGrid(View v, int height, int width) {
        LinearLayout parent = sectionBody != null ? sectionBody : controls;
        if (parent == null) return;
        int gi = grid != null ? parent.indexOfChild(grid) : -1;
        int at = gi < 0 ? parent.getChildCount() : Math.min(aboveGridIndex, gi);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                width, height > 0 ? height : ViewGroup.LayoutParams.WRAP_CONTENT);
        lp.setMargins(dp(2), dp(1), dp(8), dp(1));
        parent.addView(v, at, lp);
        aboveGridIndex = at + 1;
    }

    /* Collapsible sections: rarely used controls stay reachable behind a
     * disclosure instead of pushing the common ones off the page. */
    private final java.util.ArrayDeque<Object[]> sectionStack = new java.util.ArrayDeque<>();
    private LinearLayout sectionBody;

    private LinearLayout currentParent() {
        if (sectionBody != null) return sectionBody;
        return enhColumnTarget != null ? enhColumnTarget : controls;
    }

    private static void updateSectionHeader(TextView h, String title, boolean open) {
        h.setText((open ? "\u25BE " : "\u25B8 ") + title);
    }

    /** Add a collapsible section; returns the body to add controls to. */
    private LinearLayout beginSection(String title, boolean open) {
        LinearLayout parent = currentParent();
        final TextView head = new TextView(this);
        head.setTextSize(13);
        head.setTextColor(0xFF66CCFF);
        head.setPadding(dp(6), dp(4), dp(6), dp(4));
        head.setBackgroundColor(0x33FFFFFF);
        final LinearLayout body = new LinearLayout(this);
        body.setOrientation(LinearLayout.VERTICAL);
        body.setVisibility(open ? View.VISIBLE : View.GONE);
        updateSectionHeader(head, title, open);
        head.setOnClickListener(v -> {
            boolean show = body.getVisibility() != View.VISIBLE;
            body.setVisibility(show ? View.VISIBLE : View.GONE);
            updateSectionHeader(head, title, show);
        });
        parent.addView(head, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        parent.addView(body, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        sectionStack.push(new Object[] {
                grid, sectionBody, parent, Integer.valueOf(aboveGridIndex) });
        sectionBody = body;
        if (grid != null) {
            grid = newGrid(body);
        }
        return body;
    }

    private void endSection() {
        if (sectionStack.isEmpty()) return;
        Object[] st = sectionStack.pop();
        grid = (android.widget.GridLayout) st[0];
        sectionBody = (LinearLayout) st[1];
        aboveGridIndex = ((Integer) st[3]).intValue();
    }

    /**
     * Row of chips that wraps to a second line when the dock is narrow
     * (vertical mode). It behaves like a normal horizontal LinearLayout
     * whenever everything fits on one line.
     */
    private final class FlowBar extends LinearLayout {
        FlowBar() {
            super(sm64portActivity.this);
            setOrientation(HORIZONTAL);
        }

        @Override
        protected void onMeasure(int widthSpec, int heightSpec) {
            int width = MeasureSpec.getSize(widthSpec);
            if (width <= 0) {
                super.onMeasure(widthSpec, heightSpec);
                return;
            }
            int left = getPaddingLeft();
            int usable = width - left - getPaddingRight();
            measureChildren(MeasureSpec.makeMeasureSpec(usable, MeasureSpec.AT_MOST),
                    MeasureSpec.makeMeasureSpec(0, MeasureSpec.UNSPECIFIED));
            int x = 0, y = 0, rowH = 0;
            for (int i = 0; i < getChildCount(); i++) {
                View c = getChildAt(i);
                if (c.getVisibility() == GONE) continue;
                int cw = c.getMeasuredWidth();
                int ch = c.getMeasuredHeight();
                if (x > 0 && x + cw > usable) {
                    x = 0;
                    y += rowH;
                    rowH = 0;
                }
                x += cw;
                if (ch > rowH) rowH = ch;
            }
            setMeasuredDimension(width,
                    y + rowH + getPaddingTop() + getPaddingBottom());
        }

        @Override
        protected void onLayout(boolean changed, int l, int t, int r, int b) {
            int usable = (r - l) - getPaddingLeft() - getPaddingRight();
            int x = 0, y = getPaddingTop(), rowH = 0;
            for (int i = 0; i < getChildCount(); i++) {
                View c = getChildAt(i);
                if (c.getVisibility() == GONE) continue;
                int cw = c.getMeasuredWidth();
                int ch = c.getMeasuredHeight();
                if (x > 0 && x + cw > usable) {
                    x = 0;
                    y += rowH;
                    rowH = 0;
                }
                c.layout(getPaddingLeft() + x, y,
                        getPaddingLeft() + x + cw, y + ch);
                x += cw;
                if (ch > rowH) rowH = ch;
            }
        }
    }

    /** Compact SeekBar: a normal bar in a single-line row. */
    private final class CompactSeekBar extends SeekBar {
        CompactSeekBar() {
            super(sm64portActivity.this);
            setPadding(0, 0, 0, 0);
            setMinimumHeight(0);
            setMinimumWidth(0);
            setThumbOffset(dp(4));
            setSplitTrack(false);
        }

        @Override
        protected void onMeasure(int widthSpec, int heightSpec) {
            super.onMeasure(widthSpec,
                    MeasureSpec.makeMeasureSpec(dp(22), MeasureSpec.EXACTLY));
        }
    }

    /** Add a button-like TextView at its natural width, not full row width. */
    private void addCompact(android.view.ViewGroup parent, TextView b) {
        b.setBackgroundColor(0x22FFFFFF);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        lp.setMargins(dp(1), dp(1), dp(6), dp(1));
        parent.addView(b, lp);
    }

    /** Horizontal row inside the current tab page (compact multi-column layout). */
    private LinearLayout addRow() {
        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        currentParent().addView(row);
        return row;
    }

    /** Toggle cell for a row built with {@link #addRow()}. */
    private TextView addToggleCell(LinearLayout row, String name, final int key, boolean initial) {
        final TextView b = new TextView(this);
        b.setText(glyph(name) + " " + name + ": " + (initial ? "ON" : "OFF"));
        b.setTextSize(13);
        b.setTextColor(initial ? 0xFF66FF99 : 0xFF888888);
        b.setPadding(dp(4), dp(4), dp(4), dp(4));
        b.setOnClickListener(v -> {
            boolean on = b.getCurrentTextColor() != 0xFF66FF99;
            applyFxToggle(b, name, key, on);
        });
        row.addView(b, new LinearLayout.LayoutParams(0,
                ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        nativeSetSSFX(key, initial ? 1 : 0);
        return b;
    }

    /** Debug-view cell (shares the highlight state with the EFFECTS buttons). */
    private TextView addDebugCell(LinearLayout row, String name, final int mode) {
        TextView b = new TextView(this);
        b.setText(name);
        b.setTextSize(14);
        b.setTextColor(0xFF888888);
        b.setPadding(dp(4), dp(8), dp(4), dp(8));
        b.setOnClickListener(v -> setFxDebug(mode));
        row.addView(b, new LinearLayout.LayoutParams(0,
                ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        fxDebugButtons.add(b);
        fxDebugModes.add(mode);
        return b;
    }

    /** Action cell for a row built with {@link #addRow()}. */
    private TextView addActionCell(LinearLayout row, String name, final Runnable action) {
        TextView b = new TextView(this);
        b.setText("\u25B8 " + name);
        b.setTextSize(14);
        b.setTextColor(0xFFFFCC66);
        b.setPadding(dp(4), dp(10), dp(4), dp(10));
        b.setOnClickListener(v -> action.run());
        addPressFeedback(b);
        row.addView(b, new LinearLayout.LayoutParams(0,
                ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        return b;
    }

    private void addFxDebugButton(String name, final int mode) {
        TextView b = new TextView(this);
        b.setText("\u25C9 " + name);
        b.setTextSize(14);
        b.setTextColor(0xFF888888);
        b.setPadding(dp(4), dp(6), dp(4), dp(6));
        b.setOnClickListener(v -> setFxDebug(mode));
        addAboveGrid(b, 0);
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

    /* ------------------------------------------------------------------ *
     * On-screen layout editor: panel plumbing + headless debug control.
     * ------------------------------------------------------------------ */

    /** Sync the SYS > TOUCH LAYOUT widgets with the overlay's current state. */
    private void refreshLayoutUi() {
        if (layoutEditButton != null && touchOverlay != null) {
            boolean on = touchOverlay.isEditMode();
            layoutEditButton.setText("EDIT LAYOUT: " + (on ? "ON" : "OFF"));
            layoutEditButton.setTextColor(on ? 0xFFFFCC66 : 0xFF888888);
        }
        if (layoutProfileButton != null && touchOverlay != null) {
            layoutProfileButton.setText("PROFILE: " + touchOverlay.activeProfile()
                    + "  \u25B8");
        }
        if (layoutLayoutStatus != null && touchOverlay != null) {
            layoutLayoutStatus.setText(touchOverlay.listProfiles().size()
                    + " profiles  -  ext: " + touchOverlay.externalDir());
        }
    }

    /** Short status line under the TOUCH LAYOUT buttons (no toast overlay). */
    private void layoutNote(String text) {
        if (layoutLayoutStatus != null) {
            layoutLayoutStatus.setText(text);
        }
        Log.i(TAG, "LAYOUT " + text);
    }

    /** Simple text prompt (profile save/rename). */
    private void promptText(String title, String initial,
                            final java.util.function.Consumer<String> done) {
        final android.widget.EditText input = new android.widget.EditText(this);
        input.setText(initial == null ? "" : initial);
        input.setSelectAllOnFocus(true);
        input.setSingleLine(true);
        try {
            new android.app.AlertDialog.Builder(this)
                    .setTitle(title)
                    .setView(input)
                    .setPositiveButton("OK", (d, w) -> {
                        String s = input.getText().toString().trim();
                        if (!s.isEmpty()) done.accept(s);
                    })
                    .setNegativeButton("CANCEL", null)
                    .show();
        } catch (Throwable t) {
            layoutNote("dialog failed: " + t);
        }
    }

    /**
     * fx keys 300-309: layout editor / profiles.
     *   300 edit mode on/off        301 reset layout
     *   302 select element          303 selected x (per-mille)
     *   304 selected y (per-mille)  305 selected scale (percent)
     *   306 switch to profile index 307 save as profile p&lt;v&gt;
     *   308 export active profile   309 log status
     */
    private void handleLayoutFx(int k, int v) {
        if (touchOverlay == null) {
            Log.w(TAG, "layout fx " + k + " ignored (no overlay)");
            return;
        }
        switch (k) {
            case 300:
                touchOverlay.setEditMode(v != 0);
                break;
            case 301:
                touchOverlay.resetLayout();
                break;
            case 302:
                touchOverlay.setSelectedElement(v);
                break;
            case 303:
                touchOverlay.setSelectedNorm(0, v / 1000f);
                break;
            case 304:
                touchOverlay.setSelectedNorm(1, v / 1000f);
                break;
            case 305:
                touchOverlay.setSelectedScale(v / 100f);
                break;
            case 306: {
                java.util.List<String> ps = touchOverlay.listProfiles();
                if (v >= 0 && v < ps.size()) touchOverlay.switchProfile(ps.get(v));
                break;
            }
            case 307:
                touchOverlay.saveProfileAs("p" + v);
                break;
            case 308:
                touchOverlay.exportProfile();
                break;
            case 309:
            default:
                break;
        }
        refreshLayoutUi();
        Log.i(TAG, "FX layout " + k + "=" + v + " " + touchOverlay.statusLine());
    }

    /** `cmd layout` — string operations for headless testing. */
    void debugLayout(String action, String profile, String to) {
        if (touchOverlay == null) {
            Log.w(TAG, "layout cmd ignored (no overlay)");
            return;
        }
        String a = action == null ? "status" : action;
        boolean ok = true;
        switch (a) {
            case "on":
            case "edit":
                touchOverlay.setEditMode(true);
                break;
            case "off":
                touchOverlay.setEditMode(false);
                break;
            case "reset":
                touchOverlay.resetLayout();
                break;
            case "save":
                ok = touchOverlay.saveProfileAs(profile);
                break;
            case "switch":
                ok = touchOverlay.switchProfile(profile);
                break;
            case "rename":
                ok = touchOverlay.renameProfile(profile, to);
                break;
            case "delete":
                ok = touchOverlay.deleteProfile(profile);
                break;
            case "export":
                touchOverlay.exportProfile();
                break;
            case "select":
                try {
                    touchOverlay.setSelectedElement(Integer.parseInt(profile));
                } catch (Throwable t) {
                    ok = false;
                }
                break;
            case "x":
                try {
                    touchOverlay.setSelectedNorm(0, Integer.parseInt(profile) / 1000f);
                } catch (Throwable t) {
                    ok = false;
                }
                break;
            case "y":
                try {
                    touchOverlay.setSelectedNorm(1, Integer.parseInt(profile) / 1000f);
                } catch (Throwable t) {
                    ok = false;
                }
                break;
            case "scale":
                try {
                    touchOverlay.setSelectedScale(Integer.parseInt(profile) / 100f);
                } catch (Throwable t) {
                    ok = false;
                }
                break;
            case "profiles":
                Log.i(TAG, "LAYOUT profiles=" + touchOverlay.listProfiles()
                        + " active=" + touchOverlay.activeProfile());
                break;
            case "status":
            default:
                break;
        }
        refreshLayoutUi();
        Log.i(TAG, "LAYOUT cmd " + a + " ok=" + ok + " " + touchOverlay.statusLine());
    }

    /** Output-layer transform knobs. kind: 0=H% 1=V% 2=Wpx 3=Hshift 4=Vshift */
    private void addOutSlider(String name, int min, int max, int initial, final int kind) {
        final TextView label = new TextView(this);
        label.setTextColor(Color.WHITE);
        label.setTextSize(14);
        label.setText(glyphLabel(name, ": " + initial));

        SeekBar bar = new CompactSeekBar();
        bar.setMax(max - min);
        bar.setProgress(initial - min);
        bar.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override
            public void onProgressChanged(SeekBar seekBar, int progress, boolean fromUser) {
                int value = progress + min;
                label.setText(glyphLabel(name, ": " + value));
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
        label.setText(glyphLabel(name, which == 57
                ? ": " + aspectLabelTenths(initial)
                : ": " + initial + unit));

        SeekBar bar = new CompactSeekBar();
        bar.setMax(max - min);
        bar.setProgress(initial - min);
        bar.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override
            public void onProgressChanged(SeekBar seekBar, int progress, boolean fromUser) {
                int value = progress + min;
                label.setText(glyphLabel(name, which == 57
                        ? ": " + aspectLabelTenths(value)
                        : ": " + value + unit));
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
        label.setText(glyphLabel(name, ": " + initial));
        SeekBar bar = new CompactSeekBar();
        bar.setMax(max - min);
        bar.setProgress(initial - min);
        bar.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override public void onProgressChanged(SeekBar sb, int progress, boolean fromUser) {
                int value = progress + min;
                label.setText(glyphLabel(name, ": " + value));
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
        b.setTextSize(15);
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
        col.setPadding(0, 0, dp(6), dp(1));
        label.setTextSize(12);
        label.setPadding(0, 0, 0, 0);
        col.addView(label);

        LinearLayout barRow = new LinearLayout(this);
        barRow.setOrientation(LinearLayout.HORIZONTAL);
        barRow.setGravity(Gravity.CENTER_VERTICAL);
        TextView minus = stepButton("-", bar, -1);
        TextView plus = stepButton("+", bar, +1);
        barRow.addView(minus, new LinearLayout.LayoutParams(dp(24),
                ViewGroup.LayoutParams.MATCH_PARENT));
        barRow.addView(bar, new LinearLayout.LayoutParams(0,
                ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        barRow.addView(plus, new LinearLayout.LayoutParams(dp(24),
                ViewGroup.LayoutParams.MATCH_PARENT));
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
                        leiaTexture = texture;
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
                    if (touchOverlay.isEditMode()) {
                        touchOverlay.setControlsHidden(false);
                    } else if (forceHudHidden) {
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
                refreshLayoutUi();
                if (portalGunButton != null) {
                    String want = portalGunLabel();
                    if (!want.contentEquals(portalGunButton.getText())) {
                        portalGunButton.setText("\u25B8 " + want);
                    }
                }
                debugView.setTextColor(busyOn ? 0xFFFF5555 : 0xFFAAFFAA);
                String note = nativeGetDebugNote();
                debugView.setText((note == null || note.isEmpty() ? "" : note + "\n")
                        + (busyOn ? "BUSY - hands off  |  " : "FREE  |  ")
                        + BUILD_NAME + "  " + buildBranch());
                // Feedback badge: the directory can change behind the app when
                // an agent pulls and deletes over adb, so re-count every 2 s.
                if ((feedbackPollTick++ & 3) == 0) {
                    refreshFeedbackBadge();
                }
                h.postDelayed(this, 500);
            }
        });
    }

    /**
     * Feedback outbox controls, directly under the FPS meter / frame graph:
     * one tap captures (no text), long press captures with a note, CLEAR
     * empties the outbox for the user. Agents pull and delete over adb instead
     * (see AGENTS.md); the item count is the badge.
     */
    private void buildFeedbackBar() {
        if (mLayout == null) return;
        LinearLayout bar = new LinearLayout(this);
        bar.setOrientation(LinearLayout.HORIZONTAL);
        bar.setBackgroundColor(0x55000000);

        TextView fb = new TextView(this);
        feedbackButton = fb;
        fb.setTextSize(13);
        fb.setTextColor(0xFFFFD966);
        fb.setPadding(dp(8), dp(4), dp(8), dp(4));
        fb.setOnClickListener(v -> captureFeedback(null));
        fb.setOnLongClickListener(v -> {
            promptFeedbackText();
            return true;
        });
        bar.addView(fb);

        TextView clear = new TextView(this);
        clear.setText("CLEAR");
        clear.setTextSize(11);
        clear.setTextColor(0xFF999999);
        clear.setPadding(dp(8), dp(4), dp(6), dp(4));
        clear.setOnClickListener(v -> {
            int n = FeedbackOutbox.clear(this);
            refreshFeedbackBadge();
            Toast.makeText(this, n == 0 ? "feedback outbox already empty"
                    : "cleared " + n + " feedback item(s)", Toast.LENGTH_SHORT).show();
        });
        bar.addView(clear);

        android.widget.RelativeLayout.LayoutParams lp =
                new android.widget.RelativeLayout.LayoutParams(
                        android.widget.RelativeLayout.LayoutParams.WRAP_CONTENT,
                        android.widget.RelativeLayout.LayoutParams.WRAP_CONTENT);
        lp.topMargin = dp(112);      // FPS text at 0, graph ends at 30+78
        lp.leftMargin = dp(4);
        mLayout.addView(bar, lp);
        feedbackBar = bar;
        refreshFeedbackBadge();
    }

    /** Pending-item badge: "FEEDBACK (n)"; long press adds a note. */
    private void refreshFeedbackBadge() {
        if (feedbackButton == null) return;
        int n = FeedbackOutbox.count(this);
        feedbackButton.setText(n > 0 ? "FEEDBACK (" + n + ")" : "FEEDBACK");
        feedbackButton.setTextColor(n > 0 ? 0xFFFFD966 : 0xFFCCCCCC);
    }

    /** Long-press path: optional text, then the same capture. */
    private void promptFeedbackText() {
        final android.widget.EditText input = new android.widget.EditText(this);
        input.setHint("what to look at / what went wrong (optional)");
        input.setTextSize(15);
        new android.app.AlertDialog.Builder(this)
                .setTitle("Feedback")
                .setView(input)
                .setPositiveButton("CAPTURE", (d, w) ->
                        captureFeedback(input.getText().toString()))
                .setNegativeButton("CANCEL", null)
                .show();
    }

    private void captureFeedback(String text) {
        FeedbackOutbox.capture(this, text == null ? "" : text, this::refreshFeedbackBadge);
    }

    /** adb: cmd feedback [--es text "..."] - remote capture for tests. */
    void debugFeedback(String text) {
        Log.i(TAG, "FEEDBACK capture requested (remote)");
        runOnUiThread(() -> captureFeedback(text == null ? "" : text));
    }


    /** which: 0 = IPD px, 1 = convergence px, 2 = depth percent. */
    private native void nativeSetStereo(int which, int value);
    private native void nativeSetStereoMode(int enabled);
    private native void nativeSetStereoInt(int which, int value);

    /** Screen-space FX knobs, implemented by the per-eye post pass. */
    private native void nativeSetSSFX(int which, int value);
    private native int nativeGetSSFX(int which);
    /** GRADE LUT pixels (ARGB_8888 from Bitmap.getPixels); null clears it. */
    private native void nativeSetGradeLut(int[] pixels, int w, int h);
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
    static native void nativeJetpack(int on);
    static native void nativeSetGameHudHidden(int on);
    static native int nativeFrameTimeUs();
    static native int nativeGetRateState(int which);
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
    /* Captions: 1 = the Java overlay owns the text, 0 = native HUD glyphs. */
    static native void nativeSetCaptionMode(int javaOwns);
    static native int nativeGetCaptionMode();
    static native String nativeGetCaptionText(int which);
    static native int nativeGetCaptionPos(int which, int field);
    /** QUICK DEV cheap boot: on + boot level (read by thread5_game_loop). */
    private native void nativeSetQuickDev(int on, int level);
    private native void nativeSetQuickDevMute(int on);
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
                    if (touchOverlay.isEditMode()) {
                        touchOverlay.setControlsHidden(false);
                    } else if (forceHudHidden) {
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
            pauseLoopToggle.setText(toggleLabel("PAUSE GAME LOOP", false));
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
            portalGunButton.setText("\u25B8 " + portalGunLabel());
        }
    }

    void debugFx(int k, int v) {
        if (k >= 300 && k <= 309) {
            handleLayoutFx(k, v);
            return;
        }
        if (k == 360) {
            /* Keep the CAMERA > SETTINGS pills in step with a remote change and
             * persist it, so `cmd fx 360 1` survives a relaunch too. */
            setCamGroundClip(v, true);
            Log.i(TAG, "FX set " + k + "=" + v);
            return;
        }
        nativeSetSSFX(k, v);
        if (k == 284 && v == 6) reloadGradeLut();
        Log.i(TAG, "FX set " + k + "=" + v);
    }

    /**
     * Decode + validate the optional GRADE LUT and hand it to the renderer.
     *
     * File: <external files>/luts/grade.png
     *       = /sdcard/Android/data/com.vdavid003.sm64port/files/luts/grade.png
     * Format: horizontal strip of 16x16 blue slices, width 16*N, height 16
     *         (256x16 = 16 slices). Row 0 is green = 0, column 0 is red = 0.
     * Re-send `cmd fx --ei k 284 --ei v 6` after dropping a new file.
     */
    private void reloadGradeLut() {
        java.io.File dir = getExternalFilesDir(null);
        java.io.File f = new java.io.File(dir != null ? dir : getFilesDir(), "luts/grade.png");
        String tag = "SM64FX";
        if (!f.exists()) {
            nativeSetGradeLut(null, 0, 0);
            Log.i(tag, "GRADE LUT: no file at " + f + " -- analytic looks only");
            return;
        }
        android.graphics.Bitmap bm = null;
        try {
            bm = android.graphics.BitmapFactory.decodeFile(f.getAbsolutePath());
        } catch (Throwable t) {
            Log.w(tag, "GRADE LUT: decode failed for " + f + ": " + t);
        }
        if (bm == null) {
            nativeSetGradeLut(null, 0, 0);
            Log.w(tag, "GRADE LUT REJECTED: " + f + " is not a decodable image");
            return;
        }
        int w = bm.getWidth();
        int h = bm.getHeight();
        if (h != 16 || w < 32 || w > 1024 || (w % 16) != 0) {
            Log.w(tag, "GRADE LUT REJECTED: " + w + "x" + h
                    + " is not a 16*N x 16 stripe LUT (expected e.g. 256x16)");
            bm.recycle();
            nativeSetGradeLut(null, 0, 0);
            return;
        }
        int[] px = new int[w * h];
        bm.getPixels(px, 0, w, 0, 0, w, h);
        bm.recycle();
        nativeSetGradeLut(px, w, h);
        Log.i(tag, "GRADE LUT loaded: " + f + " (" + w + "x" + h + ", "
                + (w / 16) + " slices)");
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

    /** Pulse the on-screen CAM stick (analog camera stick) for headless tests. */
    void debugPad2(int rx, int ry, int ms) {
        final int dur = ms <= 0 ? 240 : ms;
        runOnUiThread(() -> {
            nativeOverlayStick2(rx, ry, 1);
            new android.os.Handler(getMainLooper()).postDelayed(
                    () -> nativeOverlayStick2(rx, ry, 0), dur);
            Log.i(TAG, "PAD2 rx=" + rx + " ry=" + ry + " ms=" + dur);
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
            case "RAYS":
                runOnUiThread(() -> {
                    raysOn = !raysOn;
                    if (raysToggle != null) applyFxToggle(raysToggle, "CREPUSCULAR RAYS", 250, raysOn);
                    else nativeSetSSFX(250, raysOn ? 1 : 0);
                });
                break;
            case "OCCLUSION":
                runOnUiThread(() -> {
                    setOcclusionMode((occlMode + 1) % 4);
                    Log.i(TAG, "OCCLUSION mode=" + occlMode);
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
            Log.i(TAG, "REINIT requested");
            if (surfaceValid && leiaTexture != null) {
                // Re-bind the existing producer surface and refresh the backlight.
                // Never hide/show the view: that detaches the surface on newer
                // firmware and blacks the display for good.
                final SurfaceTexture tex = leiaTexture;
                weaverLog("REINIT: rebinding existing surface (no view nudge)");
                try {
                    // Rebind only. Restarting the interlacer (pause/resume or
                    // hide/show) destroys the surface SDL is drawing into and
                    // blacks the display; it only works once, at startup.
                    SDLActivity.setOverrideSurface(new Surface(tex));
                    reapplySourceSize();
                    sdk.enableBacklight(true);
                    weaverLog("REINIT: rebind done (no interlacer restart), backlight="
                            + sdk.getBacklight());
                } catch (Throwable t) {
                    weaverError("REINIT rebind failed", t);
                }
            } else {
                // No usable texture: request a fresh one at the SBS size.
                surfaceValid = false;
                weaverLog("REINIT: requesting a fresh SBS surface");
                resizeLeiaSurface(SBS_W, SBS_H);
            }
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
            if (raysToggle != null) applyFxToggle(raysToggle, "CREPUSCULAR RAYS", 250, false);
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
                    + " sim=" + nativeGetSSFX(241) + "/" + nativeGetSSFX(242)
                        + "/" + nativeGetSSFX(243)
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
                    + " range=" + nativeGetShadow(8) + " dbg=" + nativeGetShadow(11)
                    + " fb=" + FeedbackOutbox.count(this));
            Log.i(TAG, nativePtcStatus());
            /* Caption strings reach scripts without a screenshot: the native
             * publisher is the same one the overlay reads. */
            Log.i(TAG, "CAPTIONS mode=" + (captionJavaOn ? "java" : "game")
                    + " native=" + nativeGetCaptionMode()
                    + " size=" + captionSizePct + " opacity=" + captionOpacityPct
                    + " main=\"" + nativeGetCaptionText(0) + "\""
                    + " small=\"" + nativeGetCaptionText(1) + "\"");
            if (touchOverlay != null) {
                Log.i(TAG, "LAYOUT " + touchOverlay.statusLine());
            }
            Log.i(TAG, "UNCAPPED_TEST=" + nativeGetUncappedTest());
            logPanelMetrics();
        });
    }

    /**
     * Debug panel geometry: for every page, the natural content height against
     * the page viewport. `scrolls=1` means the page needs a scroll to reach its
     * last control at the current screen size. Logged by `cmd dump` so the
     * panel layout can be checked without eyeballing screenshots.
     */
    private void logPanelMetrics() {
        if (tabViews.isEmpty()) return;
        int scrolling = 0;
        for (int i = 0; i < tabViews.size(); i++) {
            View sc = tabViews.get(i);
            if (!(sc instanceof android.view.ViewGroup)) continue;
            android.view.ViewGroup vg = (android.view.ViewGroup) sc;
            if (vg.getChildCount() == 0) continue;
            View content = vg.getChildAt(0);
            int w = sc.getWidth();
            if (w <= 0) {
                w = getResources().getDisplayMetrics().widthPixels
                        - (tuningPanelView == null ? 0 : tuningPanelView.getPaddingLeft()
                           + tuningPanelView.getPaddingRight());
            }
            content.measure(View.MeasureSpec.makeMeasureSpec(w, View.MeasureSpec.EXACTLY),
                    View.MeasureSpec.makeMeasureSpec(0, View.MeasureSpec.UNSPECIFIED));
            int ch = content.getMeasuredHeight();
            int vh = sc.getHeight();
            boolean scrolls = ch > vh + 2;
            if (scrolls) scrolling++;
            Log.i(TAG, "PANELMETRIC page=" + i + " content=" + ch + " view=" + vh
                    + " scrolls=" + (scrolls ? 1 : 0));
        }
        Log.i(TAG, "PANELMETRIC total=" + tabViews.size() + " scrolling=" + scrolling
                + " orientation=" + (panelVertical ? "V" : "H"));
        logPanelButtons();
    }

    /** Tap targets of every visible tab/sub-tab, for headless UI scripts. */
    private void logPanelButtons() {
        int[] loc = new int[2];
        StringBuilder b = new StringBuilder();
        b.append("PANELBTN page=").append(selectedPage).append(' ');
        for (int i = 0; i < tabButtons.size(); i++) {
            TextView v = tabButtons.get(i);
            if (!v.isShown() || v.getWidth() <= 0) continue;
            v.getLocationOnScreen(loc);
            b.append("top:").append(v.getText()).append('=')
             .append(loc[0]).append(',').append(loc[1]).append(',')
             .append(v.getWidth()).append('x').append(v.getHeight()).append(' ');
        }
        for (int i = 0; i < subTabButtons.size(); i++) {
            TextView v = subTabButtons.get(i);
            if (!v.isShown() || v.getWidth() <= 0) continue;
            v.getLocationOnScreen(loc);
            b.append("sub:").append(v.getText()).append('=')
             .append(loc[0]).append(',').append(loc[1]).append(',')
             .append(v.getWidth()).append('x').append(v.getHeight()).append(' ');
        }
        if (hideButton != null && hideButton.isShown()) {
            hideButton.getLocationOnScreen(loc);
            b.append("caret=").append(loc[0]).append(',').append(loc[1]).append(',')
             .append(hideButton.getWidth()).append('x').append(hideButton.getHeight()).append(' ');
        }
        for (int i = 0; i < namedPanelButtons.size(); i++) {
            TextView v = (TextView) namedPanelButtons.get(i)[1];
            if (!v.isShown() || v.getWidth() <= 0) continue;
            v.getLocationOnScreen(loc);
            b.append("tab:").append(namedPanelButtons.get(i)[0]).append('=')
             .append(loc[0]).append(',').append(loc[1]).append(',')
             .append(v.getWidth()).append('x').append(v.getHeight()).append(' ');
        }
        if (orientButton != null && orientButton.isShown()) {
            orientButton.getLocationOnScreen(loc);
            b.append("orient=").append(loc[0]).append(',').append(loc[1]).append(',')
             .append(orientButton.getWidth()).append('x').append(orientButton.getHeight())
             .append(' ');
        }
        Log.i(TAG, b.toString());
    }

    @Override
    protected void onDestroy() {
        DebugLog.markCleanExit();
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

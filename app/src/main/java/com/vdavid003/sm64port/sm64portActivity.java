package com.vdavid003.sm64port;

import android.graphics.Color;
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
public class sm64portActivity extends SDLActivity {
    private static final String TAG = "SM64Leia";
    private static final String BUILD_NAME = "v21";
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
    private android.widget.ScrollView scrollerRef;
    private TextView debugView;
    private boolean stereoOn = true;
    private boolean testOn = false;
    private boolean tintOn, zbufOn, wireOn, perfOn, swapOn, sameOn, nocullOn, flatOn;
    private final java.util.List<TextView> fitButtons = new java.util.ArrayList<>();
    private int fitMode = 0;
    private int modeIndex = 0;
    private int refreshIdx = 0;

    /** Cycle the panel refresh rate: default -> 60 -> 120 Hz. */
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

        TextView hide = new TextView(this);
        hide.setText("HIDE");
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

        TextView fpsBtn = new TextView(this);
        fpsBtn.setText("FPS");
        fpsBtn.setTextColor(0xFFCCCCCC);
        fpsBtn.setTextSize(16);
        fpsBtn.setPadding(pad, pad / 2, pad, pad / 2);
        header.addView(fpsBtn, abs(1200, 0, 200, 64));
        fpsBtn.setOnClickListener(v -> cycleRefreshRate());

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
        panel.addView(scroller, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                Math.min(900, maxH)));

        grid = new android.widget.GridLayout(this);
        grid.setColumnCount(2);
        grid.setOrientation(android.widget.GridLayout.HORIZONTAL);
        controls.addView(grid);
        addSlider("IPD", -2560, 2560, 0, " px", 0);
        addSlider("Zero parallax", -1280, 1280, 0, " px", 1);
        addSlider("Depth", -1000, 1000, 10, " %", 2);
        addSlider("Toe in/out", -100, 100, 0, " %", 17);
        addSlider("Aspect % (in)", 10, 240, 178, " %", 18);
        addIntSlider("Aspect % (out)", 0, 240, 0, new java.util.function.IntConsumer() {
            @Override public void accept(int v) { applyOutAspect(v); }
        });
        addSlider("Eye margin%", 50, 100, 100, " %", 14);
        addSlider("FOV %", 1, 200, 100, " %", 20);
        addSlider("Ctl X%", -200, 200, 0, " %", 8);
        addSlider("Ctl Y%", -200, 200, 0, " %", 9);
        addSlider("Ctl scale%", 50, 300, 100, " %", 10);
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
        String[][] tgl = {{"ZBUF", "15"}, {"WIRE", "16"}, {"SWAP", "4"}, {"SAME", "6"}, {"TINT", "7"}, {"PERF2x", "12"}, {"MODE", "-1"},
        {"RCV0", "-2"}, {"RCV50", "-2"}, {"RCV100", "-2"}, {"SRCFIT", "-3"},
                {"FILL", "13"}, {"4:3", "13"}, {"16:9", "13"}};
        for (String[] t : tgl) {
            final int which = Integer.parseInt(t[1]);
            final TextView b = new TextView(this);
            b.setText(t[0]);
            b.setTextColor(0xFF888888);
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
                boolean show = scrollerRef.getVisibility() != View.VISIBLE;
                scrollerRef.setVisibility(show ? View.VISIBLE : View.GONE);
                ((TextView) v).setText(show ? "HIDE" : "SHOW");
            }
        });
        return panel;
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
                debugView.setText(BUILD_NAME + " drawable " + nativeGetDrawableWidth() + "x"
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
    void debugSet(int k, int v) {
        if (k < 0) return;
        if (k == 11) {
            applySourceWidthPercent(v);
        } else if (k >= 8) {
            nativeSetStereoInt(k, v);
        } else {
            nativeSetStereo(k, v);
        }
    }

    void debugToggle(String name) {
        if (name == null) return;
        switch (name) {
            case "TINT": nativeSetStereoInt(7, tintOn ? 0 : 1); tintOn = !tintOn; break;
            case "ZBUF": nativeSetStereoInt(15, zbufOn ? 0 : 1); zbufOn = !zbufOn; break;
            case "WIRE": nativeSetStereoInt(16, wireOn ? 0 : 1); wireOn = !wireOn; break;
            case "PERF2x": nativeSetStereoInt(12, perfOn ? 0 : 1); perfOn = !perfOn; break;
            case "SWAP": nativeSetStereoInt(4, swapOn ? 0 : 1); swapOn = !swapOn; break;
            case "SAME": nativeSetStereoInt(6, sameOn ? 0 : 1); sameOn = !sameOn; break;
            case "NOCULL": nativeSetStereoInt(19, nocullOn ? 0 : 1); nocullOn = !nocullOn; break;
            case "FLAT":
                runOnUiThread(() -> {
                    flatOn = !flatOn;
                    nativeSetStereoInt(21, flatOn ? 1 : 0);   // zero parallax
                    Log.i(TAG, "FLAT zeroParallax=" + flatOn);
                });
                break;
            case "3D": runOnUiThread(() -> setStereoMode(!stereoOn)); break;
            case "TEST": runOnUiThread(this::showStaticTestImage); break;
            default: Log.w(TAG, "unknown toggle " + name);
        }
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
                    + " nocull=" + nocullOn);
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

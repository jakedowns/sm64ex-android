package com.vdavid003.sm64port;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.util.Log;

/**
 * Runtime control channel for headless testing.
 *
 * adb examples:
 *   adb shell am broadcast -a com.vdavid003.sm64port.CMD --es cmd dump
 *   adb shell am broadcast -a com.vdavid003.sm64port.CMD --es cmd set --ei k 0 --ei v 400
 *   adb shell am broadcast -a com.vdavid003.sm64port.CMD --es cmd toggle --es name TINT
 *   adb shell am broadcast -a com.vdavid003.sm64port.CMD --es cmd reinit
 *   adb shell am broadcast -a com.vdavid003.sm64port.CMD --es cmd quickdev --ei v 1
 *   adb shell am broadcast -a com.vdavid003.sm64port.CMD --es cmd layout --es action save
 *   adb shell am broadcast -a com.vdavid003.sm64port.CMD --es cmd layout --es action discard
 *   adb shell am broadcast -a com.vdavid003.sm64port.CMD --es cmd layout --es action save --es profile mine
 */
public class DebugReceiver extends BroadcastReceiver {
    public static final String ACTION = "com.vdavid003.sm64port.CMD";
    private static final String TAG = "SM64Cmd";

    @Override
    public void onReceive(Context context, Intent intent) {
        sm64portActivity a = sm64portActivity.instance;
        if (a == null) {
            Log.w(TAG, "CMD ignored (no activity)");
            return;
        }
        String cmd = intent.getStringExtra("cmd");
        if (cmd == null) cmd = "dump";
        switch (cmd) {
            case "set": {
                int k = intent.getIntExtra("k", -1);
                int v = intent.getIntExtra("v", 0);
                a.debugSet(k, v);
                Log.i(TAG, "CMD set k=" + k + " v=" + v);
                break;
            }
            case "fx": {
                int k = intent.getIntExtra("k", -1);
                int v = intent.getIntExtra("v", 0);
                a.debugFx(k, v);
                Log.i(TAG, "CMD fx k=" + k + " v=" + v);
                break;
            }
            case "profile": {
                /* QUALITY tier by name: low | med | high (or 0 | 1 | 2). */
                String name = intent.getStringExtra("name");
                a.debugProfile(name);
                Log.i(TAG, "CMD profile name=" + name);
                break;
            }
            case "shadow": {
                int k = intent.getIntExtra("k", -1);
                int v = intent.getIntExtra("v", 0);
                a.debugShadow(k, v);
                Log.i(TAG, "CMD shadow k=" + k + " v=" + v);
                break;
            }
            case "pad": {
                int sx = intent.getIntExtra("sx", 0);
                int sy = intent.getIntExtra("sy", 0);
                int buttons = intent.getIntExtra("buttons", 0);
                int ms = intent.getIntExtra("ms", 120);
                a.debugPad(sx, sy, buttons, ms);
                Log.i(TAG, "CMD pad sx=" + sx + " sy=" + sy + " btn=" + buttons + " ms=" + ms);
                break;
            }
            case "pad2": {
                int rx = intent.getIntExtra("rx", 0);
                int ry = intent.getIntExtra("ry", 0);
                int ms = intent.getIntExtra("ms", 240);
                a.debugPad2(rx, ry, ms);
                Log.i(TAG, "CMD pad2 rx=" + rx + " ry=" + ry + " ms=" + ms);
                break;
            }
            case "stereo": {
                int v = intent.getIntExtra("v", 1);
                a.debugSetStereo(v);
                Log.i(TAG, "CMD stereo v=" + v);
                break;
            }
            case "kill":
                a.debugKill();
                Log.i(TAG, "CMD kill");
                break;
            case "level": {
                int n = intent.getIntExtra("n", -1);
                a.debugLevel(n);
                Log.i(TAG, "CMD level n=" + n);
                break;
            }
            case "toggle": {
                String name = intent.getStringExtra("name");
                a.debugToggle(name);
                Log.i(TAG, "CMD toggle " + name);
                break;
            }
            case "fps": {
                int v = intent.getIntExtra("v", 0);
                a.debugSetFps(v);
                Log.i(TAG, "CMD fps " + v);
                break;
            }
            case "portal": {
                int v = intent.getIntExtra("v", 0);
                a.debugPortal(v);
                Log.i(TAG, "CMD portal " + v);
                break;
            }
            case "ptc": {
                String action = intent.getStringExtra("action");
                a.debugPtc(action);
                Log.i(TAG, "CMD ptc " + action);
                break;
            }
            case "uncapped": {
                int v = intent.getIntExtra("v", 0);
                a.debugUncapped(v);
                Log.i(TAG, "CMD uncapped " + v);
                break;
            }
            case "layout": {
                String action = intent.getStringExtra("action");
                String profile = intent.getStringExtra("profile");
                String to = intent.getStringExtra("to");
                a.debugLayout(action, profile, to);
                Log.i(TAG, "CMD layout action=" + action + " profile=" + profile
                        + " to=" + to);
                break;
            }
            case "quickdev": {
                int v = intent.getIntExtra("v", -1);
                int level = intent.getIntExtra("level", -1);
                int mute = intent.getIntExtra("mute", -1);
                a.debugQuickDev(v, level, mute);
                Log.i(TAG, "CMD quickdev v=" + v + " level=" + level + " mute=" + mute);
                break;
            }
            case "captions": {
                /* v: 1 = JAVA overlay, 0 = GAME FONT, omitted = toggle. */
                int v = intent.getIntExtra("v", -1);
                a.debugCaptions(v);
                Log.i(TAG, "CMD captions v=" + v);
                break;
            }
            case "runticks": {
                int n = intent.getIntExtra("n", 300);
                a.debugRunTicks(n);
                Log.i(TAG, "CMD runticks n=" + n);
                break;
            }
            case "cam2": {
                int m = intent.getIntExtra("mode", 5);
                a.debugSetCamMode2(m);
                Log.i(TAG, "CMD cam2 mode=" + m);
                break;
            }
            case "freecam": {
                int v = intent.getIntExtra("v", 1);
                a.debugSetFreecamMove(v);
                Log.i(TAG, "CMD freecam v=" + v);
                break;
            }
            case "audiorec": {
                int v = intent.getIntExtra("v", 1);
                a.debugSetAudioRec(v);
                Log.i(TAG, "CMD audiorec v=" + v);
                break;
            }
            case "hud": {
                int v = intent.getIntExtra("v", 1);
                a.debugHideHud(v);
                Log.i(TAG, "CMD hud v=" + v);
                break;
            }
            case "pause": {
                int v = intent.getIntExtra("v", 1);
                a.debugSetPause(v);
                Log.i(TAG, "CMD pause v=" + v);
                break;
            }
            case "step": {
                int n = intent.getIntExtra("n", 1);
                a.debugStep(n);
                Log.i(TAG, "CMD step n=" + n);
                break;
            }
            case "interp": {
                int v = intent.getIntExtra("v", 1);
                a.debugInterp(v);
                Log.i(TAG, "CMD interp v=" + v);
                break;
            }
            case "trace":
                a.debugTrace();
                Log.i(TAG, "CMD trace");
                break;
            case "hz": {
                int v = intent.getIntExtra("v", 60);
                a.debugDisplayHz(v);
                Log.i(TAG, "CMD hz " + v);
                break;
            }
            case "note": {
                String text = intent.getStringExtra("text");
                a.debugNote(text);
                Log.i(TAG, "CMD note " + text);
                break;
            }
            case "givegun":
                a.debugGiveGun();
                Log.i(TAG, "CMD givegun");
                break;
            case "busy": {
                int v = intent.getIntExtra("v", 1);
                a.debugSetBusy(v);
                Log.i(TAG, "CMD busy " + v);
                break;
            }
            case "logo":
                a.debugResetToLogo();
                Log.i(TAG, "CMD logo");
                break;
            case "reinit":
                a.debugReinit();
                Log.i(TAG, "CMD reinit");
                break;
            case "reset":
                a.debugReset();
                Log.i(TAG, "CMD reset");
                break;
            case "feedback": {
                String text = intent.getStringExtra("text");
                a.debugFeedback(text);
                Log.i(TAG, "CMD feedback text=" + text);
                break;
            }
            case "remote": {
                /* status | reload (config lives in <external files>/sm64remote.cfg) */
                String action = intent.getStringExtra("action");
                a.debugRemote(action);
                Log.i(TAG, "CMD remote action=" + action);
                break;
            }
            case "rinput": {
                /* test hook: the same one-tick press a relay "jump" produces */
                String input = intent.getStringExtra("input");
                a.debugRemoteInput(input);
                Log.i(TAG, "CMD rinput input=" + input);
                break;
            }
            case "dump":
            default:
                a.debugDump();
                Log.i(TAG, "CMD dump");
                break;
        }
    }
}

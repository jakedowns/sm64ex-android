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
 *   adb shell am broadcast -a com.vdavid003.sm64port.CMD --es cmd reset
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
            case "reinit":
                a.debugReinit();
                Log.i(TAG, "CMD reinit");
                break;
            case "reset":
                a.debugReset();
                Log.i(TAG, "CMD reset");
                break;
            case "dump":
            default:
                a.debugDump();
                Log.i(TAG, "CMD dump");
                break;
        }
    }
}

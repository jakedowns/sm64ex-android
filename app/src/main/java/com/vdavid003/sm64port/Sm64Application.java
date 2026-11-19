package com.vdavid003.sm64port;

import android.app.Application;
import android.util.Log;

/**
 * Starts debug log capture as early as possible so a failed Leia/SDL boot is
 * still recorded for a user bug report.
 */
public class Sm64Application extends Application {
    @Override
    public void onCreate() {
        super.onCreate();
        try {
            DebugLog.install(this);
        } catch (Throwable t) {
            Log.e(DebugLog.TAG, "debug log install failed", t);
        }
        try {
            Log.i(DebugLog.TAG, "APP_START " + DebugLog.oneLineSummary(this));
        } catch (Throwable t) {
            Log.e(DebugLog.TAG, "environment summary failed", t);
        }
    }
}

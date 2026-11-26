package com.vdavid003.sm64port;

import android.content.Context;
import android.content.SharedPreferences;
import android.util.Log;

import org.json.JSONObject;

import java.io.File;
import java.io.FileOutputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Iterator;
import java.util.List;

/**
 * Persistence for the on-screen control layout.
 *
 * One JSON document holds every named profile:
 *
 * <pre>
 * {
 *   "schema": "sm64-touch-layout/1",
 *   "active": "default",
 *   "reference": {"width": 2560, "height": 1600},
 *   "profiles": {
 *     "default": {
 *       "updated": 1730000000000,
 *       "elements": {
 *         "a": {"x": 0.9, "y": 0.7, "s": 1.0},
 *         ...
 *       }
 *     }
 *   }
 * }
 * </pre>
 *
 * Positions are normalised (0..1 of panel width/height) and sizes are
 * multipliers of the per-control base radius, so a profile is valid on a
 * different panel resolution.
 *
 * The same document is mirrored to the app's external files directory
 * ({@code .../files/layouts/layouts.json}) together with a shareable copy of
 * the active profile ({@code active.json} and {@code share-<profile>.json}).
 */
final class TouchLayoutStore {
    static final String DEFAULT_PROFILE = "default";
    static final String SCHEMA = "sm64-touch-layout/1";

    private static final String TAG = "SM64Layout";
    private static final String PREFS = "touch_layout";
    private static final String KEY_STORE = "store";
    private static final int REF_W = 2560;
    private static final int REF_H = 1600;

    private final Context ctx;
    private final JSONObject store;

    TouchLayoutStore(Context context, String[] ids, float[] nx, float[] ny, float[] ns) {
        ctx = context.getApplicationContext();
        JSONObject parsed = null;
        try {
            SharedPreferences sp = ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
            String raw = sp.getString(KEY_STORE, null);
            if (raw != null) parsed = new JSONObject(raw);
        } catch (Throwable t) {
            Log.w(TAG, "could not read layout store", t);
        }
        if (parsed == null || parsed.optJSONObject("profiles") == null) {
            parsed = newStore(ids, nx, ny, ns);
        }
        store = parsed;
        JSONObject profiles = store.optJSONObject("profiles");
        if (profiles == null) {
            profiles = new JSONObject();
            putQuiet(store, "profiles", profiles);
        }
        if (profiles.optJSONObject(DEFAULT_PROFILE) == null) {
            putQuiet(profiles, DEFAULT_PROFILE, profileJson(ids, nx, ny, ns));
        }
        String active = store.optString("active", DEFAULT_PROFILE);
        if (profiles.optJSONObject(active) == null) {
            active = DEFAULT_PROFILE;
        }
        putQuiet(store, "active", active);
        persistStore();
        Log.i(TAG, "layout store ready: active=" + active
                + " profiles=" + profiles().size() + " ext=" + externalDir());
    }

    private static JSONObject newStore(String[] ids, float[] nx, float[] ny, float[] ns) {
        JSONObject s = new JSONObject();
        putQuiet(s, "schema", SCHEMA);
        putQuiet(s, "active", DEFAULT_PROFILE);
        JSONObject ref = new JSONObject();
        putQuiet(ref, "width", REF_W);
        putQuiet(ref, "height", REF_H);
        putQuiet(s, "reference", ref);
        JSONObject profiles = new JSONObject();
        putQuiet(profiles, DEFAULT_PROFILE, profileJson(ids, nx, ny, ns));
        putQuiet(s, "profiles", profiles);
        return s;
    }

    private static void putQuiet(JSONObject o, String k, Object v) {
        try {
            o.put(k, v);
        } catch (Throwable ignored) {
        }
    }

    private static double round5(float v) {
        return Math.round(v * 100000.0) / 100000.0;
    }

    private static JSONObject elementsJson(String[] ids, float[] nx, float[] ny, float[] ns) {
        JSONObject el = new JSONObject();
        for (int i = 0; i < ids.length && i < nx.length; i++) {
            JSONObject e = new JSONObject();
            putQuiet(e, "x", round5(nx[i]));
            putQuiet(e, "y", round5(ny[i]));
            putQuiet(e, "s", round5(ns[i]));
            putQuiet(el, ids[i], e);
        }
        return el;
    }

    private static JSONObject profileJson(String[] ids, float[] nx, float[] ny, float[] ns) {
        JSONObject p = new JSONObject();
        putQuiet(p, "updated", System.currentTimeMillis());
        putQuiet(p, "elements", elementsJson(ids, nx, ny, ns));
        return p;
    }

    String active() {
        return store.optString("active", DEFAULT_PROFILE);
    }

    private JSONObject profilesJson() {
        JSONObject p = store.optJSONObject("profiles");
        if (p == null) {
            p = new JSONObject();
            putQuiet(store, "profiles", p);
        }
        return p;
    }

    /** Profile names, default first then alphabetical. */
    List<String> profiles() {
        List<String> names = new ArrayList<>();
        Iterator<String> it = profilesJson().keys();
        while (it.hasNext()) {
            String n = it.next();
            if (!n.startsWith("_")) names.add(n);
        }
        Collections.sort(names);
        names.remove(DEFAULT_PROFILE);
        names.add(0, DEFAULT_PROFILE);
        return names;
    }

    private boolean loadElements(String name, String[] ids, float[] nx, float[] ny, float[] ns) {
        JSONObject prof = profilesJson().optJSONObject(name);
        if (prof == null) return false;
        JSONObject el = prof.optJSONObject("elements");
        if (el == null) return false;
        for (int i = 0; i < ids.length && i < nx.length; i++) {
            JSONObject e = el.optJSONObject(ids[i]);
            if (e == null) continue;
            nx[i] = (float) e.optDouble("x", nx[i]);
            ny[i] = (float) e.optDouble("y", ny[i]);
            ns[i] = (float) e.optDouble("s", ns[i]);
        }
        return true;
    }

    /** Load a profile into the model without changing the active name. */
    boolean loadProfile(String name, String[] ids, float[] nx, float[] ny, float[] ns) {
        return loadElements(name, ids, nx, ny, ns);
    }

    /** Load a profile and make it active (also refreshes the external copies). */
    boolean applyProfile(String name, String[] ids, float[] nx, float[] ny, float[] ns) {
        if (!loadElements(name, ids, nx, ny, ns)) return false;
        putQuiet(store, "active", name);
        persistStore();
        writeMirrors(name, elementsJson(ids, nx, ny, ns));
        Log.i(TAG, "layout profile -> " + name);
        return true;
    }

    void saveActive(String[] ids, float[] nx, float[] ny, float[] ns) {
        String name = active();
        JSONObject prof = profileJson(ids, nx, ny, ns);
        putQuiet(profilesJson(), name, prof);
        persistStore();
        writeMirrors(name, prof.optJSONObject("elements"));
    }

    /** Save the model as a named profile and activate it. */
    boolean saveAs(String rawName, String[] ids, float[] nx, float[] ny, float[] ns) {
        String name = sanitizeName(rawName);
        if (name.isEmpty()) return false;
        putQuiet(profilesJson(), name, profileJson(ids, nx, ny, ns));
        putQuiet(store, "active", name);
        persistStore();
        writeMirrors(name, elementsJson(ids, nx, ny, ns));
        Log.i(TAG, "layout saved as " + name);
        return true;
    }

    boolean rename(String oldName, String rawNew) {
        String next = sanitizeName(rawNew);
        if (next.isEmpty() || oldName == null || next.equals(oldName)) return false;
        if (DEFAULT_PROFILE.equals(oldName)) return false;
        JSONObject profiles = profilesJson();
        JSONObject prof = profiles.optJSONObject(oldName);
        if (prof == null || profiles.optJSONObject(next) != null) return false;
        putQuiet(profiles, next, prof);
        profiles.remove(oldName);
        if (oldName.equals(active())) putQuiet(store, "active", next);
        persistStore();
        JSONObject act = profilesJson().optJSONObject(active());
        writeMirrors(active(), act == null ? null : act.optJSONObject("elements"));
        Log.i(TAG, "layout renamed " + oldName + " -> " + next);
        return true;
    }

    boolean delete(String name) {
        if (name == null || DEFAULT_PROFILE.equals(name)) return false;
        JSONObject profiles = profilesJson();
        if (profiles.optJSONObject(name) == null) return false;
        profiles.remove(name);
        if (name.equals(active())) putQuiet(store, "active", DEFAULT_PROFILE);
        persistStore();
        JSONObject activeProf = profilesJson().optJSONObject(active());
        writeMirrors(active(), activeProf == null ? null
                : activeProf.optJSONObject("elements"));
        Log.i(TAG, "layout deleted " + name);
        return true;
    }

    void exportActive() {
        String name = active();
        JSONObject prof = profilesJson().optJSONObject(name);
        if (prof == null) return;
        writeMirrors(name, prof.optJSONObject("elements"));
        Log.i(TAG, "layout exported " + name + " -> " + externalDir());
    }

    String externalDir() {
        File dir = externalDirFile();
        return dir == null ? "unavailable" : dir.getAbsolutePath();
    }

    /** Full JSON document, for logs / debugging. */
    String storeJson() {
        return store.toString();
    }

    private static String sanitizeName(String raw) {
        if (raw == null) return "";
        String s = raw.trim();
        if (s.length() > 32) s = s.substring(0, 32);
        s = s.replaceAll("[^A-Za-z0-9 ._-]", "_").trim();
        if (s.startsWith("_")) s = s.substring(1);
        return s;
    }

    private static String fileSafe(String name) {
        String s = name.replaceAll("[^A-Za-z0-9._-]", "_");
        return s.isEmpty() ? "profile" : s;
    }

    private File externalDirFile() {
        File base = null;
        try {
            base = ctx.getExternalFilesDir("layouts");
        } catch (Throwable t) {
            Log.w(TAG, "external files dir failed", t);
        }
        if (base == null) base = new File(ctx.getFilesDir(), "layouts");
        if (!base.exists() && !base.mkdirs()) {
            Log.w(TAG, "could not create " + base);
            return null;
        }
        return base;
    }

    private void persistStore() {
        try {
            ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
                    .edit().putString(KEY_STORE, store.toString()).apply();
        } catch (Throwable t) {
            Log.w(TAG, "could not persist layout store", t);
        }
        File dir = externalDirFile();
        if (dir != null) {
            writeFile(new File(dir, "layouts.json"), pretty(store));
        }
    }

    private static String pretty(JSONObject o) {
        try {
            return o.toString(2);
        } catch (Throwable t) {
            return o.toString();
        }
    }

    private void writeMirrors(String name, JSONObject elements) {
        File dir = externalDirFile();
        if (dir == null || elements == null) return;
        JSONObject prof = new JSONObject();
        putQuiet(prof, "schema", SCHEMA);
        putQuiet(prof, "profile", name);
        JSONObject ref = new JSONObject();
        putQuiet(ref, "width", REF_W);
        putQuiet(ref, "height", REF_H);
        putQuiet(prof, "reference", ref);
        putQuiet(prof, "elements", elements);
        String pretty = pretty(prof);
        writeFile(new File(dir, "active.json"), pretty);
        writeFile(new File(dir, "share-" + fileSafe(name) + ".json"), pretty);
    }

    private static void writeFile(File f, String text) {
        FileOutputStream out = null;
        try {
            out = new FileOutputStream(f);
            out.write(text.getBytes(StandardCharsets.UTF_8));
            out.flush();
        } catch (Throwable t) {
            Log.w(TAG, "write failed: " + f, t);
        } finally {
            if (out != null) {
                try {
                    out.close();
                } catch (Throwable ignored) {
                }
            }
        }
    }
}

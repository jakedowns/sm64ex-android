#!/usr/bin/env python3
"""Host-side check of the shipped cam-stick curve code.

The C functions are NOT re-implemented here: this script extracts the exact
text between the marker comments in app/jni/src/src/game/customcam.c, compiles
it with a tiny main, and runs it.  If someone edits the curve in customcam.c,
this test follows automatically (and fails if it stops being monotone).

Usage: scripts/camstick-curve-check.py [path/to/customcam.c]
"""
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
    ROOT, "app/jni/src/src/game/customcam.c")

src = open(SRC).read()
start = src.index("static int   s_stick_curve_on")
end = src.index("void custom_camera_stick_fx_set")
# Keep the static state + the functions, drop the log calls' dependencies.
body = src[start:end]

harness = r"""
#include <stdio.h>
#include <math.h>
#define CAM_STICK_POINTS 5
#define CAM_STICK_KEYS 290
""" + body + r"""

int main(void) {
    int i, bad = 0;
    /* 1. Monotonicity across a dense sweep for several clamp-legal shapes. */
    float shapes[][6] = {
        { 0.25f, 0.25f, 0.50f, 0.50f, 0.75f, 0.75f },   /* linear        */
        { 0.10f, 0.45f, 0.35f, 0.70f, 0.70f, 0.90f },   /* lifted low end*/
        { 0.40f, 0.10f, 0.60f, 0.30f, 0.85f, 0.55f },   /* slow start    */
        { 0.10f, 0.01f, 0.50f, 0.50f, 0.90f, 0.99f },   /* S-curve       */
    };
    for (int s = 0; s < 4; s++) {
        for (i = 1; i <= 3; i++) {
            s_stick_px[i] = shapes[s][(i - 1) * 2 + 0];
            s_stick_py[i] = shapes[s][(i - 1) * 2 + 1];
        }
        stick_curve_sanitise();
        float prev = -1.0f;
        for (i = 0; i <= 1000; i++) {
            float u = i / 1000.0f;
            float v = stick_curve_eval(u);
            if (v < prev - 1e-6f) { bad = 1; }
            if (v < -1e-6f || v > 1.0f + 1e-6f) { bad = 1; }
            prev = v;
        }
        printf("shape%d monotonic=%s\n", s, bad ? "NO" : "yes");
    }

    /* 2. Stick -> effective rate pairs for the two shapes we measure on the
     *    tablet (linear vs low-end lift), deadzone 10%. */
    s_stick_deadzone = 0.10f;
    float lin[3] = { 0.25f, 0.50f, 0.75f };
    for (int pass = 0; pass < 2; pass++) {
        if (pass == 0) {
            for (i = 1; i <= 3; i++) { s_stick_px[i] = lin[i - 1]; s_stick_py[i] = lin[i - 1]; }
        } else {
            s_stick_px[1] = 0.12f; s_stick_py[1] = 0.90f;
            s_stick_px[2] = 0.50f; s_stick_py[2] = 0.95f;
            s_stick_px[3] = 0.75f; s_stick_py[3] = 0.98f;
        }
        stick_curve_sanitise();
        printf("%s", pass == 0 ? "linear" : "lifted");
        for (i = 0; i < 6; i++) {
            int rx = 20 + i * 18;            /* 20..110 of 127 */
            int sy = 0;
            int ox = rx;
            custom_camera_stick_shape2(&ox, &sy);
            float rate = (float) ox * 26.0f;  /* mode-3 yaw deg/frame*100 step */
            printf("  in=%d out=%d yawstep=%.0f", rx, ox, rate);
        }
        printf("\n");
    }
    return bad;
}
"""

with tempfile.TemporaryDirectory() as d:
    c = os.path.join(d, "t.c")
    exe = os.path.join(d, "t")
    open(c, "w").write(harness)
    subprocess.run(["cc", "-O2", "-o", exe, c, "-lm"], check=True)
    subprocess.run([exe], check=True)

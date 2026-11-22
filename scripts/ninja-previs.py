#!/usr/bin/env python3
"""Previs for the reworked ninja grapple + goomba beats.

Simplified 2D cinematic planning: draws the world from a virtual camera with a
perspective projection, so timing, framing, portal reveals, the web-shooter
rope and the slow-mo gag can be tuned before touching the game code.

Outputs:
  /tmp/opencode/previs/grapple/f_XXX.png   (30 fps, 4.8 s)
  /tmp/opencode/previs/goomba/f_XXX.png    (30 fps, 8.0 s)
Render with ffmpeg afterwards (see bottom of the file).
"""
import math, os
from PIL import Image, ImageDraw, ImageFont

W, H = 960, 540
FPS = 30
FONT = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"
try:
    F = ImageFont.truetype(FONT, 34)
    FS = ImageFont.truetype(FONT, 20)
except Exception:
    F = FS = ImageFont.load_default()

# ---------------------------------------------------------------- projection
class Cam:
    """Orthographic side-view camera: center + pixels-per-world-unit."""
    def __init__(self, center, scale):
        self.cx, self.cy = center
        self.scale = scale

    def proj(self, p):
        return (W * 0.5 + (p[0] - self.cx) * self.scale,
                H * 0.5 - (p[1] - self.cy) * self.scale,
                self.scale, 0.0)


def quad(d, cam, x0, x1, y0, y1, fill, outline=None):
    pts = [cam.proj((x0, y0)), cam.proj((x1, y0)), cam.proj((x1, y1)), cam.proj((x0, y1))]
    if any(p is None for p in pts):
        return
    d.polygon([(p[0], p[1]) for p in pts], fill=fill, outline=outline)


def disc(d, cam, x, y, r, fill, outline=None):
    p = cam.proj((x, y))
    if p is None:
        return
    sx, sy, sc, dep = p
    rr = max(2, r * sc)
    d.ellipse([sx - rr, sy - rr, sx + rr, sy + rr], fill=fill, outline=outline)


def line(d, cam, a, b, fill, width=3):
    pa, pb = cam.proj(a), cam.proj(b)
    if pa is None or pb is None:
        return
    d.line([(pa[0], pa[1]), (pb[0], pb[1])], fill=fill, width=max(1, int(width)))


def lerp(a, b, t):
    return a + (b - a) * t


def clamp01(t):
    return 0.0 if t < 0 else (1.0 if t > 1 else t)


def ease(t):
    t = clamp01(t)
    return t * t * (3 - 2 * t)


def ease_in_quad(t):
    t = clamp01(t)
    return t * t


def ease_out_cubic(t):
    t = clamp01(t)
    return 1 - (1 - t) ** 3


def ease_in_out_cubic(t):
    t = clamp01(t)
    return 4 * t ** 3 if t < 0.5 else 1 - (-2 * t + 2) ** 3 / 2


def ease_out_back(t, s_=2.0):
    t = clamp01(t)
    return 1 + (s_ + 1) * (t - 1) ** 3 + s_ * (t - 1) ** 2


def ease_out_expo(t):
    t = clamp01(t)
    return 1.0 if t >= 1 else 1 - 2 ** (-10 * t)


def ease_in_out_sine(t):
    return -(math.cos(math.pi * clamp01(t)) - 1) / 2


def tween(t, t0, t1, fn=ease_in_out_cubic):
    return fn(clamp01((t - t0) / max(1e-6, (t1 - t0))))


def shake_at(t, start, dur=0.35, amp=16.0):
    if t < start or t > start + dur:
        return 0.0, 0.0
    u = (t - start) / dur
    a = amp * (1 - u) ** 2
    return math.sin(u * 60.0) * a, math.cos(u * 47.0) * a * 0.7


def fov_cam(cam, fov):
    """Widen/tighten the stage view with the planned FOV (50 deg baseline)."""
    base = math.tan(math.radians(25.0))
    scale = cam.scale * base / math.tan(math.radians(max(12.0, fov) * 0.5))
    return Cam((cam.cx, cam.cy), scale)


def cam_inset(d, st, span):
    """Top-right camera plan: position, look direction and FOV cone."""
    x0, y0, w, h = W - 286, 18, 268, 150
    d.rectangle([x0, y0, x0 + w, y0 + h], fill=(10, 16, 22), outline=(90, 110, 130))
    cx, cy = x0 + 52, y0 + h - 26
    d.ellipse([cx - 7, cy - 7, cx + 7, cy + 7], fill=(255, 220, 70))
    fov = st.get("fov", 50)
    subject = st.get("subject", (cx + 150, cy - 60))
    # scale the subject into the inset
    sx = x0 + 150
    sy = y0 + 52
    d.ellipse([sx - 8, sy - 8, sx + 8, sy + 8], fill=(120, 200, 255))
    ang = math.atan2(sy - cy, sx - cx)
    half = math.radians(fov) * 0.5
    for sgn in (-1, 1):
        a = ang + sgn * half
        d.line([cx, cy, cx + math.cos(a) * 200, cy + math.sin(a) * 200],
               fill=(90, 150, 200), width=1)
    d.text((x0 + 8, y0 + 6), f"CAM  FOV {fov:.0f}deg", font=FS, fill=(255, 235, 120))
    d.text((x0 + 8, y0 + 28), st.get("cam_note", ""), font=FS, fill=(180, 220, 240))


def caption(d, text, color=(255, 220, 70)):
    if not text:
        return
    tw = d.textlength(text, font=F)
    d.text(((W - tw) / 2, 26), text, font=F, fill=color)


# ---------------------------------------------------------------- grapple
G_ANCHOR = (-100.0, 2780.0)
G_START = (-450.0, 330.0)
G_LAND = (300.0, 320.0)


def grapple_state(t):
    """Returns Mario, rope state, caption, camera, fx."""
    mario = G_START
    rope = None            # (tipx, tipy, alpha, width)
    cap = "CEILING GRAPPLE"
    cam = Cam((0, 1400), 0.17)
    zoom = 0.0
    whip = 0.0

    shake = (0.0, 0.0)
    squash = 0.0
    flash = 0.0
    fov = 48
    note = "WIDE / LOW"
    if t < 0.6:
        pass
    elif t < 0.9:
        # whip pan up to the anchor
        u = tween(t, 0.6, 0.9, ease_in_out_cubic)
        cam = Cam((lerp(0, -150, u), lerp(1400, 1900, u)), 0.17)
        whip = u
        cap = "CEILING GRAPPLE"
        fov = 52
        note = "WHIP PAN UP"
    elif t < 1.25:
        # web shooter: rope tip travels to the ceiling
        u = tween(t, 0.9, 1.15, ease_out_expo)
        tip = (lerp(G_START[0], G_ANCHOR[0], u), lerp(G_START[1] + 80, G_ANCHOR[1], u))
        rope = (tip[0], tip[1], 255, 4)
        cam = Cam((-200, 1700), 0.18)
        cap = "SWING" if u > 0.15 else "CEILING GRAPPLE"
        fov = 45
        note = "TILT UP / SHOOT"
    elif t < 2.40:
        # swing half of the pendulum (unchanged)
        u = tween(t, 1.25, 2.40, ease_in_out_sine)
        ang = math.radians(-6 + 10 * u)
        R = 2450 - 130 * u
        mario = (G_ANCHOR[0] + R * math.sin(ang), G_ANCHOR[1] - R * math.cos(ang))
        rope = (G_ANCHOR[0], G_ANCHOR[1], 255, 5)
        cam = Cam((mario[0] + 150, mario[1] + 500), 0.20)
        cap = "SWING"
        fov = 55
        note = "WIDE SIDE TRACK"
    elif t < 3.75:
        # retract: 2x longer than before
        u = tween(t, 2.40, 3.75, ease_in_out_cubic)
        ang = math.radians(4 + 12 * u)
        R = 2320 - 250 * u
        mario = (G_ANCHOR[0] + R * math.sin(ang), G_ANCHOR[1] - R * math.cos(ang))
        rope = (G_ANCHOR[0], G_ANCHOR[1], 255, 5)
        cam = Cam((mario[0] + 160, mario[1] + 420), 0.21)
        cap = "RETRACT"
        fov = 50
        note = "SLOW REEL / HOLD"
    elif t < 4.15:
        # release: the hook lets go of the ceiling and whips back into Mario's
        # hand (2x the old fade duration), while he starts the arc.
        u = tween(t, 3.75, 4.15, ease_out_cubic)
        ang = math.radians(16)
        R = 2070
        mario = (G_ANCHOR[0] + R * math.sin(ang), G_ANCHOR[1] - R * math.cos(ang))
        hand = (mario[0], mario[1] + 80)
        tip = (lerp(G_ANCHOR[0], hand[0], u), lerp(G_ANCHOR[1], hand[1], u))
        rope = (tip[0], tip[1], 255, 5 - 3.0 * u)
        cam = Cam((mario[0] + 200, mario[1] + 200), 0.26)
        cap = "RELEASE"
        fov = 30
        note = "CRASH ZOOM / SNAP-BACK"
        zoom = 1.0 - u
    else:
        # parabolic release flight to N4
        u = clamp01((t - 4.15) / 1.55)
        sx, sy = mario
        x0 = G_ANCHOR[0] + 2070 * math.sin(math.radians(16))
        y0 = G_ANCHOR[1] - 2070 * math.cos(math.radians(16))
        vx = 260.0
        vy = -520.0 - 0.5 * 900.0 * (1.55)
        # cubic-free simple ballistic: solve to land at G_LAND
        T = 1.55
        g = 900.0
        vx = (G_LAND[0] - x0) / T
        vy = (G_LAND[1] - y0 - 0.5 * g * T * T) / T
        tt = u * T
        mario = (x0 + vx * tt, y0 + vy * tt + 0.5 * g * tt * tt)
        cam = Cam((mario[0] + 150, mario[1] + 250), 0.22)
        cap = "RELEASE"
        fov = 45
        note = "FOLLOW ARC"
        if u > 0.92:
            cap = "LAND"
            fov = 52
            note = "SQUASH LANDING"
            squash = 1.0 - clamp01((u - 0.92) / 0.08)
            shake = shake_at(t, 5.60, 0.35, 18.0)

    # fixed wide stage camera (previs readability) with a push-in on release
    cam = Cam((0, 1400), 0.16 + 0.07 * zoom)
    cam = Cam((cam.cx + shake[0] * 0.02, cam.cy + shake[1] * 0.02), cam.scale)
    return {
        "mario": mario,
        "rope": rope,
        "cam": cam,
        "cap": cap,
        "anchor": G_ANCHOR,
        "zoom": zoom,
        "squash": squash,
        "flash": flash,
        "fov": fov,
        "cam_note": note,
    }


def draw_grapple(d, st):
    cam = fov_cam(st["cam"], st.get("fov", 50))
    # sky / room
    d.rectangle([0, 0, W, H], fill=(18, 26, 34))
    # back wall gradient bands
    for i in range(6):
        y0 = 40 + i * 80
        d.rectangle([0, y0, W, y0 + 80], fill=(30 + i * 4, 52 + i * 5, 44 + i * 3))
    # platforms (x-y)
    quad(d, cam, -950, -360, 0, 330, (150, 105, 70))     # N2
    quad(d, cam, -150, -90, 0, 950, (150, 80, 170))      # N3 pillar
    quad(d, cam, -200, 1000, 0, 200, (70, 160, 170))     # N4
    # ceiling line
    line(d, cam, (-1400, 2800), (1200, 2800), (90, 100, 120), 3)
    # anchor marker
    disc(d, cam, st["anchor"][0], st["anchor"][1], 26, (255, 255, 255))
    # rope
    if st["rope"]:
        tx, ty, alpha, wd = st["rope"]
        m = st["mario"]
        hand = (m[0], m[1] + 80)
        col = (235, 240, 250)
        line(d, cam, hand, (tx, ty), col, wd)
        disc(d, cam, tx, ty, 18, (255, 255, 255))
    # Mario
    mx, my = st["mario"]
    sq = st.get("squash", 0.0)
    disc(d, cam, mx, my + 90 * (1 - 0.35 * sq), 55 * (1 + 0.35 * sq), (200, 40, 30))
    disc(d, cam, mx, my + 40 * (1 - 0.35 * sq), 45 * (1 + 0.35 * sq), (40, 70, 200))
    if st.get("flash", 0.0) > 0.001:
        d.rectangle([0, 0, W, H], fill=(255, 255, 255))
    # caption + zoom vignette
    caption(d, st["cap"])
    cam_inset(d, st, 0)
    if st["zoom"] > 0.01:
        d.rectangle([0, 0, W, H], outline=(255, 220, 70), width=3)


# ---------------------------------------------------------------- goomba
G_MARIO_Z = -3100.0
G_GOOMBA_Z0 = -2750.0
G_FLOOR_Y = 1500.0
G_CEIL_Y = 2800.0
G_CEIL_PORTAL = (-2950.0, 2800.0)
G_FLOOR_PORTAL = (-3000.0, 1500.0)


def goomba_state(t):
    """One goomba gets stuck in a portal loop, then the exit is shifted and it
    is fired into the hard ground beside the floor portal."""
    mario = (G_MARIO_Z, G_FLOOR_Y)
    goomba = (G_GOOMBA_Z0, G_FLOOR_Y)
    gvis = 1.0
    splat = 0.0
    portals = {"ceil": 0.0, "floor": 0.0}
    proj = None
    cap = "GOOMBA GRIEFING"
    slow = False
    ceil_z = G_CEIL_PORTAL[0]
    cam = Cam((-2900, 2050), 0.26)

    shake = (0.0, 0.0)
    flash = 0.0
    fov = 50
    note = "WIDE / LOW"
    # approach: no portals in sight, slow-in/ease-out waddle
    if t < 1.6:
        u = tween(t, 0.0, 1.6, ease_in_out_cubic)
        goomba = (lerp(G_GOOMBA_Z0, -2920.0, u), G_FLOOR_Y)
    # blue portal (directly above the orange one)
    if t >= 1.55:
        if t < 1.80:
            u = tween(t, 1.55, 1.80, ease_out_expo)
            proj = (G_MARIO_Z, lerp(G_FLOOR_Y + 130, G_CEIL_Y, u), (70, 150, 255))
        else:
            portals["ceil"] = ease_out_back((t - 1.80) / 0.20, 2.4)
            flash = max(0.0, 1.0 - (t - 1.80) / 0.06)
        cap = "PORTAL UP"
        fov = 45
        note = "TILT UP"
    # orange floor portal between Mario and the goomba
    if t >= 1.90:
        if t < 2.12:
            u = tween(t, 1.90, 2.12, ease_out_expo)
            proj = (G_MARIO_Z, lerp(G_FLOOR_Y + 130, G_FLOOR_PORTAL[1], u), (255, 150, 40))
        else:
            portals["floor"] = ease_out_back((t - 2.12) / 0.20, 2.4)
            flash = max(0.0, 1.0 - (t - 2.12) / 0.06)
        cap = "PORTAL DOWN"
        fov = 45
        note = "TILT DOWN"

    # portal loop: eight accelerating passes. Build the schedule from real
    # gravity so each pass is shorter than the last.
    loop_t0 = 2.20
    loop_g = 7000.0
    loop_vel = 0.0
    loop_terminal = 3000.0
    loop_passes = []
    for _i in range(8):
        _v = loop_vel
        _dur = (-_v + math.sqrt(_v * _v + 2 * loop_g * (G_CEIL_Y - G_FLOOR_Y))) / loop_g
        if _dur < 0.16:
            _dur = 0.16
        loop_passes.append((loop_t0, _dur, _v))
        loop_t0 += _dur + 0.05   # portal transit
        loop_vel = min(loop_vel + 700.0, loop_terminal)
    loop_end = loop_t0
    if t >= 2.20 and t < loop_end:
        cap = "LOOP"
        fov = 55
        note = "WIDE LOOP / ACCELERATING"
        for i, (st, du, v0) in enumerate(loop_passes):
            if t < st + du:
                dt = t - st
                gy = G_CEIL_Y - (v0 * dt + 0.5 * loop_g * dt * dt)
                if gy < G_FLOOR_Y:
                    gy = G_FLOOR_Y
                goomba = (G_CEIL_PORTAL[0], gy)
                cap = f"LOOP {i + 1}/8"
                break
        else:
            goomba = (G_CEIL_PORTAL[0], G_FLOOR_Y)
            gvis = 0.0

    # BAM: shift the blue portal one width to the side
    if t >= loop_end + 0.02:
        ceil_z = lerp(G_CEIL_PORTAL[0], G_CEIL_PORTAL[0] + 140.0,
                      ease_out_back((t - (loop_end + 0.02)) / 0.15, 3.0))
        portals["ceil"] = 1.0
        cap = "SHIFT!"
        fov = 40
        note = "WHIP + SNAP"
        flash = max(0.0, 1.0 - (t - (loop_end + 0.02)) / 0.08)
        shake = shake_at(t, loop_end + 0.02, 0.25, 14.0)
        gvis = 0.0
    # next pass: out of the shifted portal, hard ground beside the floor portal
    if t >= loop_end + 0.24:
        u = clamp01((t - (loop_end + 0.24)) / 0.34)
        gy = lerp(G_CEIL_Y, G_FLOOR_Y, u * u)
        goomba = (ceil_z, gy)
        gvis = 1.0
        cap = "FALL"
        fov = 50
        note = "TILT / TRACK"
        cam = Cam((-2900, lerp(2050, 2250, u)), 0.30)
    if t >= loop_end + 0.58:
        u = clamp01((t - (loop_end + 0.58)) / 0.70)
        goomba = (ceil_z, G_FLOOR_Y)
        # hard impact: 2-frame hold, big squash, shake, flash
        if u < 0.04:
            flash = 1.0
        splat = math.sin(min(1.0, u * 3.0) * math.pi) * (1.0 - u)
        gvis = 1.0 - clamp01((u - 0.45) / 0.55)
        cap = "SPLAT" if u < 0.5 else "YAY MARIO"
        fov = 30
        note = "CRASH ZOOM + SHAKE"
        cam = Cam((-2900, 1750), 0.38)
        shake = shake_at(t, loop_end + 0.58, 0.4, 26.0)
        slow = u < 0.25

    cam = Cam((cam.cx + shake[0] * 0.02, cam.cy + shake[1] * 0.02), cam.scale)
    return {
        "mario": mario, "goomba": goomba, "gvis": gvis, "splat": splat,
        "portals": portals, "proj": proj, "cam": cam, "cap": cap,
        "slow": slow, "ceil_z": ceil_z, "flash": flash, "shake": shake,
        "fov": fov, "cam_note": note,
    }


def draw_goomba(d, st):
    cam = fov_cam(st["cam"], st.get("fov", 50))
    d.rectangle([0, 0, W, H], fill=(18, 26, 34))
    for i in range(6):
        y0 = 40 + i * 80
        d.rectangle([0, y0, W, y0 + 80], fill=(26 + i * 4, 50 + i * 5, 43 + i * 3))
    # widened N6 slab and ceiling
    quad(d, cam, -3450, -2350, 0, G_FLOOR_Y, (120, 120, 150))
    quad(d, cam, -3450, -2350, G_FLOOR_Y - 12, G_FLOOR_Y, (160, 160, 190))
    line(d, cam, (-3500, G_CEIL_Y), (-2300, G_CEIL_Y), (90, 100, 120), 3)
    # portals
    for which, col in (("ceil", (70, 140, 255)), ("floor", (255, 140, 40))):
        s2 = st["portals"][which]
        if s2 > 0.01:
            px = st.get("ceil_z", G_CEIL_PORTAL[0]) if which == "ceil" else G_FLOOR_PORTAL[0]
            py = G_CEIL_PORTAL[1] if which == "ceil" else G_FLOOR_PORTAL[1]
            disc(d, cam, px, py, 130 * s2, col)
    if st["proj"]:
        px, py, col = st["proj"]
        disc(d, cam, px, py, 30, col)
        disc(d, cam, px, py, 55, col, outline=None)
    mz, my = st["mario"]
    disc(d, cam, mz, my + 120, 80, (200, 40, 30))
    disc(d, cam, mz, my + 45, 65, (40, 70, 200))
    if st["gvis"] > 0.01:
        gz, gy = st["goomba"]
        rr = 85 * (1.0 + 0.8 * st["splat"])
        disc(d, cam, gz, gy + 60, rr, (150, 85, 30))
        disc(d, cam, gz - 26, gy + 74, 13, (255, 255, 255))
        disc(d, cam, gz + 26, gy + 74, 13, (255, 255, 255))
    caption(d, st["cap"])
    cam_inset(d, st, 0)
    if st.get("flash", 0.0) > 0.001:
        d.rectangle([0, 0, W, H], fill=(255, 255, 255))
    if st["slow"]:
        d.text((20, 20), "SLOW-MO", font=FS, fill=(120, 220, 255))


# ---------------------------------------------------------------- render
def render_sequence(name, duration, fn, draw):
    out = f"/tmp/opencode/previs/{name}"
    os.makedirs(out, exist_ok=True)
    n = int(duration * FPS)
    for i in range(n):
        rt = i / FPS
        st = fn(rt)
        im = Image.new("RGB", (W, H), (0, 0, 0))
        d = ImageDraw.Draw(im)
        draw(d, st)
        im.save(f"{out}/f_{i:04d}.png")
    print(name, n, "frames ->", out)


def main():
    render_sequence("grapple", 6.0, lambda rt: grapple_state(rt), draw_grapple)
    render_sequence("goomba", 7.4, lambda rt: goomba_state(rt), draw_goomba)


if __name__ == "__main__":
    main()

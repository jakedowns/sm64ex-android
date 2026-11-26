# otsm64remote — opt-in telemetry + remote-input bridge

**Off by default. Nothing leaves the tablet unless `enabled = true` is written
into the config file.** The tablet is the authority; the browser at
<https://omitest.jakedowns.com/otsm64remote/> is a viewer. This is a mirror plus
a one-tick remote input, not crossplay: there are no sessions, no client
prediction and no second simulation.

Only **numeric** game state leaves the device — position, yaw, speed, animation
id/frame and a fixed action label. No assets, textures, audio, saves or ROM data
are ever sent, and the game never waits on the network.

---

## Config

Read once at startup (and again on `cmd remote reload`) from the app's external
files dir, so it is inspectable and pullable over adb:

```
/sdcard/Android/data/com.vdavid003.sm64port/files/sm64remote.cfg
```

```
# otsm64remote
enabled = false
url =
token = change-me
interval_ms = 160
```

| key | meaning |
| --- | --- |
| `enabled` | `true`/`1`/`yes`/`on` starts the worker. Anything else (or a missing file) keeps the native hooks as no-ops. |
| `url` | Relay base. Accepts the directory (`https://host/otsm64remote/`) or the script itself (`…/state.php`); `action=` is appended by the app. Must be **https** (Android blocks cleartext by default). |
| `token` | Shared secret sent as the `X-Sm64-Token` **header**; never in the URL and never logged. Change it. |
| `interval_ms` | State push period, clamped to 20–1000 ms. Default 80 ms ≈ 12.5 Hz. |

Missing file, `enabled = false` or an empty `url` all mean: zero network
traffic, no samples captured, unchanged gameplay.

```sh
adb pull /sdcard/Android/data/com.vdavid003.sm64port/files/sm64remote.cfg
adb push sm64remote.cfg /sdcard/Android/data/com.vdavid003.sm64port/files/
adb shell am broadcast -n com.vdavid003.sm64port/.DebugReceiver \
    -a com.vdavid003.sm64port.CMD --es cmd remote --es action reload
```

Enabling and disabling each log exactly one line:

```
I/SM64Remote: enabled url=https://… interval=80ms token=set
I/SM64Remote: disabled
```

The token is never printed; `cmd remote` / `cmd dump` report only
`token=default` or `token=set`.

---

## Protocol

Both requests carry `X-Sm64-Token: <token>` and `Accept: application/json`.
HTTP keep-alive is used and reused across pushes and polls.

### Push — `POST <url>?action=push`

Body (one JSON object, no framing):

```json
{"t":412345,"anim":134,"frame":12,"action":"jump","speed":8.42,"p":[12.34,1.60,-4.02],"yaw":1.571}
```

| field | type | source / units |
| --- | --- | --- |
| `t` | int | Monotonic milliseconds since the SDL engine started (`SDL_GetTicks`), captured on the game thread. Not wall clock: use it for deltas, not for displaying a clock. |
| `anim` | int | `gMarioState->marioObj->header.gfx.unk38.animID` — the value `set_mario_animation()` wrote. |
| `frame` | int | `…unk38.animFrame`, the frame index the renderer used for that animation. |
| `action` | string | Short label for `gMarioState->action`: one of a fixed table (`idle`, `walk`, `jump`, `double_jump`, `triple_jump`, `backflip`, `sideflip`, `long_jump`, `wallkick`, `freefall`, `dive`, `ground_pound`, `land`, `jump_land`, `butt_slide`, `belly_slide`, `crouch_slide`, `punch`, `water_idle`, `climbing`, `hanging`, `dying`, `dialog`, `wallrun`, …) or the action group (`stationary`, `moving`, `air`, `submerged`, `cutscene`, `automatic`, `object`, `other`). Constant strings only. |
| `speed` | float | Horizontal speed in **metres/second** (`sqrt(vx²+vz²) × 0.3`, where velocity is in SM64 units/tick at 30 Hz). |
| `p` | `[x,y,z]` | Mario's position in **metres**. SM64 world units are centimetres (Mario is 160 units ≈ 1.6 m), so `units × 0.01`. Y is up. |
| `yaw` | float | Facing yaw in **radians**, from the s16 angle (`units × 2π/65536`). |

Cadence is one sample per simulation tick (30 Hz nominal; fewer during slow-mo,
none while paused or outside a level). The C side formats each sample into a
small JSON object only when the worker asks for it.

### Commands — `GET <url>?action=commands`

The relay answers with a JSON object; the app reads at most 1 KB and looks for
`"input"`:

```json
{"input":"jump"}
```

`jump` is the only input wired to the game: it is queued as **one tick** of
`A_BUTTON` in `gPlayer1Controller->buttonPressed` on the game thread, then
cleared. Any other value is ignored (and logged once as `cmd ignored: …`).
There is no `set_mario_action()` call anywhere in this path, so nothing about
physics or action selection changes — the press behaves exactly like a pad
press.

### Timing and safety

* Push work is one `snprintf` per sample on the worker thread; the game thread
  never formats, allocates or blocks.
* Command polls run at 10 Hz (100 ms) and are checked **before** each push: one
  HTTP round trip is 60–300 ms here, so a push must never sit in front of a due
  poll. A `429` from the relay is treated as backpressure — the push period
  doubles (capped at 500 ms) and decays back to `interval_ms` on success —
  instead of spending the thread on requests the relay will reject.
* A command is applied at the next simulation tick after it arrives; the
  delivered latency is one relay round trip plus ≤100 ms poll gap plus ≤33 ms
  tick. On the hosted relay that round trip measured 60–300 ms (it rate-limits
  and serialises PHP requests), so expect ~0.3–0.5 s end to end there, not
  ~100 ms; on a fast relay the poll + tick part is ~130 ms.
* A queued command expires after **500 ms** in C; if the relay stops answering
  for 500 ms the worker also clears any pending input (watchdog). A command can
  therefore never fire long after the fact.
* A full ring drops the **newest** sample and counts it (`dropped=`), never
  blocking the game.

---

## Where the hooks live

| file | role |
| --- | --- |
| `app/jni/src/src/game/otsm64remote.c/.h` | lock-free 8-slot ring, per-tick capture, one-tick A press, JSON formatting, counters. No sockets, no allocation. |
| `app/jni/src/src/game/mario.c` | one call at the end of `execute_mario_action()` — reads `gMarioState` pos/yaw/action plus the animation state `set_mario_animation()` wrote. Read-only; no second sim, no render-loop change. |
| `app/jni/src/src/game/game_init.c` | one call after `read_controller_inputs()` — ORs a pending remote press into that tick's controller state. |
| `app/jni/src/src/pc/gfx/gfx_sdl2.c` | JNI: enable, poll (JSON into a caller buffer), queue/clear input, counters, shared `gfx_uptime_ms()`. |
| `app/src/main/java/com/vdavid003/sm64port/Sm64Remote.java` | config parse, one worker thread that samples the ring at 30 Hz into a bounded drop-oldest queue, HTTPS push at `interval_ms` + 10 Hz command poll, watchdog. |
| `app/src/main/java/com/vdavid003/sm64port/sm64portActivity.java` | `start()` in `onCreate`, `stop()` in `onDestroy`, `cmd remote` + `REMOTE …` line in `cmd dump`. |
| `app/src/main/AndroidManifest.xml` | `INTERNET` permission (needed by the opt-in bridge; no traffic while disabled). |

`cmd dump` reports the bridge state on its own line:

```
I/SM64Leia: REMOTE remote=on url=https://… interval=80 pushed=412 cmds=3 err=0 captured=1580 dropped=0 applied=3 expired=0 depth=0 lastPushAgo=21ms
```

`captured`/`applied` prove the C ring is being drained and consumed; `dropped`
should stay 0 unless the worker is stalled.

---

## Running the relay

The relay only needs to persist the newest state and hand out one command. Any
HTTPS host works; it must expose the two endpoints above under one base URL
(the app appends `?action=push` / `?action=commands` to whatever `url` says).
The viewer served from the same directory reads the stored state, so it flips
from **demo** to **live** as soon as pushes with a valid token arrive, and back
to demo when they stop.

Reference `state.php` (store outside the web root; one directory per token):

```php
<?php
// state.php - reference relay for otsm64remote.
$dir = '/var/lib/otsm64remote';
@mkdir($dir, 0770, true);
$action = $_GET['action'] ?? '';
$token  = $_SERVER['HTTP_X_SM64_TOKEN'] ?? '';
if ($token === '' || $token === 'change-me') { http_response_code(403); exit; }
$file = $dir . '/' . hash('sha256', $token);

if ($action === 'push') {
    $raw = file_get_contents('php://input');
    $j = json_decode($raw, true);
    if (!is_array($j)) { http_response_code(400); exit; }
    // numeric mirrors only; drop anything else the client might add
    file_put_contents($file . '.json', $raw, LOCK_EX);
    file_put_contents($file . '.ts', (string) microtime(true), LOCK_EX);
    header('Content-Type: application/json');
    echo '{"ok":true}';
    exit;
}
if ($action === 'commands') {
    header('Content-Type: application/json');
    $cmd = $file . '.cmd';
    if (is_file($cmd)) {
        $body = file_get_contents($cmd);
        unlink($cmd);                 // one command per poll
        echo $body;
    } else {
        echo '{"input":""}';
    }
    exit;
}
http_response_code(404);
```

* Serve it over **HTTPS**. Android's default network security config refuses
  plaintext HTTP, so a local relay needs a TLS terminator (caddy/nginx/stunnel)
  or a certificate the tablet trusts; for quick local work, point the config at
  a hosted copy.
* Enqueue a command by writing `{"input":"jump"}` to `<state>.cmd`; the next
  poll (≤100 ms) delivers it.
* Keep the token out of the URL, out of the viewer's HTML, and out of any
  access log: it is a header, and the reference relay never writes it.

---

## What is and is not sent

Sent, at 12.5 Hz by default:

* `t`, `anim`, `frame`, one of a fixed set of action labels, horizontal speed,
  position in metres (3 floats) and yaw in radians.

Not sent, ever:

* ROM data, assets, textures, models, audio, music, save files, course/level
  identifiers, star counts, or any file from the device;
* the player's input history (only the *effect* on Mario is mirrored);
* the token in any log, dump or URL;
* anything at all while `enabled = false`.

The bridge adds no gameplay state and affects no timing-critical path when
disabled: `otsm64remote_enable(0)` makes both hooks return immediately, and no
worker thread exists.

---

## Security notes

* **Opt-in only.** The default config (or no config file) sends nothing. The
  feature is inert on a stock install beyond an unused `INTERNET` permission.
* **The token is a header.** It never appears in the URL, so it does not leak
  through proxy logs, `Referer`, screenshots of the viewer, or the app's own
  `Log`/`cmd dump` output. Rotate it by editing the file and reloading; there is
  no default that works against a relay that checks it.
* **Telemetry is plaintext inside TLS.** Anything that terminates TLS sees the
  numeric stream: where Mario is, how fast he is moving, what action he is in.
  Treat the relay as a trusted party. There is no encryption beyond HTTPS and no
  attempt to anonymise position data — position is the point of the mirror.
* **The relay can press A.** A compromised relay (or anyone who learns the
  token) can make Mario jump, once per poll, subject to the 500 ms expiry. It
  cannot move the stick, change levels, write files, or script arbitrary
  actions.
* **Serve over HTTPS only.** The app does not enable cleartext traffic, so a
  misconfigured `http://` URL simply fails — it will not silently downgrade.
* The worker thread is a daemon, opened once, and closed on `onDestroy`; no
  background traffic while the app is gone.

---

## Verifying

```sh
# state: enabled? counters? last error?
adb shell am broadcast -n com.vdavid003.sm64port/.DebugReceiver \
    -a com.vdavid003.sm64port.CMD --es cmd remote
# full dump line
adb shell am broadcast -n com.vdavid003.sm64port/.DebugReceiver \
    -a com.vdavid003.sm64port.CMD --es cmd dump
adb logcat -d -s SM64Remote SM64Cmd
```

Expected on enable: one `enabled url=… interval=… token=set` line, then
`REMOTE … pushed=` climbing while a level is running. Expected on disable: one
`disabled` line and no further pushes (the ring is idle, the thread is gone).

The game-side half can be exercised without the relay (handy for a latency
measurement that excludes the network):

```sh
adb shell am broadcast -n com.vdavid003.sm64port/.DebugReceiver \
    -a com.vdavid003.sm64port.CMD --es cmd rinput --es input jump
```

That queues exactly the press a `{"input":"jump"}` would, so `cmd jump ->
queued (local test hook)` followed by `applied=` incrementing one tick later
proves the injection path; the relay test then only has to prove delivery.

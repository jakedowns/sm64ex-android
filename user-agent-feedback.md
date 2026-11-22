# User / agent feedback log

Running record of direct user feedback, kept for future reference.

## 2026-11-22T07:09:28Z

- User: **"you're a champ"** (after the feedback-v2 teaser was uploaded;
  same message thread also said "great!").
- Accomplished in that pass:
  - Trailer feedback v2: ground pound now happens **on top of the 30-cube
    pyramid**, then punch/kick target the nearest cube; reduced impulses and
    10 Box3D substeps stop cube interpenetration.
  - Visible jetpack nozzle plume + ageing exhaust trail.
  - Grapple anchors on the **room ceiling** and reels Mario up on a visible
    white rope.
  - Damage/power HUD hidden for the take; close punch/kick tracking cameras.
  - Seamless portal camera handoff (wall/back/ceil verified at
    8.8/10.0/9.8 px).
  - 30 s teaser recorded and uploaded: https://gofile.io/d/cYhOk3e6
- Commit: `5bd3299 feat(ninja): trailer feedback v2 + seamless portal handoff`.
- Fast chain on that build: PASS 8/8 (flags=0x7ff, stars 1/1, crossings 117).

### Follow-up requests after that take

- Fix the goomba finale: N6 looks like an invisible platform under Mario.
  Root cause: the ninja-course platform display lists (`ptc_dl_l1`, `n1..n8`)
  exist but are never referenced by the level geo.
- Next recording: portrait aspect (ffmpeg crop) and 60 fps.
- Add yellow in-game section captions mid-video.
- Record with sound, turning the device volume up.

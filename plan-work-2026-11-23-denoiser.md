# Rate limiting + alternative denoised rendering path

Date: November 23, 2026

Goal: add a measurable, configurable rate limiter and a selectable
denoiser-based alternative rendering path. The existing main path stays the
default and must keep working unchanged.

> [!IMPORTANT] Adb/device sessions: export `AGENT_TABLET_MUTE=1`; the lock
> script mutes and verifies the tablet. Task list mirrors the progress tree
> `denoise-plan-2026-11-23` (dr01..dr07); keep both in sync.

## Task list

- [ ] dr01 Research target hardware + open-source denoiser (subagent, docs)
- [ ] dr02 Implement precise frame pacing / rate limiting + stats
- [ ] dr03 Implement the alternative denoised rendering path (off by default)
- [ ] dr04 Controls: adb keys + debug UI (path, denoise on/off, intensity)
- [ ] dr05 On-screen captions for path/denoise/intensity/frames/bounces/rate
- [ ] dr06 Smoke test across the requested combinations
- [ ] dr07 Record the annotated comparison video + document results

## Constraints

- Main path default and untouched; alternative path selectable at runtime.
- GLES2 only (no compute); stereo per-eye FBOs.
- Target: Leia Lume Pad 2, Android, 30-60 fps capture.
- Denoiser must be open source with a documented license.

## Validation

1. Frame-time / jitter telemetry first (rate limiter).
2. Side-by-side stills for denoise off/on and intensities.
3. Video with burned-in captions for every configuration.

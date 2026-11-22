# 100 ideas — next passes on the ninja teaser and the chamber

Grouped by area, not priority. **Complexity** 1 = a few lines, 10 = multi-week.
**Impact** 1 = nice-to-have, 10 = changes how the whole demo reads.

| Title | Description | Complexity (1–10) | Impact (1–10) |
| --- | --- | --- | --- |
| 1. Director timeline editor | Scrub a keyframed camera/FOV/speed track in-app and re-record only the changed beats. | 8 | 9 |
| 2. Beat markers + edit decision list | Log phase, tick and frame for every beat into a JSON EDL the encoder can consume. | 3 | 8 |
| 3. Motion blur | Velocity-based blur on Mario, the goomba and the rope; keep wide establishing shots clean. | 5 | 7 |
| 4. Impact frames | Two or three high-contrast flash frames with radial streaks on slams and portal exits. | 3 | 7 |
| 5. Speed ramps | Per-phase time-scale curve: 0.25x on the slam, 2x on traversal, 1x on landings. | 6 | 8 |
| 6. Letterbox + safe-area overlay | Toggle 2.39:1 bars and title-safe guides while framing shots. | 2 | 5 |
| 7. FOV kick on acceleration | Tie FOV to horizontal speed and damage so the camera physically feels the motion. | 4 | 6 |
| 8. Camera shake presets | Impact, landing and portal-entry shakes with exponential falloff, safe for stereo. | 4 | 7 |
| 9. Whip-pan blends | Replace hard cuts with directional blur whip pans between shots. | 5 | 6 |
| 10. Colour grade LUT | Filmic teal-orange grade applied after the chain, toggleable and baked into capture. | 4 | 7 |
| 11. Impact post FX | Vignette pulse and subtle chromatic aberration on the big hits. | 3 | 5 |
| 12. Fake depth of field | Blur far geometry in a layered pass and keep Mario crisp. | 6 | 6 |
| 13. In-engine title sting | Render the logo and title card with the HUD font so they match the captions. | 5 | 6 |
| 14. End card | Feature list plus a QR code pointing at the build or repo. | 3 | 5 |
| 15. Vertical and square reframes | Per-shot crop keys for 9:16 and 1:1 instead of a centre crop. | 6 | 8 |
| 16. Burn-in captions from JSON | Drive captions from a timing file so re-edits never touch C. | 4 | 7 |
| 17. Localised captions | Externalise caption strings for EN/JA and other languages. | 3 | 4 |
| 18. Beat-synced music | Broadcast a metronome tick so the chain lands on musical beats. | 6 | 8 |
| 19. Sound design pass | Whooshes, rope twang, splat squelch and portal hum layered over the PCM tap. | 5 | 9 |
| 20. Music ducking | Sidechain the music under captions and impact hits. | 3 | 5 |
| 21. 15-second social cut | Generate a shorter cut from the same beat markers. | 3 | 7 |
| 22. GIF/WebP loop export | Short silent loop for chat previews. | 2 | 4 |
| 23. Thumbnail generator | Pick the best frame per beat and overlay the title. | 3 | 5 |
| 24. Freeze-frame finale | Hold the densest splat frame with a slow bloom instead of a moving hold. | 2 | 6 |
| 25. Freecam flight recording | Record a manual flight and replay it deterministically as a camera rail. | 6 | 7 |
| 26. Momentum reel-in | Grapple pull preserves and converts swing momentum instead of a fixed retract rate. | 5 | 8 |
| 27. Anchor switching | Release and re-attach mid-swing to chain two anchors in one arc. | 5 | 8 |
| 28. Grapple dynamic targets | Attach to goombas and boxes and pull them toward Mario. | 6 | 7 |
| 29. Grapple aim assist | Highlight valid anchors in the reticle and snap within a small cone. | 3 | 6 |
| 30. Rope collision | Rope wraps around corners and shortens realistically. | 7 | 5 |
| 31. Wall-run camera roll | Roll and FOV tied to wall-run speed for a proper Titanfall feel. | 4 | 7 |
| 32. Wall-jump chains | Alternate between parallel walls while keeping speed. | 6 | 7 |
| 33. Ledge grab | Catch a ledge at the end of a run and mantle up. | 5 | 6 |
| 34. Jetpack upgrades | Fuel pickups, capacity tiers and persistence across sessions. | 4 | 6 |
| 35. Jetpack hover mode | Separate hover and burst modes on a toggle. | 4 | 5 |
| 36. Portal placement ghost | Preview the landing fan and surface validity before firing. | 4 | 7 |
| 37. Portalable surface mask | Reject sky, water and moving geometry with a clear invalid effect. | 4 | 6 |
| 38. Portal momentum | Preserve velocity direction and speed through a portal pair. | 6 | 9 |
| 39. Recursive portal view | Real relay rendering instead of black fans, with a recursion depth limit. | 8 | 9 |
| 40. Portal actor traversal | Generalise the goomba loop to any actor with a per-actor cooldown. | 5 | 8 |
| 41. Loop safety | Cap portal recursions per actor and log violations instead of hanging. | 3 | 5 |
| 42. Box3D stress scene | A 200-cube pile that fractures and settles as a physics showcase. | 7 | 6 |
| 43. Destructible props | Chunked breakables with debris that fades out. | 8 | 7 |
| 44. Rocket launcher | Projectile with radial impulse and prop destruction. | 6 | 8 |
| 45. Skateboard + grinds | Board pickup and auto-generated grind rails from level ledges. | 9 | 8 |
| 46. Kart mode | Drift boost, glider and simple track markers inside the chamber. | 9 | 7 |
| 47. Paint-to-reveal | Splats reveal hidden platforms as a gameplay mechanic. | 7 | 6 |
| 48. Splats on level geometry | G-buffer albedo sampling so walls and floors can be painted too. | 9 | 8 |
| 49. Animated splats | Drips and settling over a few seconds after impact. | 6 | 6 |
| 50. Material-aware splats | Metal streaks, grass absorbs, wood splinters. | 7 | 5 |
| 51. Splat brush and reset | Erase and clear controls plus per-level persistence. | 4 | 5 |
| 52. Ghost replay | Time-trial ghost of the chain run overlaid in the chamber. | 7 | 6 |
| 53. Replay buffer | Always-on 30 s buffer at 60 fps with one-command export. | 8 | 8 |
| 54. Photo mode | Pause, freecam, filters and a high-res screenshot. | 5 | 7 |
| 55. Named save states | Snapshot and restore any chain phase by name. | 4 | 6 |
| 56. Checkpoint respawn | Respawn at the last beat without reloading the level. | 4 | 5 |
| 57. Practice teleports | One command per beat with a printed action list; extends the existing PTC actions. | 2 | 4 |
| 58. 60 Hz simulation interpolation | Interpolate render state between 30 Hz ticks for smoother capture. | 7 | 8 |
| 59. Frame pacing report | Log frame-time percentiles and dropped frames per phase. | 3 | 6 |
| 60. GPU timer queries | Per-phase GPU cost to find the finale bottleneck. | 5 | 6 |
| 61. Instanced splat rendering | Batch point sprites so max density still holds frame rate. | 6 | 8 |
| 62. Splat culling | Screen-space error and frustum culling for splat clusters. | 6 | 6 |
| 63. Stereo splat reprojection | Render splats once and reproject for the second eye. | 8 | 6 |
| 64. Smarter occluder fade | Distance and view-angle fade with hysteresis to stop popping. | 4 | 5 |
| 65. Dithered occluder fade | Alpha dithering instead of an on/off visibility flip. | 3 | 5 |
| 66. Stencil portal rendering | Single-pass stencil instead of per-portal FBOs. | 8 | 6 |
| 67. Texture atlas | Pack portal fans and splat sprites into one atlas. | 4 | 4 |
| 68. Shader warm-up | Compile all variants at load to remove first-use hitches. | 4 | 5 |
| 69. Thermal guard | Drop the effect tier when the device gets hot and log the switch. | 4 | 5 |
| 70. Power profile report | Battery and energy cost of one full chain run. | 3 | 4 |
| 71. Crash dump with phase | Persist chain phase and the last ticks on crash for triage. | 4 | 6 |
| 72. Determinism hash | Hash RNG and object states per tick and fail loudly on drift. | 6 | 7 |
| 73. Golden-frame diffs | SSIM thresholds per beat to catch visual regressions. | 6 | 7 |
| 74. One-command capture | Chain, capture, encode and verify in a single script. | 4 | 8 |
| 75. Headless host build | SDL2 + OSMesa engine build to iterate cameras without the tablet. | 9 | 8 |
| 76. Timeline JSON dump | Phase, tick, action and camera per frame for external editing. | 3 | 6 |
| 77. Scenario DSL | Declare chain sections in YAML instead of editing C. | 6 | 7 |
| 78. Generated beat sheet | Render the beat table into the docs from the chain source. | 3 | 4 |
| 79. Annotated screenshots | Overlay phase and tick on captures for review. | 2 | 4 |
| 80. Audio sync check | Measure beat-to-hit offset between the PCM tap and the frames. | 5 | 6 |
| 81. Device matrix | Run the chain across Android versions and GPUs. | 7 | 5 |
| 82. Nightly regression bot | Run the chain nightly and post a PASS/FAIL diff. | 6 | 6 |
| 83. Debug command fuzzing | Fuzz the DebugReceiver for crashes and bad states. | 5 | 5 |
| 84. Structured logging | JSON logs with levels and filters instead of ad hoc logcat greps. | 4 | 6 |
| 85. Web debug dashboard | Stream the dump and screenshots to a browser over adb. | 7 | 5 |
| 86. Replay file format | Compact `.sm64r` replay of inputs and checkpoints. | 7 | 6 |
| 87. Previs to C export | Emit the planned camera path as C from the Python previs. | 4 | 7 |
| 88. Previs vs take overlay | Overlay the planned path on a real take to see the drift. | 4 | 5 |
| 89. Caption placement solver | Move captions to avoid covering Mario and the portals. | 3 | 4 |
| 90. Settings screen | Expose freecam, splat, wall-run and portal toggles in the UI. | 5 | 7 |
| 91. Touch layout editor | Drag and resize the on-screen controls. | 6 | 6 |
| 92. Controller glyph hints | Detect pad type and show matching button glyphs. | 3 | 4 |
| 93. Tutorial chamber | Short onboarding for grapple, wall-run and portals. | 6 | 8 |
| 94. Accessibility options | Colourblind-safe portal colours and a motion-reduction mode. | 3 | 5 |
| 95. UI localisation | Externalise menu and HUD strings. | 5 | 4 |
| 96. Performance presets | Quality and battery modes with clear tradeoffs. | 3 | 5 |
| 97. Local replay sharing | Export a replay plus a QR to pull it off the tablet. | 4 | 5 |
| 98. Chamber scripting API | Let levels declare chain beats and captions in data rather than C. | 7 | 8 |
| 99. WASM browser demo | Build the chamber for browsers as a showcase. | 9 | 7 |
| 100. Release packaging | Signed APK, changelog and install guide per milestone. | 4 | 6 |

## Where I would start

1. **#19 sound design** — the take already has audio; the biggest remaining quality jump is sound.
2. **#74 one-command capture** — removes the manual record/encode/verify loop from every iteration.
3. **#2 beat EDL + #21 short cut** — one timing source, several cuts, no hand-syncing.
4. **#38 portal momentum** — turns portals from a visual into a movement mechanic.
5. **#58 60 Hz interpolation** — smooths all the fast beats without re-shooting anything.

# Bouncy Bacon — PS3 port

A native PS3 homebrew port of Evan Bacon's MIT-licensed Expo game
([EvanBacon/Expo-Crossy-Road](https://github.com/EvanBacon/Expo-Crossy-Road)),
rewritten in C for the PSL1GHT SDK. Same rows, cars, logs, lily pads, trains,
hop/squash animations, particles, sounds and scoring as the original.

The title uses the original project's package name ("Bouncy Bacon"); the
repo's "CROSSY ROAD" logo image is not included.

## Get the .pkg

Push this folder to a GitHub repo (e.g. `odhz/BouncyBacon-PS3`). The
**Build PS3 pkg** workflow runs automatically; open the run and download the
`bouncy-ps3-pkg` artifact. The first run installs the PS3 toolchain (a few
minutes with upstream's prebuilt nightly, ~1.5 h if it has to build from
source); later runs reuse the cache.

Local build with a PS3 toolchain installed:

```
export PS3DEV=/usr/local/ps3dev PSL1GHT=$PS3DEV
make pkg
```

## Install (CFW, e.g. Evilnat/Cobra)

1. Copy `bouncy-ps3.pkg` to the root of a FAT32 USB stick.
2. XMB → Game → **Package Manager** → Install Package Files → Standard.
3. Launch **Bouncy Bacon** from the Game column.

Title ID `BBCN00001`. Saves (`save.txt`: top score + mute) go to
`/dev_hdd0/game/BBCN00001/USRDIR/`.

## Controls

| Input | Action |
|---|---|
| D-pad / left stick | hop (squash on press, hop on release — like the original swipe) |
| X | hop forward / start / play again |
| START | pause |
| O (paused or game over) | back to title |
| SELECT | sound on/off |
| TRIANGLE | show/hide FPS counter |
| PS button → Quit Game | returns to XMB cleanly |

## How it's built

- `tools/convert.py` reads the original OBJ models + palette textures and
  converts the sounds to 48 kHz PCM. Object models go through the remaster
  pass in `tools/remaster.py`: every textured face is rebuilt as clean
  rectangles matching the painted pixels (windows, lights, eyes, bark), so
  the PS3 shows the original paint job crisply with no texture sampling.
  Ground strips are baked by subdividing faces until each is one colour.
  `art/` holds packed copies of the original and remastered models for
  previewing in Blender. Output is `source/assets_data.c` — no file I/O and no
  endianness issues on the big-endian Cell.
- `source/render.c` — software renderer (flat-shaded triangles, z-buffer,
  the original's Lambert lighting values, planar ground shadows) at 640×360.
  Edges and depth step in 64-bit fixed point (no float↔int conversions per
  scanline, which stall the Cell PPU), and the screen is split into two
  halves rendered on the PPU's two hardware threads.
- The RSX's scaled-blit engine upscales the frame to the TV mode; a
  first-frame self-test falls back to CPU scaling if that doesn't work.
- `source/game.c` — logic ported from `GameEngine.ts`, `CrossyGame.ts`,
  `CrossyPlayer.ts`, `Row/*` and `Particles/*`, on a fixed 60 Hz step with a
  small GSAP-style tween engine.
- `source/plat_ps3.c` — RSX framebuffer flip, pads, audio port, saves.
- `test/plat_test.c` — headless desktop build for testing without a PS3:
  `make test`, then
  `./build_test/bouncy_test out 600 "60:+X 64:-X 90:+U 93:-U" "100,200"`
  writes frames as PPM plus `audio.wav`.

## PS3 issues handled up front

- **Quit Game hang:** sysutil callback registered and polled every frame;
  exit finishes the RSX, closes the audio port and releases pads.
- **Dropped inputs:** `ioPadGetData` returns `len == 0` when nothing changed —
  the last state is kept instead of reading as "all released".
- **Any controller port** works; stick has a deadzone with hysteresis.
- **Tearing:** waits for the previous flip before drawing into a buffer.
- **Slow VRAM reads:** the upscaler never reads back from the framebuffer.
- **Silent next app:** audio port is always closed on exit.
- **Saves** are written next to the EBOOT (path from `argv[0]`).
- **Rumble** on death (DualShock 3), only sent on change.

## Changes from the original

- A new hop cancels the previous hop's animation (original could clear
  `moving` mid-air on rapid input).
- Turning takes the shortest way round (original could spin 270°).
- Train collision matches the train's actual length.
- Restart resets the camera instead of panning back.
- Being run over by a train plays a sound.
- Grass rows alternate light/dark (both textures ship in the repo).
- Train-pass sounds fade with distance from the chicken.

## Credits

Game design, code and voxel art: Evan Bacon, MIT licence (see
`LICENSE-original-MIT`). PS3 port built with PSL1GHT / ps3toolchain.

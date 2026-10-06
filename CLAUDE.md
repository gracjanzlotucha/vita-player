# Fidelity — PS Vita lossless music player

Homebrew music player for a HENkaku PS Vita with SD2Vita. Plays FLAC/WAV/MP3/OGG
from the memory card. See README.md for controls and the user-facing feature list.

## Scope decisions
- Local files are the focus. Jellyfin streaming is a possible later addition (not started).
- Background playback (leaving the app) is out of scope. Screen-off while the app
  stays open is the supported pocket mode.

## Hard-won facts (tested on the user's console)
- `scePowerRequestDisplayOff()` pauses the app's audio. Screen off is done by
  `sceAVConfigSetDisplayBrightness(0)` and restoring the brightness read from the
  registry (`/CONFIG/DISPLAY`, `brightness`). Confirmed working.
- The system's own idle dim/screen-off can also pause audio, so while playing we
  call `sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT)` every loop.
- On PS button / resume from sleep (`sceAppMgrReceiveSystemEvent`) the backlight is
  restored so the user never lands on a dark home screen.
- Pocket mode locks all controls (user request: accidental skips in a pocket):
  SELECT shows "Controls locked" for ~1.2 s, then the backlight goes off;
  only holding SELECT ~1 s wakes it (the locking press must be released first).
- Track changes and scrubbing are fast since the background loader / cached
  Now playing layer / MP3 indexer work (PR #2) — confirmed by the user on hardware.

## Not yet verified on hardware
- Battery drain over long screen-off listening.
- Touch controls (front panel coordinates assumed 1920x1088 → halved to 960x544;
  touch sampling is stopped while the screen is off).
- Whether the BGM port ever refuses to open (there is a MAIN-port 48 kHz fallback).
- Cover Flow frame rate after the speed-up (present thread, art levels,
  culling, 444 MHz while animating) and the backdrop cross-fade; the user saw
  ~30 fps before. The log line "coverflow: ... fps" tells.

## Architecture
- Threads: UI = main thread (core 0); audio thread (prio 0x50, core 1); three
  low-priority workers on core 2 — the track info loader (`app.c`), the MP3
  seek indexer (`player.c`) and the library scanner/thumbnailer (`catalog.c`) —
  plus the present thread (prio 0x60, core 2, `platform_vita.c`): the UI draws
  into one of two RAM buffers while it copies the other to CDRAM and waits for
  vblank (frames are always fully redrawn, so the alternating buffers are
  safe). `plat_cpu_boost()` raises the clock 333 → 444 MHz while cover art is
  decoded and while Cover Flow animates.
- `src/platform.h` is the only seam between app code and the console.
  `platform_vita.c` implements it with Vita APIs; `platform_host.c` is a desktop
  stand-in that writes frames to PNG and audio to WAV.
- Decoders read through stdio with 128 KB buffers (`decoder.c`); tag/picture
  reads go in 64 KB pieces so they never hold up the audio thread's reads.
  MP3 length (without a Xing header) and seek tables come from the background
  indexer; until then the UI shows `--:--` and a seek builds the table inline.
- `player.c`: one high-priority audio thread. Decode → int32 stereo → either
  bit-exact >>16 (16-bit sources at a port-supported rate), TPDF dither (24-bit),
  or windowed-sinc resample + dither (88.2k/96k/192k). Next track is chained into
  the same output buffer, so same-rate tracks are gapless.
- `catalog.c`: the library is everything under `plat_music_dir()`
  (`ux0:data/Fidelity/Music`; no file browser). Scanned in the background on
  every start / Rescan; `library.idx` caches tags + format per file, keyed by
  path, size and mtime, so only new/changed files are read. Albums group by
  album + album artist (no album artist: album + folder, so compilations stay
  together); artists are album artists. The UI takes published catalogs from
  `cat_poll()`. Thumbnails (48/36 px) are made on demand, newest request first,
  and cached as raw RGBA in `ux0:data/Fidelity/cache/<key>.t48`; `cat_cover()`
  gives the 120 px album-page header. Cover Flow uses `cat_art()` (337 px
  plus box-filtered 304/251 px levels, 16-slot LRU owned by the UI thread,
  cached as `<key>.a337.jpg`) and `cat_backdrop()` (full-screen blurred
  backdrop, one at a time, handed over to the UI which frees it). Worker
  order: cover, art, backdrop, thumbnails.
- `app.c`: library (tabs Albums/Tracks/Artists/Settings, album and artist
  pages, mini player), now playing, input, screen-off mode, settings.
  Track changes never block the UI: `loader_thread` reads tags, decodes the
  cover once (400 px; 48 px is scaled from it) and builds the background; the UI
  collects results in `refresh_track()`. Art is keyed by a hash of the picture
  bytes, so tracks sharing a cover skip all of it. Now playing draws its
  per-track parts once into `np_static` and only the clock/timeline/controls
  per frame.
  Touch: draw code registers tappable zones with `hot()` as it draws; a tap
  becomes a one-tick virtual button press, so touch reuses the button handlers.
  Exceptions are list drag/fling scrolling (`list_y`, pixels) and progress-bar
  scrubbing (`player_seek_to` on release).
  Cover Flow (`album_view`): `cf_pos` (float, in albums) eases towards the
  selection in `cf_step()`; covers are placed by interpolating the Figma
  keyframes `CFK`. Same drawing moving or still (no shadow — it popped):
  `cat_art` levels (337/304/251) drawn by `gfx_cover` at ~1:1; nothing is
  drawn under a nearer opaque cover, and the backdrop only where no cover
  is. The backdrop is fetched once the selection rests 150 ms and
  cross-fades (450 ms, `gfx_crossfade_span`, NEON) — one fade at a time.
  The log gets "coverflow: N frames, fps, draw ms" after each animation.
- `gfx.c`: software renderer (anti-aliased SDF shapes, images, stb_truetype text)
  into a RAM buffer that is copied to a CDRAM framebuffer on present. Also
  rasterises SVG path data into coverage masks (`mask_from_path`), used for
  icons copied straight from Figma.

## Design
- Source of truth: Gracjan's Figma file `aneQdWK0xWY8aeacwMLOuV` (Figma
  connector). Frames are 960x544, so Figma coordinates map 1:1 to the screen.
- Implemented: Now Playing (node `1:2`), Library / Albums tab with the mini
  player (node `14:292`) and the Cover Flow album view (node `42:231`). Checked by overlaying renders on Figma exports.
- Fonts: Geist Regular/Medium and Geist Pixel Square (bars, pills, hints),
  all loaded with `css_px=1` (Figma/CSS em sizes, kerned); `text_at()`
  positions text by its Figma line-box top. Geist Pixel has no □/△, so the
  hint bar draws those keys as small shapes (`key_draw`).
- Backgrounds (blurred, saturated cover at 25%) for Now Playing and the mini
  player are built once per track by `image_backdrop()` (gfx.c) on the loader
  thread.
- Not in Figma, so designed to match (Gracjan may redesign): album page
  (120 px header + numbered track list), artist page, Tracks / Artists /
  Settings tabs, empty and scanning states, pause icons, active
  shuffle/repeat pill, repeat-one badge, "Not Playing", toast, the extra
  "△ Now Playing" hint in the library, the Album View setting and □ hint,
  Cover Flow title/pills moving to the artist line for long titles, covers
  beyond the second neighbour (shrink and fade out).
- Test library: `tools/make_test_albums.py <dir>` makes 12 placeholder albums
  (generated covers, quiet tones, tags). Host check: `SCRIPT=covers`.

## Building
Vita: VitaSDK at `$VITASDK`, then `mkdir build && cd build && cmake .. && make`
→ `build/Fidelity.vpk`. Linker uses `-z max-page-size=0x10000` because
vita-elf-create otherwise failed with "segment 1 overlaps".

Desktop test build (no console needed) — use it to check UI changes and audio:
```
gcc -O2 -Ithird_party/dr_libs -Ithird_party/stb src/app.c src/gfx.c src/player.c \
  src/decoder.c src/tags.c src/catalog.c src/platform_host.c src/main_host.c \
  src/stbiw_impl.c -o host_test -lm -lpthread
HOST_ROOT=/path/to/music ./host_test x ./shots            # scripted screenshots (HOST_FRESH=1: rescan)
HOST_ROOT=/path/to/album HOST_WAV=out.wav SCRIPT=audio ./host_test x .  # render audio
HOST_ROOT=/path/to/music SCRIPT=touch ./host_test x ./shots  # scripted touch gestures
```
Bit-exactness check: decode the same files with ffmpeg to s16le and compare bytes.

## Testing on the console
The user installs the VPK via VitaShell. Logs go to `ux0:data/Fidelity/log.txt`
(recreated each launch) — ask for it when something misbehaves on hardware.

## Owner
Gracjan is a UI/UX designer: the current visual design (colours, layout, name) is a
placeholder and he may supply his own designs.

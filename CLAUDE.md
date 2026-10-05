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

## Not yet verified on hardware
- Battery drain over long screen-off listening.
- Whether the BGM port ever refuses to open (there is a MAIN-port 48 kHz fallback).

## Architecture
- `src/platform.h` is the only seam between app code and the console.
  `platform_vita.c` implements it with Vita APIs; `platform_host.c` is a desktop
  stand-in that writes frames to PNG and audio to WAV.
- `player.c`: one high-priority audio thread. Decode → int32 stereo → either
  bit-exact >>16 (16-bit sources at a port-supported rate), TPDF dither (24-bit),
  or windowed-sinc resample + dither (88.2k/96k/192k). Next track is chained into
  the same output buffer, so same-rate tracks are gapless.
- `app.c`: both screens (library, now playing), input, screen-off mode, settings.
- `gfx.c`: software renderer (anti-aliased SDF shapes, images, stb_truetype text)
  into a RAM buffer that is copied to a CDRAM framebuffer on present.

## Building
Vita: VitaSDK at `$VITASDK`, then `mkdir build && cd build && cmake .. && make`
→ `build/Fidelity.vpk`. Linker uses `-z max-page-size=0x10000` because
vita-elf-create otherwise failed with "segment 1 overlaps".

Desktop test build (no console needed) — use it to check UI changes and audio:
```
gcc -O2 -Ithird_party/dr_libs -Ithird_party/stb src/app.c src/gfx.c src/player.c \
  src/decoder.c src/tags.c src/library.c src/platform_host.c src/main_host.c \
  src/stbiw_impl.c -o host_test -lm -lpthread
HOST_ROOT=/path/to/music ./host_test x ./shots            # scripted screenshots
HOST_ROOT=/path/to/album HOST_WAV=out.wav SCRIPT=audio ./host_test x .  # render audio
```
Bit-exactness check: decode the same files with ffmpeg to s16le and compare bytes.

## Testing on the console
The user installs the VPK via VitaShell. Logs go to `ux0:data/Fidelity/log.txt`
(recreated each launch) — ask for it when something misbehaves on hardware.

## Owner
Gracjan is a UI/UX designer: the current visual design (colours, layout, name) is a
placeholder and he may supply his own designs.

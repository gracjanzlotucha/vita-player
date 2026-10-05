# Fidelity — lossless music player for PS Vita (homebrew)

Plays FLAC, WAV, MP3 and OGG from the memory card / SD2Vita, with a
screen-off mode for pocket listening.

## Install
1. Copy `Fidelity.vpk` to the Vita (VitaShell → FTP or USB).
2. Install it with VitaShell (✕ on the file).
3. Put music anywhere on `ux0:` (e.g. `ux0:music/Artist/Album/…`).
   SD2Vita via StorageMgr/YAMT normally mounts as `ux0:`. Other mounts
   (`uma0:`, `imc0:` …) show up if you press ○ at the top of `ux0:`.

## Controls
| Where | Button | Action |
|---|---|---|
| Everywhere | START | Play / pause |
| | L / R | Previous / next track |
| | SELECT | Screen off (SELECT again to wake) |
| Library | ↑ ↓ / ← → | Move / page |
| | ✕ | Open folder, or play this file (queues the folder) |
| | ○ | Up one folder |
| | □ | Play everything in the selected (or current) folder, incl. subfolders |
| | △ | Now playing |
| Now playing | ✕ | Play / pause |
| | ← → | Seek −/+ 10 s (hold to repeat) |
| | □ | Shuffle |
| | △ | Repeat: off → all → one |
| | ○ | Back to library |

With the screen off only START, L, R and SELECT do anything.

## What "lossless" means here
The Vita's audio output is 16-bit. Fidelity uses the system's BGM audio port,
which accepts 44.1 kHz and 48 kHz natively, so:

- **16-bit FLAC/WAV at 44.1 or 48 kHz → bit-perfect.** Samples reach the port
  unchanged (verified bit-for-bit in the desktop test build).
- **24-bit at 44.1/48 kHz →** TPDF-dithered to 16-bit.
- **88.2/96/176.4/192 kHz →** windowed-sinc resampled to 44.1/48 kHz, then dithered.
- The BGM port's automatic level control (a loudness normaliser) is switched off.
- Tracks with the same sample rate play gaplessly.

Cover art: embedded FLAC/MP3 pictures, otherwise `cover.jpg`, `folder.jpg`,
`front.jpg` (or .png) in the album folder.

Settings (last folder, shuffle, repeat) live in `ux0:data/Fidelity/settings.txt`.
A log is written to `ux0:data/Fidelity/log.txt` on each launch.

## Building
Needs VitaSDK (`$VITASDK` set).

```
mkdir build && cd build
cmake .. && make
```

`third_party/` holds dr_libs (FLAC/MP3/WAV), stb_vorbis, stb_image and
stb_truetype. The UI font is Inter (SIL Open Font License).

There is also a desktop test build (no Vita needed) that runs the same app
code, writes screenshots to PNG and audio to WAV:

```
gcc -O2 -Ithird_party/dr_libs -Ithird_party/stb src/app.c src/gfx.c src/player.c \
  src/decoder.c src/tags.c src/library.c src/platform_host.c src/main_host.c \
  src/stbiw_impl.c -o host_test -lm -lpthread
HOST_ROOT=/path/to/music ./host_test x ./shots
```

## Code map
- `platform_vita.c` — all Vita APIs: display, buttons, audio port, power, files
- `player.c` — playback thread, queue, gapless chaining, dither, resampler
- `decoder.c` — FLAC/WAV/MP3/OGG behind one interface
- `tags.c` — Vorbis comments, ID3v2, embedded/folder cover art
- `library.c` — folder listing, natural sort, recursive "play all"
- `app.c` — UI (both screens), input, screen-off mode
- `gfx.c` — software renderer: anti-aliased shapes, images, text

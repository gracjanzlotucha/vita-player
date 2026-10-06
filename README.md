# Fidelity — lossless music player for PS Vita (homebrew)

Plays FLAC, WAV, MP3 and OGG from the memory card / SD2Vita, with a
screen-off mode for pocket listening.

## Install
1. Copy `Fidelity.vpk` to the Vita (VitaShell → FTP or USB).
2. Install it with VitaShell (✕ on the file) and start it once — that creates
   the music folder.
3. Copy your music into **`ux0:data/Fidelity/Music`** (any folder layout).
   The library is built from tags: albums, artists and tracks are found
   automatically. New or changed files are picked up on every start, or with
   Settings → Rescan Library.

The first scan reads every file and can take a while for a big library (it
runs in the background and the list fills in as it goes). After that only
new or changed files are read. Album thumbnails are made the first time
they're shown and cached in `ux0:data/Fidelity/cache`.

## Controls
| Where | Button | Action |
|---|---|---|
| Everywhere | START | Play / pause |
| | SELECT | Screen off and lock the controls (hold SELECT ~1 s to wake) |
| Library | L / R | Change tab (Albums, Tracks, Artists, Settings) |
| | ↑ ↓ / ← → | Move / page |
| | ✕ | Open the album or artist, play the track |
| | ○ | Back (album / artist page) |
| | □ | Shuffle the album (album page) |
| | △ | Now playing |
| Now playing | ✕ | Play / pause |
| | L / R | Previous / next track |
| | ← → | Seek −/+ 10 s (hold to repeat) |
| | □ | Shuffle |
| | △ | Repeat: off → all → one |
| | ○ | Back |

With the screen off every control is locked, touch included, so nothing
changes in a pocket; music keeps playing. Hold SELECT for about a second to
wake it. The Vita's volume buttons still work.

### Touch
Everything can also be done on the touch screen: tap a tab, an album, artist
or track; drag the list to scroll, flick it to scroll fast. The mini player at
the bottom of the library opens Now playing, and its buttons skip and
play / pause. In Now playing, tap the controls, and tap or drag along the
progress bar to seek (it jumps when you lift your finger). The hints along
the bottom are buttons too.

Touch is ignored while the screen is off, so nothing happens in a pocket.
After a touch scroll, the first D-pad press brings the selection back on screen.

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

Settings (last tab, shuffle, repeat) live in `ux0:data/Fidelity/settings.txt`,
the library index in `library.idx` next to it.
A log is written to `ux0:data/Fidelity/log.txt` on each launch.

## Building
Needs VitaSDK (`$VITASDK` set).

```
mkdir build && cd build
cmake .. && make
```

`third_party/` holds dr_libs (FLAC/MP3/WAV), stb_vorbis, stb_image and
stb_truetype. The UI fonts are Geist and Geist Pixel (SIL Open Font License).

There is also a desktop test build (no Vita needed) that runs the same app
code, writes screenshots to PNG and audio to WAV:

```
gcc -O2 -Ithird_party/dr_libs -Ithird_party/stb src/app.c src/gfx.c src/player.c \
  src/decoder.c src/tags.c src/catalog.c src/platform_host.c src/main_host.c \
  src/stbiw_impl.c -o host_test -lm -lpthread
HOST_ROOT=/path/to/music ./host_test x ./shots   # HOST_FRESH=1 to rescan from scratch
```

## Code map
- `platform_vita.c` — all Vita APIs: display, buttons, audio port, power, files
- `player.c` — playback thread, queue, gapless chaining, dither, resampler
- `decoder.c` — FLAC/WAV/MP3/OGG behind one interface
- `tags.c` — Vorbis comments, ID3v2, embedded/folder cover art
- `catalog.c` — music library: background scan, albums/artists, index and thumbnail caches
- `app.c` — UI (library tabs, album/artist pages, now playing), input, screen-off mode
- `gfx.c` — software renderer: anti-aliased shapes, images, text

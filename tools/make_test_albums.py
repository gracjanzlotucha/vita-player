#!/usr/bin/env python3
"""Builds a pack of placeholder albums for trying out the library views.

Each album is a folder of short, quiet FLAC tracks with tags and an embedded
front cover (generated abstract art, no real releases). Copy the folders into
ux0:data/Fidelity/Music on the Vita.

    python3 tools/make_test_albums.py dist/test-albums   # needs Pillow + ffmpeg
"""
import math
import os
import random
import subprocess
import sys
import tempfile

from PIL import Image, ImageDraw, ImageFilter

SIZE = 600

# (album, artist, year, palette, style, track titles)
ALBUMS = [
    ("Low Orbit", "Satellite Hearts", 2021, ["#0b1a3a", "#ff6b3d", "#ffd27a"], "sun",
     ["Launch Window", "Apogee", "Static Bloom", "Re-entry"]),
    ("Paper Lanterns", "Mira Okada", 2019, ["#f4e9d8", "#d6423b", "#1f2a44"], "circles",
     ["Kite String", "Lantern Walk", "Tidepool", "Night Market", "Embers"]),
    ("Glass Garden", "The Quiet Rooms", 2023, ["#0f3d3e", "#7fd1b9", "#e8f7ee"], "grid",
     ["Greenhouse", "Condensation", "Fern"]),
    ("Velvet Hours", "Nora Vale", 2018, ["#2b0f2e", "#c2185b", "#f8bbd0"], "waves",
     ["After Midnight", "Slow Dial", "Velvet", "Last Tram"]),
    ("Concrete Bloom", "Brutal Florals", 2022, ["#bdbdbd", "#212121", "#ff3d00"], "blocks",
     ["Rebar", "Pollen Count", "Overpass", "Weeds in the Cracks"]),
    ("Northern Static", "Fjell", 2020, ["#06141b", "#4fc3f7", "#e0f7fa"], "aurora",
     ["Polar Night", "Magnetosphere", "Snowblind"]),
    ("Citrus Season", "Juniper & Co.", 2024, ["#fff3c4", "#ff9f1c", "#2ec4b6"], "slices",
     ["Zest", "Orchard Road", "Sun Tea", "Pith", "Marmalade"]),
    ("Signal / Noise", "Datafield", 2017, ["#000000", "#00e676", "#ffffff"], "bars",
     ["Carrier", "Handshake", "Packet Loss", "Checksum"]),
    ("Desert Radio", "Coyote Hour", 2016, ["#3e1f0f", "#e07a3f", "#f2cc8f"], "dunes",
     ["Mile Marker 9", "Mirage", "Dust Devil"]),
    ("Soft Machines", "Unit 7", 2025, ["#e3e7ff", "#5c6bc0", "#ff80ab"], "rings",
     ["Boot Sequence", "Servo", "Gentle Logic", "Sleep Mode"]),
    ("Tidal", "Marlowe", 2015, ["#012a4a", "#2a6f97", "#a9d6e5"], "waves2",
     ["Undertow", "Salt", "Lighthouse Keeper", "Slack Water"]),
    ("Monochrome Summer", "Ada Lindqvist", 2022, ["#f5f5f5", "#111111", "#9e9e9e"], "stripes",
     ["Heatwave", "Shade", "Linen", "Late Light"]),
]


def rgb(h):
    h = h.lstrip("#")
    return tuple(int(h[i:i + 2], 16) for i in (0, 2, 4))


def mix(a, b, t):
    return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(3))


def gradient(c0, c1, vertical=True):
    im = Image.new("RGB", (SIZE, SIZE))
    d = ImageDraw.Draw(im)
    for i in range(SIZE):
        c = mix(c0, c1, i / (SIZE - 1))
        d.line([(0, i), (SIZE, i)] if vertical else [(i, 0), (i, SIZE)], fill=c)
    return im


def cover(style, pal, seed):
    rnd = random.Random(seed)
    a, b, c = (rgb(p) for p in pal)
    if style == "sun":
        im = gradient(a, mix(a, b, 0.5))
        d = ImageDraw.Draw(im)
        for k in range(8, 0, -1):
            r = 60 + k * 22
            d.ellipse([300 - r, 330 - r, 300 + r, 330 + r], fill=mix(b, c, 1 - k / 8))
        for y in range(380, SIZE, 18):
            d.rectangle([0, y, SIZE, y + 7], fill=a)
    elif style == "circles":
        im = Image.new("RGB", (SIZE, SIZE), a)
        d = ImageDraw.Draw(im)
        for _ in range(14):
            r = rnd.randint(30, 140)
            x, y = rnd.randint(0, SIZE), rnd.randint(0, SIZE)
            d.ellipse([x - r, y - r, x + r, y + r], fill=rnd.choice([b, c, mix(b, a, 0.4)]))
    elif style == "grid":
        im = Image.new("RGB", (SIZE, SIZE), a)
        d = ImageDraw.Draw(im)
        n = 6
        s = SIZE // n
        for i in range(n):
            for j in range(n):
                t = rnd.random()
                pad = rnd.randint(6, 30)
                col = mix(b, c, t)
                if rnd.random() < 0.5:
                    d.ellipse([i * s + pad, j * s + pad, (i + 1) * s - pad, (j + 1) * s - pad], fill=col)
                else:
                    d.rectangle([i * s + pad, j * s + pad, (i + 1) * s - pad, (j + 1) * s - pad], fill=col)
    elif style in ("waves", "waves2"):
        im = gradient(a, mix(a, b, 0.6))
        d = ImageDraw.Draw(im)
        for k in range(12):
            y0 = 120 + k * 38
            amp = 18 + k * 3
            ph = rnd.random() * 6
            pts = [(x, y0 + amp * math.sin(x / (70 if style == "waves" else 45) + ph)) for x in range(0, SIZE + 10, 10)]
            d.line(pts, fill=mix(b, c, k / 11), width=10 if style == "waves" else 6)
    elif style == "blocks":
        im = Image.new("RGB", (SIZE, SIZE), a)
        d = ImageDraw.Draw(im)
        for _ in range(9):
            x, y = rnd.randint(-50, 500), rnd.randint(-50, 500)
            w, h = rnd.randint(80, 260), rnd.randint(80, 260)
            d.rectangle([x, y, x + w, y + h], fill=rnd.choice([b, b, mix(a, b, 0.5)]))
        d.ellipse([370, 60, 530, 220], fill=c)
    elif style == "aurora":
        im = Image.new("RGB", (SIZE, SIZE), a)
        layer = Image.new("RGB", (SIZE, SIZE), (0, 0, 0))
        d = ImageDraw.Draw(layer)
        for k in range(5):
            y0 = 140 + k * 50
            pts = [(x, y0 + 60 * math.sin(x / 110 + k)) for x in range(0, SIZE + 10, 10)]
            d.line(pts, fill=mix(b, c, k / 4), width=34)
        layer = layer.filter(ImageFilter.GaussianBlur(22))
        im = Image.blend(im, layer, 0.85)
        d = ImageDraw.Draw(im)
        pts = [(0, 520)] + [(x, 470 - 70 * abs(math.sin(x / 90))) for x in range(0, SIZE + 10, 20)] + [(SIZE, 520)]
        d.polygon(pts + [(SIZE, SIZE), (0, SIZE)], fill=(4, 10, 14))
        for _ in range(60):
            x, y = rnd.randint(0, SIZE), rnd.randint(0, 260)
            d.point((x, y), fill=c)
    elif style == "slices":
        im = Image.new("RGB", (SIZE, SIZE), a)
        d = ImageDraw.Draw(im)
        for cx, cy, r in ((190, 210, 150), (430, 400, 130), (150, 480, 80)):
            d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=b)
            d.ellipse([cx - r + 14, cy - r + 14, cx + r - 14, cy + r - 14], fill=mix(b, a, 0.55))
            for k in range(8):
                ang = k * math.pi / 4
                d.line([(cx, cy), (cx + (r - 18) * math.cos(ang), cy + (r - 18) * math.sin(ang))], fill=b, width=5)
        d.rectangle([0, 560, SIZE, SIZE], fill=c)
    elif style == "bars":
        im = Image.new("RGB", (SIZE, SIZE), a)
        d = ImageDraw.Draw(im)
        n = 40
        w = SIZE / n
        for i in range(n):
            h = int(abs(math.sin(i * 0.37) * math.cos(i * 0.11)) * 380 + rnd.randint(10, 80))
            d.rectangle([i * w + 2, 300 - h // 2, (i + 1) * w - 3, 300 + h // 2], fill=b if i % 7 else c)
    elif style == "dunes":
        im = gradient(c, b)
        d = ImageDraw.Draw(im)
        d.ellipse([360, 90, 470, 200], fill=mix(c, (255, 255, 255), 0.5))
        for k in range(5):
            y0 = 300 + k * 60
            pts = [(x, y0 + 40 * math.sin(x / (140 - k * 12) + k * 1.7)) for x in range(0, SIZE + 10, 10)]
            d.polygon(pts + [(SIZE, SIZE), (0, SIZE)], fill=mix(b, a, k / 4))
    elif style == "rings":
        im = Image.new("RGB", (SIZE, SIZE), a)
        d = ImageDraw.Draw(im)
        for k in range(14, 0, -1):
            r = k * 22
            d.ellipse([300 - r, 300 - r, 300 + r, 300 + r], outline=b if k % 2 else c, width=9)
        d.ellipse([270, 270, 330, 330], fill=c)
    else:  # stripes
        im = Image.new("RGB", (SIZE, SIZE), a)
        d = ImageDraw.Draw(im)
        for i in range(-SIZE, SIZE * 2, 46):
            d.polygon([(i, 0), (i + 22, 0), (i + 22 - SIZE, SIZE), (i - SIZE, SIZE)], fill=b)
        d.ellipse([170, 170, 430, 430], fill=a)
        d.ellipse([230, 230, 370, 370], fill=c)
    # a little grain so it reads as a print, not a flat vector
    noise = Image.effect_noise((SIZE, SIZE), 18).convert("RGB")
    return Image.blend(im, noise, 0.06)


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "test-albums"
    os.makedirs(out, exist_ok=True)
    tmp = tempfile.mkdtemp()
    for n, (album, artist, year, pal, style, titles) in enumerate(ALBUMS):
        folder = os.path.join(out, f"{artist} - {album}".replace("/", "-"))
        os.makedirs(folder, exist_ok=True)
        art = os.path.join(tmp, f"{n}.jpg")
        cover(style, pal, n).save(art, quality=88)
        for i, title in enumerate(titles, 1):
            dst = os.path.join(folder, f"{i:02d} {title.replace('/', '-')}.flac")
            secs = 20 + (n * 7 + i * 13) % 25
            # a soft tone that fades in and out, so playback is audible but not annoying
            freq = 220 * 2 ** (((n * 5 + i * 3) % 12) / 12)
            tone = (f"sine=frequency={freq:.1f}:sample_rate=44100:duration={secs},"
                    f"volume=0.05,afade=t=in:d=2,afade=t=out:st={secs - 3}:d=3")
            subprocess.run([
                "ffmpeg", "-loglevel", "error", "-y",
                "-f", "lavfi", "-i", tone, "-i", art,
                "-map", "0:a", "-map", "1:v", "-ac", "2", "-sample_fmt", "s16",
                "-c:a", "flac", "-c:v", "copy", "-disposition:v", "attached_pic",
                "-metadata:s:v", "comment=Cover (front)",
                "-metadata", f"title={title}", "-metadata", f"artist={artist}",
                "-metadata", f"album_artist={artist}", "-metadata", f"album={album}",
                "-metadata", f"date={year}", "-metadata", f"track={i}/{len(titles)}",
                dst,
            ], check=True)
        print(folder)


if __name__ == "__main__":
    main()

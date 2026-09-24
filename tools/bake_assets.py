#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Bake PNG + WAV assets into a single blob + header.

PNGs are quantized to 8-bit indexed with a 1 KB RGBA palette.  Slot 0 is
permanently reserved for transparent pixels; every other slot holds an
opaque RGBA triple.  WAVs are downmixed to mono S16 and resampled to
48 kHz with linear interpolation.
"""
import os
import sys
import wave
import struct
from PIL import Image

ASSETS = [
    ("bg_day",     "assets/background-day.png",                 "png"),
    ("bg_night",   "assets/background-night.png",               "png"),
    ("base",       "assets/base.png",                           "png"),
    ("pipe_top",   "assets/pipe-green-top.png",                 "png"),
    ("pipe_bot",   "assets/pipe-green.png",                     "png"),
    ("bird_down",  "assets/yellowbird-downflap.png",            "png"),
    ("bird_mid",   "assets/yellowbird-midflap.png",             "png"),
    ("bird_up",    "assets/yellowbird-upflap.png",              "png"),
    ("gameover",   "assets/gameover.png",                       "png"),
    ("sfx_jump",   "assets/jump.wav",                           "wav"),
    ("sfx_score",  "assets/score.wav",                          "wav"),
    ("sfx_hit",    "assets/hit.wav",                            "wav"),
]

FMT_RGBA8_INDEXED = 0
FMT_S16_MONO_48K  = 1

# Source alpha below this is treated as fully transparent.  Everything
# at or above is rendered fully opaque.  This is the correct treatment
# for pixel art: the sprites have hard edges, and the only semi-alpha
# in the source PNGs is anti-aliased fringe we want to snap opaque.
ALPHA_CUTOFF = 8


def resample_linear(samples, in_rate, out_rate=48000):
    """Linear-interpolate a list of ints from in_rate to out_rate."""
    if in_rate == out_rate or not samples:
        return samples
    n = len(samples)
    out_n = int(n * out_rate / in_rate)
    out = [0] * out_n
    ratio = in_rate / out_rate
    for i in range(out_n):
        pos = i * ratio
        i0 = int(pos)
        i1 = i0 + 1 if i0 + 1 < n else i0
        frac = pos - i0
        out[i] = int(samples[i0] + (samples[i1] - samples[i0]) * frac)
    return out


def read_wav_mono(path):
    """Return (samples_48k_s16_list).  Accepts any sample rate, any
    channel count, 16-bit PCM."""
    with wave.open(path, "rb") as w:
        nch = w.getnchannels()
        sw  = w.getsampwidth()
        fr  = w.getframerate()
        n   = w.getnframes()
        raw = w.readframes(n)

    if sw != 2:
        raise ValueError(f"{path}: expected 16-bit samples, got {sw*8}-bit")

    samples = list(struct.unpack("<" + "h" * n, raw[:2*n]))

    if nch == 2:
        samples = [(samples[i] + samples[i+1]) // 2 for i in range(0, n, 2)]
    elif nch != 1:
        raise ValueError(f"{path}: expected mono or stereo, got {nch}ch")

    if fr != 48000:
        samples = resample_linear(samples, fr, 48000)

    return samples


def bake_png(path):
    """Return (palette_bytes, indices_bytes, w, h).

    Palette format: 256 entries * (R,G,B,A) = 1024 bytes.
      - Slot 0 is permanently (0,0,0,0) — transparent.
      - Every other slot holds an opaque RGB triple with A=255.
    Indices format: w*h bytes, row-major.
    """
    img = Image.open(path).convert("RGBA")
    w, h = img.size
    px = list(img.getdata())

    palette = [(0, 0, 0, 0)]           # slot 0 = transparent
    lookup  = {}                       # (R,G,B,255) -> palette index

    indices = bytearray(w * h)

    for i, (r, g, b, a) in enumerate(px):
        if a < ALPHA_CUTOFF:
            indices[i] = 0
            continue

        key = (r, g, b, 255)
        idx = lookup.get(key)
        if idx is None:
            if len(palette) < 256:
                idx = len(palette)
                palette.append(key)
                lookup[key] = idx
            else:
                # Out of slots: snap to the nearest non-transparent
                # palette entry by squared RGB distance.
                best   = 1
                best_d = 1 << 30
                for j, (pr, pg, pb, pa) in enumerate(palette):
                    if pa == 0:
                        continue
                    dr = r - pr
                    dg = g - pg
                    db = b - pb
                    d  = dr*dr + dg*dg + db*db
                    if d < best_d:
                        best_d = d
                        best   = j
                        if d == 0:
                            break
                idx = best
        indices[i] = idx

    pal_bytes = bytearray()
    for entry in palette:
        pal_bytes.extend(entry)
    while len(pal_bytes) < 1024:
        pal_bytes.extend((0, 0, 0, 0))

    return bytes(pal_bytes), bytes(indices), w, h


def main():
    blob = bytearray()
    entries = []   # (enum_name, offset, w, h, fmt)

    for name, path, kind in ASSETS:
        if not os.path.isfile(path):
            print(f"[!] missing {path}", file=sys.stderr)
            sys.exit(1)

        if kind == "png":
            pal, idx, w, h = bake_png(path)
            data = pal + idx
            off = len(blob)
            blob += data
            entries.append((f"A_{name.upper()}", off, w, h, FMT_RGBA8_INDEXED))
            print(f"  {name:<11} PNG {w}x{h:<6} {len(data):>8} B")
        else:
            samples = read_wav_mono(path)
            data = struct.pack("<" + "h" * len(samples), *samples)
            off = len(blob)
            blob += data
            entries.append((f"A_{name.upper()}", off, 0, len(samples), FMT_S16_MONO_48K))
            print(f"  {name:<11} WAV 48000Hz  {len(samples)} frames  {len(data):>8} B")

    with open("src/assets.h", "w") as f:
        f.write("/* SPDX-License-Identifier: MIT */\n")
        f.write("/* generated by tools/bake_assets.py -- do not edit */\n")
        f.write("#ifndef ASSETS_H\n#define ASSETS_H\n\n#include \"core.h\"\n\n")
        f.write("enum asset_fmt {\n")
        f.write("    ASSET_FMT_RGBA8_INDEXED = 0,\n")
        f.write("    ASSET_FMT_S16_MONO_48K  = 1,\n")
        f.write("};\n\n")
        f.write("struct asset {\n")
        f.write("    u32 offset;\n")
        f.write("    u16 w;\n")
        f.write("    u16 h;\n")
        f.write("    u8  fmt;\n")
        f.write("    u8  _pad[3];\n")
        f.write("};\n\n")
        f.write("enum asset_id {\n")
        for i, (enum, *_rest) in enumerate(entries):
            f.write(f"    {enum} = {i},\n")
        f.write(f"    ASSET_COUNT = {len(entries)},\n")
        f.write("};\n\n")
        f.write("static const struct asset asset_table[ASSET_COUNT] = {\n")
        for enum, offset, w, h, fmt in entries:
            f.write(f"    {{ {offset}u, {w}u, {h}u, {fmt}u, {{0,0,0}} }}, /* {enum} */\n")
        f.write("};\n\n")
        f.write("#endif\n")

    with open("src/assets.bin", "wb") as f:
        f.write(blob)

    with open("src/assets.S", "w") as f:
        f.write("/* SPDX-License-Identifier: MIT */\n")
        f.write("/* generated by tools/bake_assets.py -- do not edit */\n")
        f.write("    .section .rodata\n")
        f.write("    .globl asset_blob\n")
        f.write("    .balign 16\n")
        f.write("asset_blob:\n")
        f.write("    .incbin \"src/assets.bin\"\n")
        f.write("    .globl asset_blob_end\n")
        f.write("asset_blob_end:\n")
        f.write("    .previous\n")

    print(f"\nbaked {len(entries)} assets, {len(blob)} bytes total")


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Bake assets for the Flappy payload.

Reads assets/*.png and assets/*.wav, writes:
  src/assets.h     — enum + struct table
  src/assets.bin   — packed blob
  src/assets.S     — assembly wrapper around the blob

Image format on disk (per asset):
  1024 bytes    palette, 256 entries x (R, G, B, A)
  w*h bytes     index into palette, row-major

Audio format on disk:
  h*2 bytes     s16 little-endian mono samples at 48 kHz
"""
import os
import struct
from PIL import Image
import wave

ASSETS_DIR = "assets"
SRC_DIR    = "src"

IMAGE_ASSETS = [
    ("bg_day",    "background-day.png",      "A_BG_DAY"),
    ("bg_night",  "background-night.png",    "A_BG_NIGHT"),
    ("base",      "base.png",                "A_BASE"),
    ("pipe_top",  "pipe-green-top.png",      "A_PIPE_TOP"),
    ("pipe_bot",  "pipe-green.png",          "A_PIPE_BOT"),
    ("bird_down", "yellowbird-downflap.png", "A_BIRD_DOWN"),
    ("bird_mid",  "yellowbird-midflap.png",  "A_BIRD_MID"),
    ("bird_up",   "yellowbird-upflap.png",   "A_BIRD_UP"),
    ("gameover",  "gameover.png",            "A_GAMEOVER"),
]

AUDIO_ASSETS = [
    ("sfx_jump",  "jump.wav",                "A_SFX_JUMP"),
    ("sfx_score", "score.wav",               "A_SFX_SCORE"),
    ("sfx_hit",   "hit.wav",                 "A_SFX_HIT"),
]

# Pixels with source alpha below this are treated as fully transparent.
ALPHA_CUTOFF = 8


def quantize_rgba(img):
    """RGBA image -> (palette_bytes, indices_bytes).

    Index 0 is permanently reserved for transparent pixels.  Every other
    slot is keyed on exact RGB with alpha forced to 255, so solid white
    and transparent white never share a slot.  This is what fixes the
    "blank spots" on the pipe highlight and the bird's eye/beak.

    If the image has more than 255 unique opaque colours, subsequent
    pixels snap to the nearest non-transparent entry by squared RGB
    distance.
    """
    img = img.convert("RGBA")
    w, h = img.size
    px = list(img.getdata())

    palette = [(0, 0, 0, 0)]           # slot 0 = transparent
    lookup  = {(0, 0, 0, 255): 0}      # sentinel so nothing ever maps to 0

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

    return bytes(pal_bytes), bytes(indices)


def read_wav_s16(path):
    with wave.open(path, "rb") as w:
        nch = w.getnchannels()
        sw  = w.getsampwidth()
        fr  = w.getframerate()
        n   = w.getnframes()
        raw = w.readframes(n)

        if sw != 2:
            raise ValueError(f"{path}: expected 16-bit samples, got {sw*8}-bit")
        if fr != 48000:
            raise ValueError(f"{path}: expected 48000 Hz, got {fr}")

        samples = struct.unpack("<" + "h" * n, raw[:2*n])
        if nch == 2:
            samples = tuple(
                (samples[i] + samples[i+1]) // 2 for i in range(0, n, 2)
            )
        elif nch != 1:
            raise ValueError(f"{path}: expected mono or stereo, got {nch}ch")
        return list(samples)


def main():
    os.makedirs(SRC_DIR, exist_ok=True)

    blob    = bytearray()
    entries = []   # (enum_name, offset, w, h, fmt)

    FMT_RGBA = 0
    FMT_S16  = 1

    for name, fname, enum in IMAGE_ASSETS:
        path = os.path.join(ASSETS_DIR, fname)
        img  = Image.open(path)
        w, h = img.size
        pal, idx = quantize_rgba(img)
        data = pal + idx

        offset = len(blob)
        blob.extend(data)
        entries.append((enum, offset, w, h, FMT_RGBA))
        print(f"  {name:<12} PNG {w}x{h:<6} {len(data):>8} B")

    for name, fname, enum in AUDIO_ASSETS:
        path = os.path.join(ASSETS_DIR, fname)
        samples = read_wav_s16(path)
        data = struct.pack("<" + "h" * len(samples), *samples)
        offset = len(blob)
        blob.extend(data)
        entries.append((enum, offset, 0, len(samples), FMT_S16))
        print(f"  {name:<12} WAV 48000Hz  {len(samples)} frames  {len(data):>8} B")

    with open(os.path.join(SRC_DIR, "assets.bin"), "wb") as f:
        f.write(blob)

    with open(os.path.join(SRC_DIR, "assets.h"), "w") as f:
        f.write("/* Auto-generated by tools/bake_assets.py - do not edit. */\n")
        f.write("#ifndef ASSETS_H\n#define ASSETS_H\n\n")
        f.write('#include "core.h"\n\n')
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

    with open(os.path.join(SRC_DIR, "assets.S"), "w") as f:
        f.write("/* Auto-generated by tools/bake_assets.py - do not edit. */\n")
        f.write("    .section .rodata\n")
        f.write("    .globl asset_blob\n")
        f.write("    .balign 16\n")
        f.write("asset_blob:\n")
        f.write("    .incbin \"src/assets.bin\"\n")
        f.write("    .globl asset_blob_end\n")
        f.write("asset_blob_end:\n")
        f.write("    .previous\n")

    with open(os.path.join(SRC_DIR, ".assets.stamp"), "w") as f:
        f.write("ok\n")

    print(f"\nbaked {len(entries)} assets, {len(blob)} bytes total")


if __name__ == "__main__":
    main()

#!/usr/bin/env bash
set -e

# ---------- Assets ----------
# Auto-fetch assets if the directory is empty
if [ ! -f assets/background-day.png ]; then
  echo "Assets missing — fetching from MexrlDev/PsVue-Mod..."
  ./tools/fetch_assets.sh assets
fi

# Sanity check — every source asset must be present
missing=0
for f in background-day.png background-night.png base.png \
         gameover.png hit.wav jump.wav \
         pipe-green-top.png pipe-green.png score.wav \
         yellowbird-downflap.png yellowbird-midflap.png yellowbird-upflap.png; do
  [ -f "assets/$f" ] || { echo "MISSING assets/$f"; missing=1; }
done
[ "$missing" -eq 0 ] || exit 1

# ---------- Font atlas ----------
# If the AA font atlas isn't checked in, bake it now (needs Pillow + a TTF).
if [ ! -f src/font_aa.bin ] || [ ! -f src/font_aa_metrics.h ]; then
  echo "Font atlas missing — baking from system TTF..."
  python3 tools/bake_font.py
fi

# ---------- Asset table ----------
# Generates src/assets.h, src/assets.bin, src/assets.S from the raw assets.
echo "Baking assets..."
python3 tools/bake_assets.py

# ---------- Build ----------
make clean
make -j"$(nproc)"
make hex

echo
echo "Built flappy.bin ($(stat -c%s flappy.bin 2>/dev/null || stat -f%z flappy.bin) bytes)"

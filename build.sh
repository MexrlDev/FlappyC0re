#!/usr/bin/env bash
set -e

# Auto-fetch assets if the directory is empty
if [ ! -f assets/background-day.png ]; then
  echo "Assets missing — fetching from MexrlDev/PsVue-Mod..."
  ./tools/fetch_assets.sh assets
fi

# Sanity check
missing=0
for f in background-day.png background-night.png base.png \
         gameover.png hit.wav jump.wav \
         pipe-green-top.png pipe-green.png score.wav \
         yellowbird-downflap.png yellowbird-midflap.png yellowbird-upflap.png; do
  [ -f "assets/$f" ] || { echo "MISSING assets/$f"; missing=1; }
done
[ "$missing" -eq 0 ] || exit 1

make clean
make -j"$(nproc)"
make hex

echo
echo "Built flappy.bin ($(stat -c%s flappy.bin 2>/dev/null || stat -f%z flappy.bin) bytes)"

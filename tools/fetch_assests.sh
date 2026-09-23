#!/usr/bin/env bash
# Fetch the original Flappy Bird assets from MexrlDev/PsVue-Mod.
# All assets are MIT-licensed (samuelcust/flappy-bird-assets).
set -e

BASE="https://raw.githubusercontent.com/MexrlDev/PsVue-Mod/main/Flappy%20Bird/bird"
DEST="${1:-assets}"

mkdir -p "$DEST"

FILES=(
  background-day.png
  background-night.png
  base.png
  gameover.png
  hit.wav
  jump.wav
  pipe-green-top.png
  pipe-green.png
  score.wav
  yellowbird-downflap.png
  yellowbird-midflap.png
  yellowbird-upflap.png
  Credits.txt
)

echo "Fetching Flappy Bird assets into $DEST/"
for f in "${FILES[@]}"; do
  url="$BASE/$f"
  out="$DEST/$f"
  if [ -f "$out" ]; then
    echo "  skip  $f"
    continue
  fi
  if curl -fsSL "$url" -o "$out"; then
    sz=$(stat -c%s "$out" 2>/dev/null || stat -f%z "$out")
    printf "  fetch %-28s %8d B\n" "$f" "$sz"
  else
    echo "  FAIL  $f  ($url)"
    exit 1
  fi
done

echo
echo "Done. $(ls -1 "$DEST" | wc -l) files in $DEST/"

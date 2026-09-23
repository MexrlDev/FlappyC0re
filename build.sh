#!/usr/bin/env bash
set -e
mkdir -p assets
if [ ! -f assets/background-day.png ]; then
  echo "Please put your Flappy Bird PNGs and WAVs into ./assets/"
  echo "Required files:"
  echo "  background-day.png  background-night.png  base.png"
  echo "  pipe-green-top.png  pipe-green.png"
  echo "  yellowbird-downflap.png  yellowbird-midflap.png  yellowbird-upflap.png"
  echo "  gameover.png"
  echo "  jump.wav  score.wav  hit.wav"
  exit 1
fi
make clean
make -j"$(nproc)"
make hex
echo
echo "Built flappy.bin ($(stat -c%s flappy.bin) bytes)"

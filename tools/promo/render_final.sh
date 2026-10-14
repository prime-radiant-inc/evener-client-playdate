#!/bin/sh
set -eu
cd "$(dirname "$0")"
PROMO_PYTHON=${PROMO_PYTHON:-python3}
"$PROMO_PYTHON" compose_score.py
"$PROMO_PYTHON" render_promo.py --manifest footage.json \
  --audio a-little-closer-original-score.wav --out evener-playdate-promo.mp4
"$PROMO_PYTHON" render_promo.py --manifest footage.json \
  --frames 3.5 10 18 20.8 26 33 41 47 54 --out proofs-final
cp proofs-final/frame-03.50.png poster.png
ffmpeg -v error -i evener-playdate-promo.mp4 -f null -
ffprobe -v error -show_entries format=duration,size \
  -show_entries stream=codec_name,width,height,r_frame_rate,sample_rate,channels \
  -of json evener-playdate-promo.mp4 > final-media-info.json

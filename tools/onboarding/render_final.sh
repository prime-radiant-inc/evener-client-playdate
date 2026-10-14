#!/bin/sh
set -eu
cd "$(dirname "$0")"
ONBOARDING_PYTHON=${ONBOARDING_PYTHON:-python3}
mkdir -p build
"$ONBOARDING_PYTHON" render_onboarding.py --manifest scenes.json \
  --out build/onboarding-render.mp4
ffmpeg -y -v error -i build/onboarding-render.mp4 \
  -f ffmetadata -i build/onboarding-render.chapters.ffmetadata -i captions.en.srt \
  -map 0:v:0 -map 0:a:0 -map 2:s:0 -map_metadata 1 -map_chapters 1 \
  -c:v copy -c:a copy -c:s mov_text -metadata:s:s:0 language=eng \
  -movflags +faststart build/evener-playdate-onboarding.mp4
ffmpeg -y -v error -ss 2 -i build/evener-playdate-onboarding.mp4 \
  -frames:v 1 build/onboarding-poster.png
ffmpeg -v error -i build/evener-playdate-onboarding.mp4 \
  -map 0:v:0 -map 0:a:0 -f null -
ffprobe -v error -show_streams -show_chapters -show_format -of json \
  build/evener-playdate-onboarding.mp4 > build/media-info.json

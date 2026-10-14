# Evener for Playdate — A little closer

Editable production source for the 58-second Evener for Playdate film. The output is 1920 × 1080 at 60 fps, H.264 with stereo AAC. All application screen imagery is selected from real physical-device recordings made through the official Playdate Mirror application. The housing, crank, sound waves, connection diagram, typography and music are original production graphics and sound design.

## Render

Use Python 3.12, FFmpeg, Pillow and NumPy. Exact production versions are listed in `render-environment.json` and `requirements.txt`.

```sh
python3 -m venv .venv
.venv/bin/pip install -r requirements.txt
PROMO_PYTHON=.venv/bin/python3 ./render_final.sh
```

The artwork uses the Avenir Next font collection supplied by macOS at `/System/Library/Fonts/Avenir Next.ttc`. The font is not redistributed. To render on another system, provide that font under its own license and update the `FONT` setting in `render_promo.py`. The font collection's weight indices are declared in the `font` function.

The render needs no running Hub, device, configured application bundle, provider credential or network request. It streams frames to FFmpeg with bounded memory. `footage.json` and the brand asset use paths relative to this source folder.

## Edit

- `render_promo.py`: palette, typography, illustrated device, choreography and compositing.
- `compose_score.py`: original 120 BPM score, synthesis, percussion and musical chirps. No samples or external music are used.
- `footage.json`: the five selected display clips and the voice recording/release cue.
- `inputs/`: lossless RGB H.264 clips at 400 × 240 and 60 fps. Their decoded RGB frames match the corresponding raw-source pixels after the declared nearest-neighbor downscale.
- `inputs/PROVENANCE.json`: original recording names and hashes, exact source ranges and what each range shows.
- `inputs/PIXEL-VERIFICATION.json`: per-clip verification counts and selected-input hashes.
- `assets/primeradiant-logo.png`: the supplied official Prime Radiant mark.
- `on-screen-titles.srt`: optional accessibility transcript of the motion titles. The film has no spoken narration.
- `STORYBOARD.md`: editorial sequence and visual intent.

The package contains only the selected safe excerpts. It excludes full raw recordings, configured bundles and private application data.

## Capture timing

Mirror records sparse variable-frame-rate video: an unchanged screen can remain visible for a long time with no newly encoded frame. Source conversion normalizes that presentation timing before trimming. It never input-seeks past the frame that owns an unchanged interval. Constant-frame-rate duplicates preserve actual held screens; no UI motion is interpolated. The selected clips retain their original speed.

The setup chapter shows **Receive setup / Received: 0% / Listening**. It does not show receipt of an invitation or completed onboarding. The short three-note sounds in the score are designed musical effects, not the Hub's encoded setup sound. The voice chapter uses a separate unsent demonstration take; review, send and reply imagery come from the canonical successful take. The film is an edited montage of those actual captures and makes no latency promise.

The device illustration is not a photograph. Display content is never redrawn or rewritten. Screens use nearest-neighbor scaling; the introduction and hero also rotate the complete device image slightly, while the reading and input sequences remain square to the camera.

The final title identifies the product as an **experimental native client**. The source does not claim production readiness or completed fresh onboarding.

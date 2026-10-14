# Evener for Playdate — recorded onboarding walkthrough

Editable source for the [2 minute 34 second walkthrough](../../docs/media/evener-playdate-onboarding.mp4): 1920 × 1080, 30 fps, H.264 with stereo AAC, selectable English captions and 15 chapters. The film covers private installation, receiving the setup sound, Tailscale enrollment instructions, matching the complete Hub code, saving the destination, and a recorded voice/review/send/reply exchange.

Application imagery comes from actual physical Playdate and authenticated Hub captures. The display has a simple frame; it is not an illustration of the device housing. Title cards and instructions are editorial graphics. The Prime Radiant logo is the [official mark](https://primeradiant.com/brand/logo.png).

## What was recorded

The film combines several recorded takes, with waits edited. It is not a continuous enrollment recording or a latency measurement.

- The setup sound is the actual room recording from the production receiver take. Both complete audible blocks are included. Its timing is aligned against the device microphone meter and the browser's playing state, to approximately 0.12 seconds. Narration ends before the sound begins.
- That setup take ends before its comparison code appears. The matching-code and approval chapter comes from an earlier recorded enrollment and is explicitly labeled that way. The temporary sound-test invitation was cancelled; it did not replace the approved destination.
- Tailscale authorization was completed on a phone off camera. No genuine phone or sign-in QR capture was available. This step is an instruction card labeled **PHONE APPROVAL NOT RECORDED**; no phone interface or QR is reconstructed.
- Recording, review, send and reply come from the dedicated demonstration conversation. The final reply is “small screen big ideas”. Earlier “maple lantern” test messages visible while scrolling belong to the same demonstration conversation.
- Narration uses the macOS Samantha synthesized voice. The speech originally dictated into the demonstration recording was also synthesized. Both are disclosed in the film. The setup sound is not synthesized or replaced.

The selected inputs exclude raw recordings, unrelated conversations and session lists, long-lived mobile connection QR material, configured app bundles, enrollment identities, and diagnostic receiver screens. The only supplied microphone audio is the short setup-sound excerpt, including the quiet interval immediately around it. Captured video audio is muted.

The recorded successful exchange does not establish reliable delivery under every network condition. The [client README](../../README.md#on-the-playdate) describes current SDK network stalls and incomplete recovery.

## Render

Use Python 3.12, Pillow and FFmpeg. Production versions are in `render-environment.json`; Python dependencies are pinned in `requirements.txt`.

```sh
python3 -m venv .venv
.venv/bin/pip install -r requirements.txt
ONBOARDING_PYTHON=.venv/bin/python3 ./render_final.sh
```

The result and poster are written to the ignored `build/` directory. Rendering needs no Hub, device, private configuration, credential or network request. The macOS Avenir Next font collection is used at `/System/Library/Fonts/Avenir Next.ttc`; it is not redistributed. On another platform, provide the font under its license and update `FONT` in the renderer.

The final script renders the film, adds the supplied captions and chapters without re-encoding video or audio, extracts its poster, and decodes the complete result as a media-integrity check. Encoded bytes can vary with tool versions; selected input hashes and decoded-pixel verification are recorded independently.

## Edit and inspect

- `scenes.json` is the ordered timeline, with relative media paths, durations, titles, instructions and local audio offsets. `edited_waits` displays a visible timing disclosure.
- `render_onboarding.py` composites real media with Pillow and streams bounded frames to FFmpeg. It accepts `title`, `device`, `browser` and `split` layouts.
- `sources/` contains only the selected excerpts and crops. Videos use lossless RGB H.264; entire decoded streams were verified identical to the lossless editing intermediates. No application pixels are reconstructed.
- `narration/` supplies the synthesized AIFF tracks and their editable text. `NARRATION.md` provides the timed script. Reusing supplied tracks preserves the reviewed timing; regenerating a system voice may change it.
- `captions.en.srt` contains sentence-timed narration and the actual-sound cue; update it when changing scene timing. Chapters are generated from the scene timeline for each render. The checked-in `chapters.ffmetadata` is the delivered film's review snapshot. Renderer-generated `*.narration-draft.srt` files are only scene-wide editing aids.
- `PROVENANCE.json` records capture names, hashes, trims, crops, sound alignment and editorial boundaries. `SOURCE-VERIFICATION.json` records complete decoded-stream equality. `VERIFICATION.json` records checks of the delivered film.

```sh
# Validate all referenced inputs and scene durations.
.venv/bin/python3 render_onboarding.py --manifest scenes.json --validate

# Review exact layouts without encoding the entire film.
.venv/bin/python3 render_onboarding.py --manifest scenes.json \
  --frames 27 47.4 62 77 124 140 --out build/proofs
```

Mirror captures are sparse: unchanged screens may have no new encoded frame for many seconds. Normalize to the target frame rate **before** trimming. Input-side seeking into raw Mirror media can jump to a later update. Selected clips already preserve the original presentation timing. Device imagery uses nearest-neighbor scaling at its 5:3 aspect ratio. Browser crops retain their aspect ratio and are letterboxed as needed.

Each media object accepts `path`, `in`, optional `duration`, `speed`, optional `[x, y, width, height]` `crop`, and `hold_last`. A final device scene requires actual video. Source overruns fail unless `hold_last` is explicitly true; such a freeze receives a visible label. No scene in this delivered film overruns its source.

Audio is explicit: a scene's `audio` entries use local `at` times, optional input trim/duration and `gain_db`. Top-level tracks use absolute film time. Tracks are mixed at the declared gain, peak-limited and encoded as 48 kHz stereo AAC. There is no automatic narration ducking; keep the setup sound clear when editing.

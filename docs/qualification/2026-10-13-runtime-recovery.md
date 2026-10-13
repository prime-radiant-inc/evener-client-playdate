# Physical runtime recovery and voice qualification, 2026-10-13

## Build and preservation

- Client source: `7e8e389ff2061993de5af49b0cad682dd72fe7ac`.
- Device executable SHA-256:
  `d35692425400c91c75bc3284e72cab732063d1f8401b10e50a0a6eacae5a078c`.
- Physical Playdate, SDK/firmware 3.1.2, native Tailscale and direct OpenAI TLS.
- The installation copied only `pdex.bin` and `pdex.dylib` into the existing
  bundle. All 62 saved-data files were backed up and remained byte-identical
  through installation. Existing destination settings and identity were retained.
- No Hub/server, radio configuration or firmware change.

This run exercises the revised application teardown guard described in the
[earlier investigation](2026-10-12-sdk-tcp.md#runtime-recovery-integration)
only to the boundaries observed below. It does not establish that the underlying
SDK duplex failure has been repaired.

## Observed behavior

| Scenario | Physical result |
| --- | --- |
| Reported group conversation | Loaded seven chat messages and completed control maintenance |
| Natural SDK worker stall | A DERP write remained at 13 pending bytes with SDK error zero; the adapter reported its write-progress timeout |
| Automatic runtime recovery | The app requested one restart, restored the same view, completed native control/DERP/WireGuard connection and read the same seven messages again |
| Dictation during control maintenance | The microphone started synchronously; the transcription connection was requested 45 ms later, and the first caption appeared at 8.168 s during a 15.051 s hold |
| Interrupted transcription | A final-response timeout retained all 120,279 recorded bytes; one ordinary transcription retry replayed the audio and produced the full review text 58.394 s after release |
| Separate send after review | One newly accepted user message and its expected reply were read back from the Hub |
| Dictation alongside established DERP | DERP socket 18 stayed connected while OpenAI socket 19 transcribed; the first caption appeared at 5.807 s during a 15.014 s hold |
| Prompt finalization | The second recording's final review text arrived 0.794 s after release; the OpenAI socket then retired and maintenance resumed |
| Second separate send | The distinct second test message and its expected reply were read back with a separate mutation identity |

The two test recordings used spoken fixtures played through the MacBook speakers
into the physical microphone. Screens captured captions before release and the
review state before a separate send press. The Hub reads contained one accepted
record for each new test message and its corresponding reply; no duplicate
submission was observed. The first send received `yellow comet`, and the distinct
second send received `silver otter`.

Across the recorded sequences, the client logged thirteen completed control-map
refreshes and fourteen authoritative conversation reads. The revised automatic
restart did not show the repeated termination from the initial failing build.
Only **one natural stalled-worker restart** occurred in this observation; normal
control maintenance and a transcription retry are separate recovery paths.

## Remaining boundaries

The first transcription retry incurred nearly a minute of finalization delay.
Successful replay protects the recording but does not make that delay acceptable
as a final latency target. Direct dictation with the established relay completed
quickly in this run; that does not guarantee every network attempt will do so.

The initial build's firmware allocator fault remains an observed regression with
an incompletely established cause. Removing application teardown after requesting
restart addresses a concrete ownership interaction. One successful natural restart
and subsequent voice/send cycles support that change, but do not qualify repeated
stalled-worker restarts or prove the origin of the earlier allocator corruption.

USB disappeared before the final crash-log read and diagnostic-app cleanup. The
captured sequences contain no observed reboot, but the post-run crash log has not
yet been checked. The candidate remains installed; temporary diagnostic bundles
remain until that cleanup can be completed. The pull request remains a draft
pending this final device check and stronger repeated-recovery evidence.

Private saved-data backups, configured binaries and raw captures stay outside
Git. Host storage/socket/recovery checks and ARM/Simulator build evidence are
recorded in the earlier investigation; no source code changed for this run.

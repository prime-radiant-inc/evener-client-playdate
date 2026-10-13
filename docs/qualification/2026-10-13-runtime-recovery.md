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

The pull request remains a draft pending stronger repeated-recovery evidence.

## Final device check

After USB reconnected, the post-run crash and error logs were byte-identical to
the pre-run copies (6,880 and 814 bytes respectively). Neither recorded a new
entry during the qualification sequences. The latest 62 saved-data files were
backed up with matching hashes. Removing the temporary `RawTCP` and
`NetworkInspect` diagnostic bundles left those saved files and the installed
Evener executable unchanged.

After safe ejection, Evener completed native control, DERP, WireGuard and Hub
connection and returned to its normal connected session list. The revised
candidate remains installed with the current pairing, drafts, recordings and
cache preserved. No client source, Hub/server, radio configuration or firmware
changed during this final check.

Private saved-data backups, configured binaries and raw captures stay outside
Git. Host storage/socket/recovery checks and ARM/Simulator build evidence are
recorded in the earlier investigation; no source code changed for this run.

## Follow-up: repeated SDK recovery

A later isolated SDK investigation reproduced a callback into unloaded game code
in Simulator 3.1.2. Its hardware callback control completed five ordinary runtime
restarts. Repeated blocked-write recovery failed separately on physical firmware:
one full-MiB recovery passed, then the second attempt faulted in the firmware
allocator. A guarded probe that stopped post-request rendering and system-event
work produced the same pass-then-fault sequence. See the [minimal reproductions,
registers and diagnosis boundaries](sdk-network-report.md).

These failures came from separate credential-free diagnostic apps. The installed
Evener executable and all 62 saved-data files were unchanged through the probes.
They supersede the earlier open question about qualification: repeated forced
runtime recovery has failed, so the workaround remains experimental and the PR
remains a draft. The earlier successful client voice/send observations still
stand, but do not establish recovery reliability.

## Post-probe client run

The unchanged installed client returned to the dedicated verification
conversation and completed native control, DERP, WireGuard and a fresh Hub read.
A DERP write then stalled at 117 pending bytes before the new recording began.
The microphone still started synchronously; the voice connection was requested
35 ms later with 232 samples already captured. After a 16.016-second hold, the
client saved the whole recording and performed one automatic runtime restart.
It restored the composer and replayed all 127,988 audio bytes. This run did not
show captions during the original hold; its first caption came after replay.

The provider eventually produced final text and the client durably saved a
`DRAFT_READY` record. Its on-disk checksum, version and state were checked after
the run. The text did not reproduce the entire spoken fixture, so this is not
a transcription-fidelity pass. The audio was removed only after final text had
been saved. No send button was pressed, and authoritative Hub readback was
unchanged: no partial or duplicate message was submitted.

The client subsequently added a firmware allocator fault at UTC 16:55:40:
PC `0x080619c4` (bundled hardware-ID-0 symbols: `malloc_consolidate`), LR zero,
BFAR `0x0000ac88`. Complete screen captures stopped arriving. This is a failed
client run even though the draft survived. The captured allocator site alone
does not establish the corruption origin or prove it is the same fault as the
standalone blocked-write failure.

The latest 62 saved-data files, including the finalized test draft, were backed
up and hash-verified. The error log remained unchanged; the crash log contains
the two standalone faults and this client fault. At this checkpoint macOS's
locked loginwindow refused safe ejection, so reopening the surviving review and
its separate-send check remain pending an unlock. The device is safely mounted;
no forced ejection, credential reset, firmware change or server change was used.

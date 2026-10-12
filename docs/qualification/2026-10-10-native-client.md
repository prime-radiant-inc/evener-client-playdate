# Native Playdate client qualification

This is the historical qualification record from the Evener source repository.
Commit IDs and `tools/playdate-client/` paths refer to that source tree; private
captures stay in the original local worktree and are not distributed here. The
standalone layout is documented in [source provenance](../provenance.md).

## Status

The qualified client source is `168de9dfac`, including socket ownership
corrections in `b1228da884` and acoustic pairing changes in `ae216f3f44`.
The browser-to-microphone invitation, matching approval codes, reviewed save,
and authenticated connection to an isolated generic Hub passed on hardware.
The original Hub profile was restored without replacing identity or draft data.

The ordinary bundle is installed and hash verified. Its binary SHA-256 is
`7c19a3ecf994bddc7499748cf4e8d3b8243de7f7827f8bfe9f21f2138d1ba18a`.
The fresh physical run passed immediate microphone capture, live captions,
durable review, separate send, deliberately lost receipt, accepted-mutation
reconciliation and a new reply rendered on the transcript. The Hub independently
confirmed exactly one accepted item for that mutation. Automatic maintenance
after the reply reconnected and reloaded the conversation. Firmware crash and
error logs were unchanged. The ordinary app subsequently loaded the larger
conversation that had failed before the polling change.

Earlier routine refreshes reproduced stalled SDK writes in both the qualification
wrapper and ordinary client. Commit `b1228da884` fixed command ownership but did
not alone resolve those failures. A receive-resume diagnostic then sent the
saved draft, reconciled deliberate receipt loss, rendered the reply and completed
two further maintenance/reload cycles. The equivalent client polling change
passed the fresh run above. Its boundary case fails against the prior adapter
and passes under ASan/UBSan; independent review found no ownership regression.
These runs establish the exercised device behavior, not the unavailable radio
firmware's internal wait condition or cancellation of every possible SDK stall.
The product guide retains that API limit. The production Hub service and its
configuration were not changed; generic pairing was tested against an isolated
Hub and has not been deployed to production here.

The native transport, authenticated Tailscale maintenance, session/chat surface,
immediate microphone capture, durable audio, review and mutation recovery are
implemented. The client directory contains its runtime dependencies; initial
provisioning uses the isolated enrollment helper in `tools/playdate-derp/enroll`.
Credentials and private bundles remain ignored by Git.

## Confirmed physical-device observations

- The native Playdate opened the dedicated Evener verification conversation and
  read its eight chat items through its own DERP/WireGuard/lwIP connection.
  This succeeded after stable Tailscale discovery advertisement and authenticated
  DERP discovery replies were integrated. A read-only observation on the existing
  Hub peer showed the Playdate in both the networking engine and magicsock, with a
  fresh WireGuard handshake and increasing received/transmitted byte counts.
- Session navigation and cached transcript bubbles remained readable while later
  native connections failed. The physical display retained its compact inverse
  header, inverse user bubbles, outlined assistant bubbles and one-line footer.
- An actual microphone press started capture before network work. The connection
  attempt began 23 ms later with 92 samples already saved. Releasing at 2,184 ms
  stopped capture before transcription connected; the file grew to 17,414 mu-law
  bytes after the remaining callback samples drained.
- That recording stayed at 17,414 bytes through transcription connection timeouts,
  repeated native connection failures and an explicit Wi-Fi radio reset. A later
  completed app relaunch recovered all 17,414 bytes. The offline-first device
  harness sent that entire recording and received the final transcription event
  7.63 seconds after recovery began.
- A fresh physical microphone capture lasting 45.051 seconds saved and sent all
  360,048 mu-law bytes, including samples appended after the initial reader
  snapshot. The first audio was queued 8.68 seconds after capture began; the
  commit was queued 32 ms after release and the final event arrived at 46.005
  seconds from capture start. This establishes complete sustained byte delivery,
  including capture before the transcription connection was ready.
- The Mac speakers were muted during those recovered and fresh recording checks,
  and both final transcripts were empty. The original short recording measured
  RMS 13 and peak 56, confirming a quiet input. Logged first/final-caption events
  therefore establish provider protocol completion, not intelligible speech,
  live captions or final review text. These limits apply to those earlier runs;
  a later audible-input run demonstrated both live captions and saved text.
- The later normal-native audible-input harness started the microphone
  synchronously; the transcription network attempt began 29 ms later with 185
  samples already saved. The first live caption arrived at 5.350 seconds while
  A remained held. Capture lasted 45.067 seconds and sent all 360,187 mu-law bytes.
  Final transcription arrived at 45.693 seconds from capture start and the
  device durably saved the following exact 100-character review:

  > Playdate native end to end chat. Please reply with yellow comet chat. Please reply with yellow comet

  The harness held that review for at least five seconds before a separate A
  requested the send. Automatic maintenance at 83.33 seconds retired the DERP
  connection with 585 bytes pending, drained them before close, refreshed the
  authenticated map and restored native Hub reading.
- The first send harness forced a reconnect at 117.25 seconds while the draft was
  SENDING but before dispatch. After reconnect, user mutation
  `111c1b690159a8a61f166479f0dc96e3` was accepted and appeared as one new user
  message. This is preserved-input/pre-dispatch recovery evidence; it does not
  test losing the receipt for an already accepted mutation. A later authoritative
  Mac Hub read shows the assistant reply `yellow comet`, after approximately
  190 seconds of provider wait. The first harness left the game before the
  device observed that reply, so this is Hub reply evidence, not a completed
  physical-device reply-display check.
- Authenticated Tailscale map refresh and native Hub reading passed repeatedly.
  Two maintenance reconnect cycles completed with zero pending bytes at close.
  Earlier maintenance attempts stalled after retiring sockets with 468 or 1,417
  bytes initially pending. The later one-outstanding-write/drain-before-close
  policy recovered the physical 585-byte pending-close case above. Broader
  reliability and the terminal asynchronous-send-error path still require
  physical qualification.
- The second normal-native run used the committed adapter's terminal-send-error
  handling as well. Its microphone started before networking, the first caption
  arrived at 4.893 seconds while held, and a 45.068-second capture sent all
  360,187 audio bytes. The final event arrived at 45.894 seconds and saved a
  127-character review containing both spoken repetitions of the test phrase.
  This run also read ten existing chat items, including the first run's reply.
- The USB harness requested an immediate same-app relaunch after that durable
  review, before the separate send action. Termination reported zero pending
  audio and complete saving. The new process logged its first control connection
  open with Wi-Fi not connected, then made no further observable progress.
  USB stats later reported zero completed frames; USB help/stats still responded.
  A partial screen capture was black. The blocking SDK call and cause are not
  established. The saved draft's successful reload and this second run's send
  remain unverified. A storage-mode transition exposed the draft files, but the
  device disconnected before a fresh backup/readback completed.
- USB data storage is accessible again. App/data backups preserve recordings,
  drafts, caches and device diagnostics. No new firmware crash entries appeared
  through `device-backup-composer-oct11/`; this observation covers those runs
  and is not a general crash-free qualification.
- A Mac WebSocket check with the configured transcription key reached
  `session.created`. This confirms credential/session availability independently
  of the SDK connection lifecycle; it is not device evidence.

The dedicated session is `local:034dqGiFzThCtfD05eE8M3`, titled
“Playdate client verification.” The new native message and Hub reply described
above belong to that session. Earlier user-confirmed LAN-client sends remain
separate evidence and do not qualify the native transport integration.

## Defects found and fixed

1. Native map requests omitted the stable discovery key, causing the existing
   Tailscale peer to exclude the Playdate from its networking engine. Control now
   advertises and verifies that identity; transport authenticates and answers the
   selected peer's DERP discovery pings. Physical native Hub reading then worked.
2. The SDK caches file size when opening a reader. The long physical capture saved
   559,368 bytes but the old reader sent only its initial 35,294-byte snapshot,
   losing later speech from the transcription request. Commit `969b4c4386` reopens
   exhausted snapshots as the writer grows and prevents final commit until every
   saved sample has been sent. The corrected code is installed and the physical
   captures sent all 360,048 quiet-input bytes and then all 360,187 audible-input
   bytes. The later run also produced live captions and durable review text.
3. Termination originally drained only one 800-byte batch. It now drains all
   accepted callback samples before closing, with bounded individual writes.
4. A failed first map-state commit could publish readiness while retaining the
   old enrollment snapshot. Commit `16bfd7c075` installs the configuration and
   readiness together only after successful persistence.
5. Empty text drafts could still contain retained audio while the conversation
   promised Hold A Dictate; the composer also labelled pending transcription as
   Review message. Commit `a6df2754b8` distinguishes recording, transcription and
   paused dictation, shows errors in the body, and labels retained input
   A Transcribe saved audio. The indicator is cached at input/navigation
   transitions without disk polling during drawing. Waiting copy does not
   promise that RAM samples have already been durably saved. Recovery remains
   first, and sending still requires a separate A press after final review.
   Microphone setup failure is visible and retryable without creating an empty
   audio file, while any samples accepted before failure remain recoverable.
   Switching drafts clears stale voice state and status. Commit `00dd615b4b`
   labels an empty fresh composer Dictate message, reserving Review message for
   actual saved text.
6. Closing SDK connections with pending writes could report success while never
   releasing the retired connection. The adapter now permits one outstanding
   write and drains pending writes before closing. The physical maintenance case
   beginning with 585 pending bytes completed close, authenticated map refresh
   and Hub reconnection. A further terminal asynchronous-send-error fix has an
   offline failing-then-passing behavior case; it has not been fault-injected in
   firmware. These results do not establish that every SDK close failure is fixed.

## Focused host and build evidence

- Native DERP/WireGuard/TCP sustained one stream across rekey: 1,369 of 1,369 HTTP
  200 responses over 140.1 seconds, with 495,578 exact body bytes.
- Receive-window/backpressure check: 85 of 85 responses containing 20,465-byte
  bodies, including a three-second application read stall; 1,739,525 body bytes
  matched exactly.
- Official authenticated native register/map passed with the existing identity
  and selected Hub peer, including its discovery key. A forced renewal staged a
  candidate and returned authenticated reauthorization; current-key refresh
  retained that candidate. Completed approved key rotation was not tested.
- Offline discovery checks decrypted actual transport replies and checked peer
  binding, tamper rejection and output backpressure.
- Audio checks exercised real files with the SDK's cached-size behavior, immediate
  capture/early release, replay from byte zero, partial writes, failed flushes,
  storage recovery, short recordings, capture overflow, the two-minute cap,
  termination drain and premature EOF without truncated commit. ASan/UBSan passed.
- Composer checks exercised the real voice/recording code for microphone failure
  and immediate retry, preservation of 160 samples accepted before setup failed,
  stale status reset for unrelated READY/PARTIAL drafts and nonmutating saved-audio
  probes with a growing spool. The real main rendering check distinguishes saved
  audio, fresh input and saved text, verifies paused/error body copy and avoids
  filesystem probes on redraw. Failed final draft saves remain partial and retain
  audio until the final text commit succeeds. Strict host/ARM compilation and
  diff checks passed for the affected sources.
- SDK adapter checks covered delayed open/close notifications, failed opens,
  close-busy retries and readable bytes after EOF. Map-save failure injection
  covered open/write/flush/close/rename failures without premature activation.
  ASan/UBSan passed. A follow-up offline terminal asynchronous-send-error case
  failed before the fix and passed afterward; this is adapter behavior evidence,
  not firmware fault-injection evidence.
- A further offline control check passed 560 authenticated Noise records across
  fragmented reads, maximum wire-record sizes and HTTP/2 boundaries. Start,
  failure and stop paths reset framing state. It did not reproduce the device's
  isolated invalid-record error.
- ARM and simulator builds pass. ARM compilation rejects individual stack frames
  over 1,536 bytes; reviewed crypto/control nesting stays within the device's
  10 KiB game-task budget before unknown SDK internals. Physical evidence remains
  necessary for SDK stack and scheduling behavior.

These were narrow opt-in qualification checks. The repository's heavyweight
suite and new default credential-dependent tests were not used.

## Resumed physical qualification and remaining work

Jesse resumed physical work on October 11 and authorized unattended button input
in the dedicated verification session. Backups preserve the original application
data and every failure run. Hardware observations from this resumed pass are:

- A previously saved review survived installation/relaunch. Mutation
  `d2dc5cfab3a8f2e959b5a297d55bc49b` was fully dispatched and acknowledged by the
  peer TCP stack before the harness intentionally reconnected without reading
  its AppWire receipt. A fresh Hub snapshot settled the original mutation; the
  device subsequently read the new assistant reply. The composer incorrectly
  remained visible, which motivated the uncertainty-to-empty UI fix.
- A fresh 45.060-second physical recording retained and sent all 360,141 mu-law
  bytes and displayed live captions. The concurrent native connection failed
  with an oversized DERP frame; final transcription did not acknowledge its
  commit. The audio stayed intact through repeated retries and application exit.
  Relaunch replayed the complete recording and obtained durable final text.
  This establishes preservation and recovery, not seamless initial delivery.
- That recovered review was sent as mutation
  `ccbaa259266a858acfa3fc16df73266b`. The harness again interrupted after complete
  frame dispatch and peer TCP acknowledgement, before reading the receipt.
  Reconnection found the accepted user message and returned to the physical
  transcript. The Hub produced a new `yellow comet` reply, but the device's
  next maintenance close stalled with 117 queued SDK bytes and did not display
  that new reply before the run ended.
- The stale-pending deadline exposed another lifecycle fault: local close
  succeeded but emitted no server-close notification, leaving admission blocked.
  SDK inspection confirmed that local closure need not emit that callback and
  that queued sends retain raw references to the TCP object. A subsequent queued
  open result now fences earlier commands before object release. Remote EOF uses
  the same fence because the SDK clears its pending counter before every queued
  send necessarily finishes.
- An intermediate development build tried clearing the close callback. Review
  found the SDK C trampoline unconditionally invokes that pointer; the build
  stopped during its next connection attempt and returned to the launcher.
  The final source retains a non-null callback and ignores its release effect
  while local closure is in progress. The subsequently collected firmware record
  confirms a null program counter (`pc:00000000`) in that intermediate build.
  The corrected physical run completed without a new crash record.
- Acoustic setup originally failed despite adequate microphone levels. Actual
  captured PCM showed adjacent tones overlapping at 300 symbols/sec. The shared
  sender now uses 100 symbols/sec; the receiver measures shorter spectral
  windows and samples inside each symbol. The physical Playdate then decoded
  the browser sound, showed the matching approval code, received approval,
  saved the exact destination and synthetic token, and authenticated AppWire.
  The test used the real generic pairing service on the Mac, not the production
  Hub. The production Hub has not been deployed with these pairing endpoints.

After Jesse reconnected USB again, the corrected ordinary and qualification
bundles were installed with every file hash verified. The fresh physical run
started its microphone synchronously, began transcription networking 34 ms
later with 185 samples already captured, and displayed its first caption at
5.155 seconds. Releasing after 45.061 seconds sent all 360,141 mu-law bytes;
the final event arrived at 45.829 seconds. A 132-character review was saved and
held for five seconds before a separate send action.

Mutation `e8159b55f3be72b00190b8e246332fc3` was fully dispatched and TCP-acknowledged
before the harness reconnected without reading its receipt. At 146.88 seconds
from launch the physical client had settled the original mutation, returned to
the transcript, and rendered the new assistant reply `yellow comet`. A separate
authoritative Hub read found exactly one accepted item for this identity. This
qualifies fresh voice through visible reply and post-dispatch receipt loss.

The next maintenance cycle stalled with 117 SDK bytes pending and error zero.
The write deadline permitted close and a new control open, but that open never
called back. The 45-second control timeout retired it; its `opening` flag still
blocked admission. The SDK can omit an open callback on some failure paths, and
an accepted close alone does not establish that the network worker completed an
earlier send. Extended recovery is being qualified separately before leaving
the ordinary Evener app running against the original Hub.
The isolated oversized-DERP observation has not been reproduced in the captured
SDK receive streams and is not claimed fixed solely by the lifetime correction.

Focused resumed checks passed: 24 native acoustic interoperability cases,
frontend formatting/typecheck, ARM/simulator compilation, and the new socket
boundary check under ASan/UBSan. The latter covers no-progress retirement,
consecutive equal-sized writes, local close without notification, a racing
close notification, and remote EOF with an outstanding SDK send. Independent
review confirmed the corrected ownership ordering against the installed SDK.

The follow-up SDK inspection found that local close immediately invalidates the
handle, before its queued close runs. An earlier queued send still reads that
handle. A later-open completion check therefore protected allocation lifetime
too late to protect the handle itself. `b1228da884` uses a single globally
outstanding `network.setEnabled(true)` completion callback before close, and a
second after close before release. Both operations use the SDK's FIFO worker.
Command sequences prevent an older callback from completing later writes or
closes. The same mechanism retires a timed-out open whose worker command finished
without its own callback. It cannot cancel a worker that never reaches the
completion operation; that limit remains explicit in the product guide.

The targeted check first failed the previous adapter when close invalidated a
handle still referenced by a queued send. The corrected version passes under
ASan/UBSan, including stale counters, missing open callbacks, remote EOF, progress
reset, a second socket writing after a fence was queued, and a close submitted
while an older fence remained outstanding. Independent review reran it and found
no remaining actionable source/check findings. ARM and macOS builds pass.

After the Mac was unlocked, the `b1228da884` physical run captured 45.084 seconds
of speech. Networking began 33 ms after microphone start; the first caption
arrived at 5.288 seconds and the final at 46.035 seconds. All 360,327 mu-law bytes
were submitted and the 136-character review was saved. Normal socket closes
completed through the new worker callbacks on hardware. However, maintenance
then reconnected and stalled during `thread/read`, before the harness pressed
send: 117 SDK bytes remained pending with error zero. The queued pre-close worker
callback never arrived. The draft was retained and this run did not dispatch a
mutation. Safe lifetime management therefore does not resolve every underlying
network stall.

The ordinary installed app was then run without the qualification wrapper or
its raw network capture. It also failed during a transcript read, this time
after receiving 193,659 bytes, with 702 bytes pending and SDK send error -5.
Firmware logged `readresult timed out, sending again`; its close eventually
completed, but subsequent control and relay opens failed with read timeout -8.
This excludes diagnostic capture as the sole cause. Device data was backed up
after each run, and both firmware crash/error logs remained byte-identical to
the preceding backup. Further experiments isolate SDK receive-resume behavior
and incoming burst size; neither is a qualified production fix yet.

The receive-resume diagnostic kept the 16 KB TCP window and removed raw network
capture. It called the SDK's zero-length read when availability was zero, which
exercises the SDK receive-resume path without consuming data. Saved mutation
`d4be827954bedd61116a6c5a4d6b601a` reached the Hub, then the harness disconnected
after TCP acknowledgement and before receipt handling. At 68.77 seconds the
physical screen showed the new user item and `yellow comet` reply. A separate
Hub read confirmed exactly one accepted item. Two later automatic maintenance
cycles reconnected and reloaded the transcript without a stall. Crash and error
logs were unchanged. This supports the polling change; it does not identify
the unavailable firmware implementation's internal wait condition.

The equivalent client implementation then completed a fresh physical run with
the diagnostic SDK shim removed. Capture began synchronously; networking
started 33 ms later with 185 samples buffered. A first live caption appeared at
6.857 seconds. Release after 45.069 seconds submitted all 360,187 audio bytes;
the final arrived at 52.595 seconds. The 91-character review remained saved
through automatic connection maintenance before a separate send. Mutation
`ea65d04a5a736a446c7c0d0f5e15c279` was fully dispatched and TCP-acknowledged before
the harness dropped its unread receipt. At 151.13 seconds, the physical client
rendered the new accepted user item and `yellow comet` reply and returned to the
transcript. The independent Hub read again found exactly one accepted item.
Subsequent maintenance reconnected, initialized AppWire and reloaded all twelve
chat rows without an SDK error or stalled-write diagnostic. After the run,
firmware crash/error logs matched the previous backup byte for byte. The ordinary
app then connected, loaded the larger Terminal Bench conversation that previously
failed, and displayed its five current chat rows. No diagnostic SDK wrapper was
present in that ordinary-app check. The ordinary app was left on the verification
conversation with the new reply visible and Hold A Dictate in the footer.

One intermediate qualification attempt stopped before recording because the
harness held A across cached project and session selections. Its target-binding
guard prevented composing or sending. The harness now releases A between these
actions; `hardware-poll-client-fresh-complete.log` is the completed fresh run.
The two-MSS diagnostic bundle was built but never installed or run, and no TCP
window or buffer-size tuning is part of the client change.

## Local artifacts

Under `tools/playdate-client/`, ignored local artifacts include:

- `Evener.pdx` and `build/Evener-native-private.pdx.zip`: normal private build.
- Previous ordinary-device binary SHA-256:
  `63f1d02a0ef387de0bf5fbb3b6cca4d3cc8babb23770e91726c17cd3cfb640d9`.
  Corrected ordinary bundle prepared from `903f8983cf` has binary SHA-256
  `c9dd0ed7011fe787f2e56d7fefd38e5e1a60c5a23849194e8afcaa90edfae4ad`;
  passed the earlier fresh physical end-to-end run. The `b1228da884` bundle had
  SHA-256 `72aa03766027707b220590d2750b100afa8337a4f1a24c36f7389d30c9639b94`;
  every copied file was hash verified. Its physical voice/review path passed,
  but maintenance stalled before send as described above.
  The currently installed `168de9dfac` ordinary bundle has binary SHA-256
  `7c19a3ecf994bddc7499748cf4e8d3b8243de7f7827f8bfe9f21f2138d1ba18a`.
- `build/native-build-metadata.json`: metadata for its archived private build;
  consult each later run's build/console logs for the code actually exercised.
- `build/EvenerCheck.pdx` and `build/device_check.c`: opt-in physical qualification
  wrapper, using production networking, microphone, storage and rendering.
- `build/EvenerDispatchCheck.pdx` and `build/device-dispatch-check-build.log`: the built
  second harness for actual-dispatch interruption, exact session restriction and
  case-insensitive expected-phrase checking. Resumed runs verified actual-dispatch
  receipt-loss reconciliation. The corrected run additionally completed fresh
  voice through a new reply on the physical display; later maintenance remains
  a separate unresolved recovery case.
- `build/device-console.py`: SDK USB console capture, button and screenshot helper.
- `build/native-short-press.log`, `native-short-release.log` and
  `native-radio-recovery.log`: immediate capture and retained-audio evidence.
- `build/native-short-thread.png` and `native-radio-recovery.png`: physical
  cached transcript and pending transcription screens.
- `build/native-oct11-scheduled-launch.log`, `native-oct11-retirement-launch.log`
  and `native-oct11-open-after-maintenance.log`: native control/Hub and connection
  retirement observations.
- `build/native-oct11-offline-first.log` and
  `native-oct11-fresh-offline-first.log`: recovered 17,414-byte delivery and fresh
  360,048-byte capture/delivery, final events and pending-close diagnostics.
- `build/native-oct11-recovered-transcription.png` and
  `native-oct11-user-review.png`: physical transcription/composer observations.
- `build/native-oct11-close-burst-immediate.log` and related close-probe build
  logs: public-endpoint SDK retirement isolation. The bundle labelled drained
  reused an earlier ELF and also ran immediate mode; it is not a drained/immediate
  comparison. Wrapper builds now force relinking as well as compilation.
- `build/native-oct11-drain-normal-voice.log`,
  `native-oct11-drain-normal-final.log` and `native-oct11-drain-normal-send.log`:
  normal-native live captions, full audio delivery, durable review, pending-write
  maintenance recovery and pre-dispatch reconnect/accepted-mutation observations.
- `build/native-oct11-drain-normal-reply.raw` and its PNG: the first harness's
  final capture. The raw file includes a binary screenshot prefix and must not
  be treated as a text-only log or as proof that the device observed the reply.
- `build/verification-read.json`: authoritative later Hub read containing the
  accepted user message and assistant reply `yellow comet`.
- `build/native-oct11-dispatched-voice.log` and
  `build/native-oct11-durable-review-relaunch.log`: the second live capture,
  127-character durable review and rapid-relaunch stall. The filename does not
  imply that the dispatch interruption actually ran.
- `build/native-oct11-relaunch-stats.log`,
  `build/native-oct11-after-review-relaunch.raw` and its partial PNG:
  first-frame/no-progress diagnostics. The incomplete screenshot is not a full
  device-screen capture.
- `build/device-backup-before-native/`, `device-backup-oct11/` and
  `device-backup-composer-oct11/` and `device-backup-drain-oct11/`: preserved
  app/data and firmware crash diagnostics.

The private bundles contain the authorized embedded credentials. They are local
artifacts, not publishable releases. No separate repository or remote PR has been
created.

Resumed-run evidence is also preserved under the ignored build directory:

- `hardware-reconciled-transcript.png`: physical return to transcript after the
  accepted uncertain send; `verification-thread-current.json` records one
  accepted user item for each recovered mutation and the Hub replies.
- `device-before-acoustic/`, `device-concurrent-failure/`,
  `device-wire-capture/`, `device-retirement-stall/`, and
  `device-local-close-failure/`: staged physical backups and failure evidence.
- `acoustic-physical-code.png`, `acoustic-approved.png`, `acoustic-saved.png`,
  and `device-acoustic-approved/`: matching code, approved review, authenticated
  connection and durable settings readback. The temporary services were stopped
  after this successful check.
- `hardware-close-lifecycle-live.log`: intermediate callback-clearing build;
  `hardware-owned-close-build.log` and `ordinary-owned-close-build.log` describe
  the corrected builds that preserve callback and worker ownership.
- `hardware-owned-close-live.log` and `hardware-owned-close-reply.png`: the
  completed fresh physical recording, live captions, durable review, actual
  dispatch, deliberately lost receipt, reconciled message and visible new reply.
- `hardware-owned-close-maintenance.log` and its `-followup.log`: the subsequent
  117-byte stall and omitted open callback. `hardware-fence-live.log` records two
  successful read-only maintenance cycles without reproducing that condition.
- `device-before-owned-close/`, `device-owned-close-e2e/` and
  `device-passive-fence/`: device data and firmware diagnostics around those runs.
- `hardware-worker-fence-build.log`, `ordinary-worker-fence-build.log` and
  `native-build-metadata.json`: the installed follow-up's build and file hashes.
- `hardware-worker-fence-live.log` and `device-worker-fence-stall/`: the follow-up
  recording/review and pending-worker failure with preserved device data.
- `ordinary-worker-fence-start.log`, `ordinary-location.raw`,
  `ordinary-return-projects.log`, `ordinary-recovery-wait.log`,
  `ordinary-esp-boot.log` and `device-ordinary-stall/`: the ordinary app's
  independent reproduction and firmware diagnostics. The `.raw` file includes
  a binary screen block and is not a text-only log.
- `hardware-empty-read-delivery.log`, `empty-read-reply.png` and
  `device-empty-read-delivery/`: saved-draft delivery, deliberately lost receipt,
  physical new reply, two later maintenance reloads and unchanged diagnostics.
- `hardware-poll-client-fresh-complete.log`, `poll-client-live.png`,
  `poll-client-review.png`, `poll-client-reply.png` and
  `device-poll-client-qualified/`: fresh physical voice-through-reply and later
  maintenance with the committed client polling change.
- `ordinary-qualified-start.log`, `ordinary-qualified-large-read.log` and
  `ordinary-qualified-large.png`: ordinary-app connection and successful reading
  of the larger conversation that failed with the previous client.
- `ordinary-final-thread.log` and `ordinary-final-thread.png`: the ordinary app
  connected to the original Hub and left ready to dictate in the verification
  conversation, with the newest accepted message and reply visible.
- `device-diagnostic-bundles/`: hash-verified local copies of the temporary
  qualification/probe apps removed from the device. The ordinary Evener app and
  the earlier voice/DERP spikes remain installed; device data was preserved.

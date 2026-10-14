# Evener for Playdate

A native C client for Playdate SDK 3.1.2. It connects directly to an
Evener Hub over AppWire v7 through its own Tailscale DERP/WireGuard connection,
and to OpenAI Realtime transcription over SDK TLS. Native control-plane
maintenance refreshes the enrolled identity and selected Hub peer. Runtime
needs no Mac helper or application proxy. The client's source and vendor trees
contain its audio, networking and control dependencies. Building the app does
not require an Evener source checkout.

See the [client guide](docs/client.md), [source provenance](docs/provenance.md),
and [physical qualification report](docs/qualification/2026-10-10-native-client.md).

## Controls

- **Sessions:** D-pad or crank selects; A opens. Left/right switches Live and
  Projects. Inside a project, left/right switches Current, Recent and Archived.
  Select More sessions for the next page; B returns to the previous page/view.
- **Conversation:** crank or up/down scrolls. Scrolling to the top requests
  older history; scrolling to the bottom of an older window continues toward
  newer messages. The newest ten messages arrive first, then earlier history
  loads into the device's cache in the background. B returns to sessions.
  The system menu's Latest action returns to the newest text.
- **Text size:** the system menu's Font option selects Small, Medium or Large.
  It sizes conversation text and session/project titles; lists show six, five or
  four rows respectively. The device remembers the choice, keeps the selected
  list row and preserves your place when conversation text reflows.
- **Voice:** when the conversation says Hold A Dictate, hold A and speak
  immediately. The microphone starts before network work; connecting or retrying
  does not postpone capture.
  Captions appear when transcription becomes available. Release A to finish,
  including before connection completes. Review the saved final text, then
  press A separately to send; an active session receives it in its queue.
- **Drafts:** B preserves the session's draft and unfinished audio. A reopens
  saved input and resumes transcription before allowing a replacement recording.
  An empty text draft with retained audio says A Transcribe saved audio, including
  in an empty conversation; that action restores the recording instead of starting
  the microphone. A Review draft opens text already saved for review.
  Use Actions → Discard draft in the system menu to replace it explicitly. Partial text
  remains visible through retranscription and cannot be submitted as a final
  message. A failed final text save keeps the audio and requires a successful
  save before sending.
- **Offline reading:** previously opened session pages and conversation windows
  are saved on the device. Reopening shows the saved copy while reconnecting;
  sending waits for a fresh session snapshot. Latest keeps the visible
  conversation until its replacement arrives.
- **Network:** the system menu's Actions option offers Reconnect, Receive setup,
  Tailscale sign-in and Change tailnet. Receive setup listens for a Hub invitation
  played by the Web UI's Pair using sound screen. Approve the matching fingerprint
  in that browser, then press A on the Playdate to save, or B to cancel. Change tailnet asks
  before creating a new native identity and keeps saved messages and drafts.

The display includes user messages, assistant messages and human steering.
Tools, reasoning, images, interactive approval cards and Hub settings are outside
this client's scope. Use another Evener client for those interactions.
History reads request conversation-only content from the Hub using the generic
AppWire inclusion flags. Use a Hub that implements history content selection;
update the Hub before installing this client build.

## Configure and build

Acoustic setup requires a Hub with the shared device-pairing API and Web UI,
maintained in the Evener repository. For a Hub without that feature, use the
baked-in destination configuration below. Ordinary session browsing and voice
use the existing Hub API.

Install Playdate SDK 3.1.2 and a full ARM GNU toolchain with newlib. Node 22+
is needed only to generate the private configuration.

Clone `prime-radiant-inc/evener-client-playdate` and run from its root:

```sh
node --env-file=/path/to/openai.env configure.mjs \
  --setup-only

export PLAYDATE_SDK_PATH=/path/to/PlaydateSDK
export PATH=/path/to/arm-gnu-toolchain/bin:$PATH
make
```

The environment file supplies `OPENAI_API_KEY`. The generator creates a unique
private entropy seed and omits Hub and tailnet credentials. On first launch,
the device listens for setup sound. In the authenticated Web UI, open **Settings →
Mobile app → Pair using sound**, select the Hub address and Tailscale or direct
Wi-Fi, create an invitation and play its sound near the device. If fragments are
missing, replay the same sound. The browser needs no microphone or camera and
works on HTTP over the tailnet.

The invitation supplies the address and a short-lived secret. The Playdate
contacts that Hub with its own temporary key. Compare the fingerprints and
approve that device in the browser. The Hub returns the credential encrypted to
the approved key; press A on the Playdate to save the reviewed destination.
The [shared protocol reference](docs/device-pairing.md) also supplies QR invitations
to the native phone app. The Hub and Web UI have no device-specific pairing API.

For Tailscale, the device shows the official sign-in QR if it needs enrollment;
approve it on the Hub's tailnet. The native client saves its pending identity
before registering and resumes that identity across restarts. The authenticated
map resolves the Hub by name or IPv4 address. A network interruption retries the
same pending claim; cancelling or restarting setup leaves the saved Hub and its
drafts untouched. Receiving an invitation temporarily selects its connection
namespace, while data ownership changes only after A saves the approved Hub.
OpenAI stays direct with the key in this private build; a shared Hub voice
endpoint is a future integration, not required by setup.

To bake in a destination instead, replace `--setup-only` with
`--hub-url http://your-hub:9180 --hub-token-file /path/to/hub-token`.
The token file contains only the Hub authentication token. Optionally,
`--tailnet-config /path/to/private-tailnet.json` supplies the isolated node
configuration exported by the one-shot [enrollment helper](enroll/README.md).
It must include its machine identity, control public key and stable selected
peer ID. Enrollment needs the person's Tailscale authorization. Runtime
maintenance uses that identity directly from the handheld. `--output DIR` writes
private configuration to another bundle staging directory. Give each device
its own generated seed; do not distribute or clone a configured private bundle.

Use an HTTP/WS Hub URL inside the encrypted tailnet: WireGuard authenticates the
selected peer and encrypts the Hub stream, while the DERP socket uses verified
TLS. The generator takes the Hub address and port from the enrolled peer;
authenticated map refreshes keep them current. OpenAI uses its separate verified
SDK TLS connection. Without `--tailnet-config`, configuration selects the
existing SDK TCP/TLS mode and needs a Hub address reachable from the device's
network; HTTP/WS in that mode carries plaintext traffic on that network.

Only one runtime may own the isolated node identity. Stop the device before
running enrollment, native host probes or refresh with that identity. After
native renewal, return ownership to the enrollment helper using the device's
current configuration and `--adopt-native-config`; a stale bundled configuration
cannot establish its current keys. See the helper's ownership contract before
performing that transfer.

Open `Evener.pdx` in Playdate Simulator, then choose **Device → Upload Game To
Device**. The installed app is **Evener**. Allow its microphone and network
permissions for the transcription service, Tailscale control service and selected
DERP relay. The bundle includes its generated configuration and credentials,
as explicitly chosen for this personal build. `Source/config.json`,
`Source/tailnet.json`, build outputs and PDX bundles are ignored by Git; do not
distribute the private PDX.

## Ownership and limits

The 400 × 240 layout shows six, five or four session/project rows for Small,
Medium or Large text. List titles and conversation text use the chosen system
font, with Roobert 10 as the default. Transcripts use outlined assistant bubbles
and inverse user bubbles, with a compact inverse header and one footer line.
The bundled Nontendo Light font by Shaun Inman (from the Playdate SDK) supplies
compact metadata and controls. Long recovery instructions take priority over
footer control hints.

The firmware's game task has a fixed 10 KB stack, including SDK calls. Large
encoding and cache buffers use static storage on the serial game loop. The ARM
build writes `.su` stack-usage reports and rejects individual frames above
1.5 KB; nested call totals still need review. Changing a sample Makefile's
`STACK_SIZE` does not enlarge the firmware task's stack.

- `websocket.c` implements bounded RFC 6455 framing over the native Hub byte
  stream or an SDK socket: masked sends, upgrade validation, fragmentation and
  ping/pong. `net_socket.c` retains retired SDK TCP objects until their queued
  work finishes, so an outstanding send cannot use a released connection.
  It admits one open attempt at a time and at most two TLS connections.
  Empty-buffer availability polls issue a zero-length SDK read so the radio's
  receive-resume path is serviced without waiting for or consuming bytes.
  Each socket permits one outstanding SDK write. An SDK send error or twenty
  seconds without progress initiates recovery. Before local close, a queued
  worker callback confirms earlier opens and sends have finished; a second
  callback confirms close completion before releasing the object or admitting
  its replacement. The pending counter alone cannot establish completion, and
  SDK close immediately invalidates the handle used by queued sends.
  SDK 3.1.2 serializes `network.setEnabled(true)` on that same worker. The adapter
  uses its callback as the completion fence, with only one outstanding call
  because the SDK stores one global callback. This also retires a timed-out
  open whose command finished without delivering its own callback. Remote EOF
  keeps the same ownership protections. A worker that never reaches the fence
  still cannot be cancelled through this queued API. After thirty seconds of
  stalled retirement, the experimental fallback saves the current draft and
  restarts its runtime,
  retaining the selected view and pending send intent. Relaunch is spaced at
  least sixty seconds apart, waits for active recording to stop and audio to
  finish saving, and never bypasses a failed save. Existing mutation identities
  reconcile through a fresh Hub read. The fallback uses no additional socket:
  DERP and direct transcription retain one TLS slot each. Acoustic pairing and
  identity replacement are not automatically restarted. Repeated stalled-worker
  recovery has failed in a standalone SDK probe with firmware allocator faults.
  The fallback remains experimental and cannot guarantee recovery; see the
  [SDK reproductions](docs/qualification/sdk-network-report.md) and
  [client hardware evidence](docs/qualification/2026-10-13-runtime-recovery.md).
  Queued admission does not consume connection deadlines.
  `random.c` combines a private build seed with a durably advanced launch counter
  to supply cryptographic nonces across relaunches.
- `tailnet.c` owns authenticated DERP framing, WireGuard and lwIP's reliable TCP
  stream for one configured IPv4 Hub peer and port. The raw lwIP API runs on the
  serial game loop with bounded queues and explicit backpressure. WireGuard
  rekeys without replacing the TCP stream; an unrecoverable stream failure
  returns to the Hub's authenticated snapshot and mutation-recovery flow. A
  stable discovery key is advertised through control, and the transport answers
  authenticated discovery pings from the selected peer through DERP.
- `control.c` authenticates Tailscale register/map maintenance with Noise IK and
  HTTP/2. It refreshes node/peer keys, addresses, expiry and relay metadata. A
  replacement node key is durably staged before renewal and is activated only
  after an authenticated map is persisted. `tailnet_device.c` owns verified TLS,
  permissions, persistence, retries and transport activation. A changed relay
  is pinned over verified TLS before use. First activation waits for a completed,
  saved map; later starts can use that saved map during a control-service outage.
  Control maintenance holds exclusive SDK access across key fetch and map, then
  automatically reconnects the relay. Dictation preempts or defers maintenance.
  Foreground conversation loads and sends on an established Hub connection also
  defer routine maintenance while the saved identity and peer remain authorized
  and unexpired. Reconnects remain free to refresh a stale map.
  The established relay can coexist with transcription, but a relay reconnect
  waits for transcription to release its socket before starting its handshake.
  If renewal needs sign-in, the device
  retains the staged key and presents the authorization instruction; it keeps
  using and refreshing an authorized, unexpired current identity while possible.
- `pairing.c` owns invitation reception, the temporary recipient key, microphone
  lifecycle, expiry and reviewed settings acceptance. `chirp.c` decodes 44.1 kHz
  four-tone PCM with Hamming correction, CRC-checked fragments and bounded
  reassembly. `pairing_http.c` retries a bounded claim over the pending direct or
  native tailnet connection. The Hub encrypts approved settings with NaCl box;
  the client verifies they match the invitation before offering to save.
  A setup transfer is bounded at 1152 bytes and expires after ten minutes.
- `settings.c` persists accepted Hub settings independently of the embedded
  OpenAI key and build seed. Drafts, audio and caches are scoped to the
  destination and authenticated Tailscale peer; native connection keys use the
  destination alone. Token changes retain the same profile. Initial bundled
  ownership is saved before loading data, preserving root files across changes
  to build defaults. Switching destinations or tailnets flushes input and
  disconnects before clearing RAM. A fully flushed settings stage is committed
  even if promotion must wait for restart; cancel removes an incomplete attempt
  before leaving review.
- `audio.c` owns the bounded atomic capture ring, 44.1 kHz FIR/downsampling and
  8 kHz G.711 mu-law encoding. The microphone callback only filters and enqueues
  samples. `voice.c` starts capture synchronously, independently connects to
  `gpt-live-transcribe`, sends saved audio in bounded chunks and commits after
  release. A transcription disconnect preserves capture and replays the saved
  recording from the beginning on retry. Short taps retain their speech and
  receive trailing silence only to meet the service's 100 ms minimum.
- `recording.c` stores session-bound mu-law audio in the device's data directory.
  A checksummed header verifies session identity. The game loop writes and
  flushes at most 800 audio bytes per frame; pending short writes or failed
  flushes retain their batch for recovery. Readers reopen when they exhaust a
  file-size snapshot, so later speech in a growing recording reaches the service.
  A final commit waits for every saved sample to be sent. Audio is removed after successful
  final text persistence or explicit discard. Pause/lock stops capture and lets
  subsequent frames drain; termination drains all accepted audio in bounded
  batches before closing the spool. A storage failure stops capture and asks
  the person to keep the app open so pending RAM can be saved.
- `hub.c` reads navigation v3 pages, subscribes to the selected thread, pages
  recorded history and applies live chat overlays. The Hub owns session order,
  identity, capabilities and accepted input.
- `storage.c` writes per-session drafts and mutation identity to the Playdate's
  data directory, with a checksum and recoverable temporary record. It flushes
  and closes that record before replacing the primary file, and promotes a
  complete interrupted write on load. A send is persisted
  before dispatch. Reconnect reads reconcile accepted identities; unresolved
  sends can replay the original method, ID, instance fence and text only after
  an authoritative mutation snapshot. A refused send returns to review.
- The first caption is saved immediately, followed by updates at most once per
  second and on finalization or leaving the composer. `cache.c` saves bounded
  navigation pages and chat windows, plus older history pages, with checksums
  and recoverable temporary files. Background history reads do not replace the
  visible window; scrolling selects older or newer pages from that archive.
  Cached transcripts provide reading context; the Hub remains authoritative.
- The device retains 64 chat messages, at most 32 KiB of text per message,
  8 KiB voice drafts, 20 navigation rows per page and 2 MiB incoming WebSocket
  messages. Audio recordings are capped at two minutes (960000 mu-law bytes);
  the capture ring holds 32768 samples. Reaching the cap or overflowing that
  ring stops capture and keeps the accepted audio with a review warning.
  Oversized chat text is explicitly marked. Reading windows stay bounded as
  scrolling moves through the disk archive; Latest restores the tail.
  The system fonts do not contain every Unicode glyph.

The Hub excludes unrequested tool and reasoning content before paging. A response
beyond the device's message limit still cannot be displayed. Session creation,
transcript export and editing typed text are not implemented.

## Verification

Build both ARM and simulator targets. With explicitly supplied credentials,
check native session navigation, existing conversation reads, immediate physical
microphone capture, captions, review-before-send, a dedicated session's reply,
draft/audio preservation and reconnect. Include release before TLS configuration,
transcription failure with replay, restart with unfinished audio, termination with
a full capture ring, SD write/flush latency, and a sustained Hub stream across
WireGuard rekey. Host protocol checks and synthetic audio fixtures establish
those paths only at their tested boundary; they do not prove physical microphone
onset or device SD behavior. Provider requests are never part of the repository's
default test suite. This project's authorized verification workflow does not require
new automated tests.

The opt-in acoustic checks use synthetic credentials and never contact a provider:

```sh
# From this repository root, independent storage qualification:
make -f check/Makefile build/check/storage
build/check/storage

# SDK ownership and write-progress qualification, without network access:
make -f check/Makefile build/check/network-socket
build/check/network-socket

# Saved transcript window and authoritative refresh, without network access:
make -f check/Makefile build/check/hub-recovery
build/check/hub-recovery

# TCP receive credit through real packets, without sockets or credentials:
make -f check/Makefile build/check/tcp-receive
build/check/tcp-receive
```

The cross-client acoustic checks live with the shared pairing implementation in
Evener. With its shared pairing implementation checked out and frontend dependencies
installed, point that checkout at this client explicitly:

```sh
export EVENER_PLAYDATE_CLIENT=/absolute/path/to/evener-client-playdate
export PLAYDATE_SDK_PATH=/path/to/PlaydateSDK

# From the Evener checkout's cmd/evener-hub/frontend directory:
node scripts/check-playdate-pairing.mjs

# From the Evener checkout's root (isolated Hub, browser, C and phone protocol):
EVENER_PAIRING_E2E=1 go test ./cmd/evener-hub -run TestDevicePairingBrowserRoundTrip -v
```

These optional interoperability checks need both repositories; the app build and
local storage/socket checks need only this repository and the SDK.

SDK or serial crank injection can disable physical crank updates. After any
such check, turn off Simulator device control, quit Simulator, and issue the
console's `enablecrank` command before handing the device back. Qualification
helpers that inject crank movement must restore it in their cleanup path, even
when a check fails. Confirm scrolling with the physical crank before finishing
device qualification; injected movement alone does not verify the hardware input
path.

The browser check runs real Web Audio at desktop and phone sizes, generates a
shared invitation QR and sound, decodes both, approves through a real local Hub,
decrypts its result in C and TypeScript, and authenticates with the received
credential. It also builds and runs `build/check/receiver`: the real C receiver,
HTTP poller and settings code with host SDK boundaries. A loopback TCP adapter
maps the pending destination to that test Hub and injects a lost reply plus short
reads/writes. The check covers permission/pause/expiry, reviewed save, failed-save
retry/cancellation, restart and draft preservation. These checks do not exercise
SDK TLS or the native Tailscale transport, speaker-to-microphone reliability,
handheld timing or SD behavior; those require separate transport or physical runs.

`make -f check/Makefile build/check/control-host build/check/tailnet-host` builds
the explicit live protocol runners (OpenSSL defaults to Homebrew's path; set
`OPENSSL` elsewhere). `control-host --enroll HUB PORT STATE.binary` creates an
isolated test identity, saves it before enrollment, and resumes the same identity
on repeated invocations. `PLAYDATE_CONTROL_AUTH_URL_FILE` receives its sign-in
URL; `PLAYDATE_CONTROL_RESULT_JSON` receives the authenticated map configuration.
Keep both outputs private and all other owners of that identity stopped.
`node check/pin-relay.mjs RESULT.json` pins its relay over verified TLS before a
`tailnet-host RESULT.json SECONDS` traffic run. None of these live checks runs
under the default test suite.

Transport and control provenance and licenses live under `vendor/`: lwIP 2.2.1,
the pinned WireGuard/refc implementation, TweetNaCl, Nayuki QR 1.8.0 and
nghttp2 1.67.1 HPACK.
The host protocol runners are opt-in qualification programs, excluded from the
handheld sources in `src/transport.mk` and `src/control.mk`.

`src/jsmn.h` vendors [jsmn v1.1.0](https://github.com/zserge/jsmn/tree/v1.1.0),
under its embedded MIT license. The JSON adapter decodes escapes and keeps
bounded text copies on UTF-8 boundaries.

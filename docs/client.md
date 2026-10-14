# Playdate client

The [native Playdate client](../README.md) provides
session navigation, chat transcripts and reviewed voice input. It is a
C client using the existing [AppWire protocol](https://github.com/prime-radiant-inc/evener/blob/main/docs/appwire-protocol.md). It
connects to the Hub through its native Tailscale DERP/WireGuard byte stream and
to OpenAI through a separate SDK TLS connection for transcription. The device
owns transport and control-plane recovery without an application proxy or
ongoing Mac helper.

## Setup and saved destinations

An authenticated browser opens **Settings → Mobile app → Pair using sound** and
creates an invitation for the desired Hub and connection type. The device's
system menu exposes **Network → Receive setup**; an unconfigured build opens it
on launch. The browser plays the shared [device invitation](device-pairing.md)
as sound. It contains the Hub address and a temporary invitation secret, not a
Hub credential. The microphone queues PCM; the game loop decodes and verifies
complete fragments. Repeated playback repairs missing fragments.

The device generates an ephemeral key and contacts the advertised Hub. Both
screens independently show the same 128-bit comparison fingerprint as four lines
of two four-digit hex groups. After the person approves that exact code in the
authenticated browser, the Hub releases the
credential encrypted to that device key. The Playdate verifies the envelope and
its destination, then offers A to save the reviewed settings. B cancels without
changing saved settings or drafts. A network interruption retries with the same
key; receiving the approved result does not consume it. Setup expires after ten
minutes. Restarting the app requires receiving a new invitation; temporary keys
and approval responses are not persisted.

While seeking approval, the pending destination supplies connection settings and
its own native identity namespace. The active data namespace remains unchanged,
and no old Hub token is sent to the pending destination. Cancelling restores the
active connection. Only a successful settings commit changes data ownership.
Native Tailscale accepts HTTP with DNS or IPv4 locators; direct Wi-Fi also
supports TLS. Unsupported invitation transports are reported on the device.

Received settings own only Hub connectivity. The private build still supplies
the OpenAI key and unique entropy seed. OpenAI credentials never enter the QR
or setup sound. A future Hub voice endpoint can replace that direct connection
when the Web UI's shared voice service exists; setup does not depend on it.

The device commits settings by flushing and closing a checksummed temporary
record. Replacement of the primary file can finish on restart without delaying
use of the committed settings. An incomplete save keeps the candidate available
for retry; B removes that attempt before reporting cancellation. If removal
fails, review stays open so cancellation can be retried.

Drafts, retained recordings and transcript/navigation caches belong to the Hub
destination and, for Tailscale, its authenticated peer stable ID. Matching names
on different tailnets do not share saved input. Native connection keys belong to
the configured destination so the device can reconnect before resolving its
peer. Token rotation keeps data ownership; a different destination or peer gets
its own files. The original bundled destination is persisted before any saved
work is used, and its root files are bound to the first authenticated peer.
Changing a build's defaults does not move saved work. Changing Hubs or tailnets
saves pending input and closes the previous connection before clearing the
selected session. Returning to a previous destination and peer restores its
saved input. Pending submissions are never replayed into a different Hub.

For Tailscale, the device generates and saves its own machine, node and discovery
keys before contacting the control service. A sign-in QR opens Tailscale on the
person's phone, where they choose the Hub's tailnet. The device polls with the
same pending identity through restarts. Its authenticated map resolves the Hub
by configured name or IPv4 address, then pins the peer's stable ID for subsequent
refreshes. **Network → Tailscale sign-in** reopens that status; **Change tailnet**
requires an explicit A press to create another identity and preserves local
messages and drafts. The old node remains in Tailscale until removed there.
Hub approval uses the generic device-pairing endpoint. Voice remains a separate
direct connection to OpenAI.

## Navigation and reading

History reads, older pages and reconnects request `includeConversation: true`
and exclude thinking, tool calls/results, tool-call intents, system messages and
diagnostics through the Hub's shared history-selection API. The eight-item page
counts selected conversation items; excluded activity does not consume its slots.
Metadata-only send preparation uses the same options and retains capabilities,
mutation authority and receipts while omitting detailed diagnostics. The Hub must
implement the generic inclusion flags before this client is installed. Opaque
cursors and snapshot incarnations stay owned by the server.

The Hub's navigation v3 resources own membership and ordering. Live sessions
are available directly; Projects opens paged Current, Recent and Archived
session lists. The client reads only root session rows, rather than treating
delegate children as an activity tree. It does not expose session activity,
Hub settings, tool output, reasoning or interactive approval controls.

Selecting a session reads its bounded transcript window and replaces the
connection's subscription. The device displays user and assistant text plus
human steering. Recorded updates merge by transcript key and version; streaming
assistant overlays are retired when their round ends. D-pad and crank scroll
the transcript; reaching the top loads older items. Scrolling away from the end
stops automatic following. Latest messages restores the newest window.

Transcripts distinguish speakers with outlined assistant bubbles and inverse
user bubbles instead of repeated labels. Compact type, an inverse header and a
single-line footer reserve most of the display for conversation text. The footer
shows connection or session status alongside controls when both fit; longer
recovery instructions take priority. Navigation displays six rows per screen.

The client composes each screen in an offscreen bitmap before copying changed
pixel rows to the display buffer. An unchanged frame requests no display update,
so idle network activity does not require clearing and repainting the visible
conversation. Setup, authorization, navigation and voice use the same presentation
path; input, recording and networking continue on every update.

Opened navigation pages and conversation windows are cached in the device's
data directory, preserving reading across disconnects and restarts. The client
shows saved content while it refreshes. A successful fresh read replaces the
startup cache; a failed refresh leaves it readable. History requests use eight
projected items per page, with older items fetched as the person scrolls or when
the first page contains too few chat messages. Cached content never grants send
capabilities or establishes mutation acceptance.

## Voice and delivery

When the conversation says Hold A Dictate, hold A and speak immediately. The
microphone starts before network work, while transcription connects independently.
Release stops capture even
before the connection is ready. The device sends filtered 8 kHz mu-law audio to
OpenAI Realtime and shows partial captions when available. Finalized, saved text
remains in review; a separate A press from that review sends it. A button press
during finalization cannot become a send when the caption arrives in that frame.
B preserves the selected session's text and unfinished audio. Discard draft is
an explicit system-menu action.

The microphone callback writes only to a bounded atomic ring. The game loop
flushes small batches to a session-bound audio spool, retaining speech captured
before connection and through transcription interruptions. A retry replays from
the beginning while keeping previous partial text visible until new captions
arrive. Reopening the composer restores unfinished audio before allowing a
replacement recording. Audio is removed only after final text is durably saved
or the person explicitly discards it. A saved final or pending-send text draft
continues to own the session if an interrupted audio-cleanup file remains.

The conversation labels A Transcribe saved audio when an empty text draft still
has a retained recording, including before any chat messages exist. A restores
that audio for transcription; it starts fresh capture only when no saved input
owns the session. Saved text remains available through A Review draft. The device
refreshes its saved-audio indicator at session and input transitions without
opening or interrupting the active audio spool.

The composer header distinguishes Recording, Transcribing audio and Dictation
paused from Review message. An empty composer says Dictate message and accepts
a fresh hold of A; silence does not leave an empty message labelled for review.

A two-minute recording cap or ring overflow stops capture, preserves accepted
audio and marks the result for review. Storage failure stops capture and retains
pending RAM and the saved prefix while storage retries; the device asks the
person to keep Evener open so that remaining audio can be saved. Lock/pause stops
capture and subsequent frames drain pending audio. Termination drains accepted
audio in bounded batches before closing the spool. Abrupt power loss can lose
the tail still waiting in RAM; flushed audio and saved text recover.

The device owns drafts and pending submission identity. It persists the full
text, method, mutation ID and expected instance before dispatch. The existing
Hub and daemon contracts own resume, queue admission, idempotence and durable
acceptance. Active sessions receive queued input. Preparing a send reads current
session metadata without downloading history again. It keeps the conversation
window and older-history cursor, and the composer continues to show the pending
send until it dispatches or reports a failure. Another A press while preparation
is pending does not start another preparation read. After reconnect, delivery
reconciliation takes priority over refreshing history and session lists. That
metadata read does not mark cached conversation text as freshly loaded.
Accepted identities settle
pending submissions; an authoritative mutation snapshot permits replay with
the original identity and payload. An uncertain send is never turned into a
new mutation merely because a response was lost.
Confirming acceptance returns the composer to the conversation, including when
that confirmation arrives in a reconnect snapshot, so the reply remains visible.

Draft commits flush a complete checksummed temporary record before replacing
the primary file. A valid interrupted temporary record is promoted on load;
an incomplete one leaves the previous committed draft intact. This preserves
input across a reboot even when the device cannot rename over an existing file.

The client automatically retries Hub and transcription connections with bounded
backoff. A transcription failure retains audio for replay; partial text cannot
be sent as finalized speech. Draft storage failure preserves in-memory text,
keeps failed final saves partial and prevents dispatch. Other clients remain
available for approvals and interactions outside the Playdate's chat surface.

The first nonempty caption is persisted immediately, with further partial
updates at most once per second and final text saved before sending. Connection
cleanup keeps retired TCP objects alive through their outstanding callbacks,
so reconnecting cannot reuse an object still owned by the SDK's notification
queue. Unlocking restarts the Hub connection automatically.

## Native connectivity and device boundaries

The device's native transport owns one reliable TCP stream to the selected IPv4
Hub peer and port. lwIP, DERP framing, WireGuard and control maintenance run on
the serial game loop with bounded queues. WireGuard rekey preserves the TCP
stream. A failed stream reconnects through the Hub's existing authenticated
snapshot and mutation reconciliation; transport acknowledgment never establishes
application acceptance. Cached reading and device-owned input survive that
recovery.

The experimental receive path limits the Hub to 512 bytes of outstanding TCP
data. Once the application consumes bytes, it promptly advertises the available
space, including after a short segment; unread data receives no new credit.
The WebSocket reader flushes the relay output after consuming those bytes, so
acknowledgments need not wait for the next game update. That flush uses the same
bounded queue and one-outstanding-write admission as ordinary relay output.
This avoids delayed window updates starving a sender with only one segment of
capacity. The packet contract is checked locally, but the transport remains
subject to the [native hardware qualification](qualification/2026-10-13-transport-workaround.md).

SDK connection admission is queued, with one open attempt at a time and at most
two TLS connections. Availability polls enter the SDK's receive-resume path even
when its read buffer is empty, without waiting for or consuming incoming bytes.
Each socket has at most one outstanding SDK write. An SDK
send error or twenty seconds without write progress initiates recovery. Cleanup
confirms the SDK worker finished earlier sends and opens before closing their
handle, then confirms close completion before releasing the object and admitting
its replacement. Neither a zero pending counter nor a server-close notification
establishes completion. A timed-out open can retire even when its own callback
was omitted, once the worker confirms completion. If the SDK worker itself stops
processing commands, the queued API cannot cancel it. The experimental runtime
recovery fallback waits thirty seconds after stalled retirement, saves work and
calls the SDK's runtime restart. Repeated recovery from stalled SDK workers
has failed in a standalone probe with firmware allocator faults, so this
experimental fallback cannot guarantee recovery. See the
[SDK reproductions](qualification/sdk-network-report.md) and
[qualification record](qualification/2026-10-13-runtime-recovery.md) for the
separate SDK and client voice-delivery boundaries.
It restores the current conversation or navigation page, scroll/follow state
and composer. A send requested before dispatch resumes preparation; a dispatched
send retains its durable mutation identity and requires the existing
reconciliation before replay. Cached reading remains available during recovery.

Automatic relaunch is spaced at least sixty seconds apart. It waits while the
microphone is recording and while accepted audio remains unsaved, so holding A
continues to capture speech. A failed draft or audio save defers relaunch.
An interrupted transcription replays its saved audio after returning to the
composer. Acoustic pairing and identity replacement keep their existing
explicit retry flow rather than losing temporary setup state to a restart.
After requesting restart, the current update returns without drawing or submitting
another frame. Subsequent updates, menu actions and lifecycle callbacks leave
teardown to the SDK. The fallback does not allocate another
socket: one TLS slot belongs to DERP and the other to direct transcription.

The device needs exclusive networking
resources to open its Tailscale connections: maintenance pauses the relay across
its key fetch and authenticated map request, then automatically restores Hub reading.
An established relay can remain connected while transcription runs. Dictation
preempts or defers maintenance, and a relay that disconnects during dictation
waits for transcription to finish before reopening. Cached conversation text and
captured audio remain available throughout. Waiting for connection admission
does not consume the actual connection timeout. Completed transcription releases
its connection independently of any pending draft-save or audio-cleanup retry.

An isolated enrolled Tailscale machine identity supplies authenticated Noise
register/map maintenance. The device refreshes node and peer keys, addresses,
expiry and relay metadata, then persists the completed map before activating
changes. Initial activation waits for that completed, saved map; a previously
saved map allows later launches to connect during a control-service outage.
The identity advertises a stable discovery key and answers the selected peer's
authenticated discovery pings over DERP. A changed DERP relay is pinned over
verified TLS. Replacement node keys
are staged durably before registration and reused after an interrupted attempt.
When replacement authorization requires the person, the device retains that
candidate and presents sign-in information while continuing to refresh a healthy,
authorized current identity. If current authorization fails or the map expires,
Hub connectivity waits for authenticated maintenance to restore it, with sign-in
when the service requires it. Cached messages and saved input remain available.

The private build contains the OpenAI key and an individual entropy seed. Hub
credentials and an already enrolled Tailscale identity can optionally be baked
in; acoustic setup and native enrollment avoid that requirement. Generated
configuration and artifacts are excluded from Git. Only one runtime may use a
node identity at a time. The optional enrollment helper requires explicit
adoption of the device's current configuration before taking ownership back
after native renewal. Cloning a private build is not a safe way to provision
multiple devices: each device needs its own seed and node identity.

OpenAI and outer control/relay connections use SDK TLS verification. WireGuard
protects the inner Hub stream and authenticates its selected peer. The existing
SDK Hub mode remains available when tailnet configuration is omitted; its
reachability belongs to the device's configured network, independent of the Mac.

The client bounds navigation pages, transcript rows, individual messages, voice
drafts, audio recordings and protocol queues. Oversized text is labelled; the
complete transcript remains authoritative on the Hub. The current protocol can
include large tool payloads in transcript responses even when this client
displays only chat, and a frame above its 2 MiB limit cannot be read. See the
client README for exact bounds, build requirements and controls. All handheld
sources and vendor dependencies are inside the client directory; the audio code
does not depend on the separate transcription spike.

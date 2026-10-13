# Native transport workaround investigation, 2026-10-13

## Observed failure

The installed client authenticated native control, DERP and WireGuard, opened
Hub TCP and issued `thread/read`. A subsequent 13-byte SDK send remained pending
with SDK error zero and connected Wi-Fi status. The client retired the socket
and restarted after its recovery deadline. The replacement runtime authenticated
again and reached the Hub. This is a post-authentication transport stall;
resetting pairing or replacing the Tailscale identity does not address it.
The final observed Connected screen does not establish sustained recovery.

This follows the [plain TCP isolation results](2026-10-12-sdk-tcp.md) and
[repeated restart failures](2026-10-13-runtime-recovery.md). A received DERP ping
produces a 13-byte framed pong in this client, but byte count alone does not prove
which outgoing frame the SDK held. Raw device captures remain private.

## Split DERP connections

The successful plain TCP probe with separate send and receive sockets does not
translate directly into two DERP connections. The upstream
[DERP server implementation](https://github.com/tailscale/tailscale/blob/4f86936ef0ec88c6671dc3ad091c1aa0eac7e76c/derp/derpserver/derpserver.go)
keeps duplicate-key connections open but directs received packets to the active
sender. Interleaved duplicate senders can be disabled. A receive-only connection
therefore cannot be assumed to keep the return stream when its duplicate sends.
The [peer receive path](https://github.com/tailscale/tailscale/blob/4f86936ef0ec88c6671dc3ad091c1aa0eac7e76c/wgengine/magicsock/derp.go)
also resolves the DERP source node key to a configured peer; substituting an
unrelated sender key is not a transparent workaround.

No production split-relay implementation was made. It would also have to keep
within the two SDK TLS connections, reserving one for direct transcription.

## Receive-resume comparison

Inspection of the SDK 3.1.2 read implementation confirms that `read` checks the
network receive-resume path even at length zero, while `getBytesAvailable` only
reports the application ring. Evener already calls a zero-length read after an
empty availability poll. The standalone duplex probe previously did not.

The `resume` mode in the [TCP probe](../../check/sdk-tcp/README.md) adds only that
empty-poll read. It leaves the payload, receipt count, read buffer, write admission
and transfer deadlines identical to `duplex`. This comparison distinguishes a
recoverable receive pause in the minimal reproduction from the client's separate
pending-write failure. A passing probe alone would not qualify DERP or voice.

Both ARM and macOS Simulator targets compile. After removable-volume access
became available, all 65 current Evener data files were backed up and
hash-verified. Installing the standalone probe did not change any backed-up
Evener data.

| Physical run | Device verified receive | Mac verified receipts | Outcome |
| --- | ---: | ---: | --- |
| Baseline duplex | 17,764 bytes | 1,170 bytes / 10 receipts | Receive stopped; SDK error and pending count remained zero |
| Duplex with empty-buffer resume reads | 6,500 bytes | 1,170 bytes / 10 receipts | Same stall despite continuous resume polling |

Both transfers advertised a zero receive window to the Mac and failed before
the one-MiB target. The device received a close callback about 30.5 seconds after
its last read. Each host transfer ended with a connection reset. The baseline
server was subsequently interrupted while waiting for an unused second
connection; the first transfer had already recorded failure. The comparison
rules out missing empty-buffer resume polling as a repair for this reproduction.
It does not prove that receive stalls and stuck outgoing writes have one cause.

## Strict request/reply comparison

The `lockstep` probe lets the server send at most 1200 bytes before receiving
one complete 117-byte receipt. Unlike rate pacing, the next block depends on
receipt of the previous block. The device verifies the final short block as
well. Both targets compile; a real loopback transfer first verified the host's
one-MiB accounting and 874-receipt contract.

On the physical device the transfer then completed: 1,048,576 verified bytes and
874 receipts (102,258 bytes) reached their destinations. The server verified
exact bytes and EOF in 60.641 seconds. This establishes a successful request/reply
pattern on one SDK TCP connection. It does not establish that a one-MSS inner
TCP window reproduces that ordering through DERP, or that unsolicited DERP and
discovery traffic can coexist safely. Those require a separate native-client run.

A read-only radio status query during a third receive stall reported an open
TCP connection but did not recover reception. Later queries, after the close
callback, reported active receive mode (`CIPRECVMODE:0`) and no receive length
(`CIPRECVLEN:-1`). Their timing cannot establish where bytes were held during
the stall. No radio configuration was changed.

## Native receive-window comparison

An executable-only install changed the native client's receive window from
16384 to 1200 bytes, matching its 1200-byte MSS. All 65 saved-data files were
hash-verified unchanged at installation; the installed private bundle was backed
up separately. Credentials and saved work were not reset.

The first connection authenticated control, DERP and WireGuard and initialized
AppWire, but did not complete its conversation read. The SDK stalled with 13
pending bytes after 1944 received bytes, reported no SDK error and did not reach
the close fence. Automatic runtime recovery then reopened the same view. Later
connections read eleven chat messages and completed two routine control refreshes,
with about 135 KB and 120 KB received on those DERP connections. These partial
successes do not erase the initial failure. No voice or send test was performed.

A header-only capture on the Hub showed that the 1200-byte window admitted pairs
of 600-byte segments. The client sometimes replenished capacity after only one
of the pair, so traffic remained pipelined. The Hub runs Linux 6.8; upstream
[`tcp_bound_to_half_wnd`](https://github.com/torvalds/linux/blob/v6.8/include/net/tcp.h)
explains the half-window packetization. It does not halve windows at or below
`TCP_MSS_DEFAULT` (536 bytes for IPv4). This motivated a matching 512-byte MSS
and receive window. Both targets compiled and
that candidate was installed by replacing only the executable. The 65 current
saved-data files were backed up again and hash-verified unchanged by installation.

The 108-second native run did not exhibit an SDK pending-write timeout. It did
not complete a fresh conversation read either. The Hub capture showed recurring
61/451-byte or 59/453-byte segment pairs, with delayed acknowledgments and reduced
advertised capacity after the shorter segment. The server reset the first stream
about 30 seconds after its SYN. The actual Hub binary identifies revision
`0ef324a1623a2657b75a4a2a4959c3cbc00fd366` with local modifications; its checkout
contains a 30-second WebSocket write budget. That deadline is consistent with
the captured reset, but there is no server-side error log proving its cause.
A separate authoritative read of the verification conversation returned 107,484
JSON bytes. No server timeout was changed.

The relay completed its subsequent routine control refresh: retirement drained
its pending bytes and both SDK worker fences completed. This is one successful
cleanup, not repeated recovery qualification. Voice and message delivery were
not exercised with this candidate.

## Receive-credit correction

With a 512-byte receive window, lwIP's default window-update threshold withholds
capacity after a short read. Its delayed ACK then reports less than one MSS of
available space, and the Linux sender repeatedly delays the next small write.
The client now advertises the consumed capacity immediately when `tcp_recved`
has left advertised space below the actual available space. It never grants
credit for unread bytes and retains the existing bounded receive window.

The packet-level `build/check/tcp-receive` check injects real IP/TCP packets into
lwIP and drains them through `tn_read`, without sockets, keys or service calls.
Against the prior receive path it fails after the first 61-byte read because no
acknowledgment is emitted. With the correction, 61, 451, 1, 2, 59 and 453-byte
segments each receive an immediate acknowledgment advertising 512 available
bytes. Reading only 32 bytes of a full window advertises exactly 32; consuming
the remaining 480 restores 512. Empty reads produce no packet. Assertion checks
remain enabled independently of the crypto build's `NDEBUG` flag.

Both ARM and Simulator targets build with this correction. It was installed
from `3ad6c16` by replacing only the executable, after a new backup and hash check
of all 65 current saved-data files. The installed executable's SHA-256 is
`8cbf5e1196739ebaac22671f5ab8d2396da85fdc9ad74830c0dbdc7cae44fc88`.
The saved-data hashes were unchanged by installation.

Fresh reads of the verification conversation completed across multiple relay
connections. A header-only Hub trace showed predominantly full 512-byte segments
and prompt receive-credit restoration, with no TCP reset in its 130-second
capture. A separate `itemLimit:24` read measured 74,249 serialized bytes at that
point. The old 40-item measurement above describes a different response window.
One control-map request closed after 12,567 bytes; the client retained its saved
map, reconnected to the relay, read the conversation again and subsequently
completed another authenticated map refresh. This partial control failure remains
part of the result, despite the later successful refreshes.

The reported group conversation also completed a fresh read of seven chat
messages. That view was selected through the client's existing restored-view
launch arguments; the separate session-list check used ordinary B/D-pad actions.
No message was sent to the group conversation. The final storage check found
the same installed executable hash, backed up all 65 current saved-data files,
and confirmed that the crash log was byte-identical to the pre-install copy.
Runtime identity and cache changes were retained; no old data was restored.

## Voice, delivery and remaining stall

The existing saved review draft was sent with one separate A press. Routine
control maintenance interrupted the fresh read needed before dispatch; the
client resumed that read, sent the original requested input and cleared the
draft after acceptance. An independent Hub read found one matching user item
with one mutation ID and the requested reply, `copper`. The device subsequently
showed that reply. There was no second Send press.

A fresh physical microphone test played a 9.75-second synthetic spoken phrase
near the handheld and held A for 14 seconds. The installed client recorded these
boundaries:

- Microphone capture began synchronously with the button action. Network setup
  began 37 ms later, after 232 samples had already been captured.
- The first audio reached the outgoing queue at 4.652 seconds and the first
  caption at 6.019 seconds, while A remained held. A captured Recording screen
  contains the partial phrase.
- Release occurred at 14.005 seconds. The final caption arrived at 15.857 seconds,
  after 111,920 mu-law audio bytes; the device then showed Review message.
- The OpenAI TLS connection coexisted with the established DERP TLS connection
  and retired cleanly after finalization. No third TLS connection was used.

A separate A press sent the reviewed phrase once. The authoritative Hub read
found one matching user item and the requested `amber lantern` reply. However,
the device did not immediately receive that reply: its DERP connection stalled
with 117 pending SDK write bytes after 110,989 received bytes. SDK error remained
zero until the client's progress deadline. Retirement did not reach its worker
fence, and the existing fallback restarted the runtime. This happened about
726 seconds after the executable's initial launch.

The replacement runtime restored the verification conversation with an empty
accepted draft, authenticated again, read fifteen chat messages and displayed
`amber lantern`. No second send was requested or observed. This establishes one
successful recovery after this build's natural write stall. It does **not**
establish repeated restart safety or eliminate the independent SDK failure.
The 512-byte receive-credit correction improves completed reads; it is not a
complete transport repair.

## Display and restart boundary

Jesse reported visual flashing during the native observation. Three passive
screen captures four seconds apart were pixel-identical and did not reproduce
it. USB disappeared and returned during diagnosis; that alone does not establish
a restart or the cause of the flashing.

Code inspection found that the same update which requested `restartGame` still
called its draw routine and returned a display-update result. The current source
returns zero immediately from both restart call sites. Later callbacks retain
their existing guard. ARM and Simulator builds and the existing storage,
socket-ownership and Hub-recovery checks passed. This correction was installed
with the 512-byte candidate and is included in the receive-credit build.
Sixteen additional passive captures before the latter install were pixel-identical.
Jesse then confirmed that the display was steady during the current native run.
The later write stall exercised one automatic restart and restored the conversation.
These observations do not isolate the rendering correction as the cause of the
improvement or establish repeated restart safety. The crash log read before
installation contained no new recorded crash from the flashing interval; USB
disappearance alone cannot establish a crash.

## Idle display composition

Jesse subsequently reported a whole-screen repaint while the conversation was
sitting still. An equal-rate comparison captured 180 USB screen frames at four
frames per second before and after the display change. The baseline contained
one partly painted frame: rows 102 through 239 changed, the lower conversation
and footer were blank, and the next capture restored the complete screen. There
was no logged runtime restart at that point. The other baseline change was
confined to the footer. A USB framebuffer capture is not an optical recording
of the LCD, so this observation alone does not prove what reached the panel.

The application had cleared and redrawn the working display buffer on every
30 Hz update. It now composes each screen in one offscreen bitmap, copies only
changed visible rows into the working buffer, and marks those row ranges for
presentation. An identical frame returns zero from the update callback. Setup,
authorization, navigation and voice share that path. Input, microphone work and
network maintenance retain their existing update schedule. This is completed
frame composition followed by row copies, not an atomic framebuffer swap.

Both ARM and macOS Simulator targets built. A temporary host check exercised
the actual presentation function against an SDK graphics boundary: the working
buffer remained untouched while painting; an unchanged frame requested no
update; disjoint changed rows produced the expected inclusive ranges; different
bitmap/display strides and display padding were respected; and an external
change to the working buffer was restored on the next frame. No configured
Simulator runtime was launched alongside the device's native identity.

Only the physical executable was replaced. All 65 current saved-data files were
backed up and hash-verified unchanged by installation. The installed executable
SHA-256 is
`4d0032bb8ff9c614bfa43702de5b340a462dde693476140ccb37a6a7713f0c93`.

All 180 post-install idle captures had identical conversation pixels (rows
24 through 223); changes were confined to footer rows 227 through 239. No partly
blank conversation frame was observed. Jesse separately confirmed that the
physical display looked steady. Opening and dismissing the system menu, then
locking and unlocking once, returned to a steady conversation according to
Jesse. The serial capture recorded pause/resume and lock events, disconnected
during locking, and was reopened after USB returned. This was one lifecycle
cycle, not repeated recovery qualification. The idle capture also recorded three completed
conversation reads across relay connections and two successful control-map
refreshes. The initial control-map connection had closed after 24,995 bytes;
the saved map allowed the client to connect and read before later maintenance
completed. This remains a display qualification, not a repair of that separate
transport failure or proof that SDK runtime recovery is reliable.

## Conversation and send latency

Jesse reported that a reviewed message remained in preparation, briefly showed
Connected and eventually sent, and that opening another conversation remained
on Loading. The serial capture showed a full `thread/read` before `turn/start`,
then repeated connection closures while reading the newly selected conversation.
The client replaced preparation status with Connected during initialization and
read completion even when send intent was still pending.

Read-only measurements from the Mac separated Hub generation from device
transfer cost. The verification conversation's 24-item response was 88,596 bytes;
its metadata-only response was 16,778. The reported larger conversation's
24-item response was 197,641 bytes; an eight-item response was 151,373, including
about 70 KB of diagnostics and 61 KB of overlays. These Mac reads completed in
10–31 ms. The sizes describe those snapshots, not fixed protocol payload sizes.
The existing `thread/turns/list` endpoint rejected an invented end-of-history
position with `transcriptItemCursorStale`; that attempted shortcut was not
implemented in the client.

Send preparation and uncertain-delivery reconciliation now request only current
thread metadata. They retain the conversation window, older cursor, draft and
original submission identity, and run before the ordinary history refresh.
Metadata does not make cached history authoritative. Initial and older history
pages request eight projected items. The pending composer keeps preparation
visible and does not let another A press restart it. Completed and interrupted
requests log elapsed time and byte counts without message text or credentials.

The focused Hub check failed on the prior code's history-bearing preparation
request. It passes with metadata-only preparation, real draft persistence,
unchanged cached rows/cursor, replay using the original mutation identity,
refusal to replay without mutation authority, and settlement from a recorded
receipt. The existing socket check still passes its one-write, callback-ownership
and two-TLS-slot cases. Both ARM and Simulator targets build.

A first physical candidate used only the smaller requests and status change.
Its installed executable SHA-256 was
`ac0d22449ff9c6ed3c6aebebfc93d4a842a765ffd0b954af5cb1c07a58affe66`.
All 67 saved-data files were backed up and unchanged by installation. Control,
DERP and WireGuard authenticated, but the large conversation read still closed
before completion. Reducing the page size alone did not repair that run.

The next candidate also flushes relay output immediately after the WebSocket
reader consumes native TCP bytes. Previously the receive-credit acknowledgment
waited for the next 30 Hz update before reaching the SDK. The shared output loop
retains its existing budget and outstanding-write gate; it adds no socket or
receive polling. The installed executable SHA-256 is
`61ec16c05c636228e211806cef609b97689cff7ab37454e5a4ab14f92ce04b8e`.
All 68 current saved-data files were backed up and unchanged by installation.

That physical run completed the initial 143,590-byte response in 16.897 seconds,
then a 7,503-byte older page in 0.875 seconds and an 11,620-byte navigation read
in 1.450 seconds. A captured screen displayed the reported conversation's chat.
This is a completed read where the preceding candidate failed, but a roughly
17-second first open is still slow. The existing Hub response includes substantial
diagnostics and overlays the client does not display; no Hub API, server timeout,
radio configuration or firmware was changed.

The final executable adds only a concise pending-send footer to that candidate.
Its installed SHA-256 is
`8533e58300611138fc23f8b6c6c52524ba4d558477637e5a1df4dfef00b63689`.
All 68 current saved-data files were backed up and hash-verified unchanged by
installation. The saved review draft survived replacement of the executable.
The verification conversation's fresh 32,854-byte read completed in 3.508 seconds.

One separate A press requested delivery of that saved review. Its first
metadata request received no response: a 229-byte SDK write remained pending,
and routine Tailscale maintenance interrupted the request after 13.446 seconds.
The close fence did not complete and runtime recovery restarted the app. Without
another Send press, the restored send intent fetched 16,796 bytes of current
metadata in 1.868 seconds and received `turn/start` acceptance 75 ms later.
Independent read-only Hub checks found no matching fixture before the press and
one completed user item with one mutation identity afterward, followed by the
requested `Silver Meadow` reply. The device displayed both. The fast timings
are the successful post-reconnect requests, not the total time from the press.

A first voice fixture had been captured during control maintenance: recording
started immediately and its saved audio produced a complete review, but captions
arrived after release. A second physical microphone run on the final executable
started with DERP established. Capture began synchronously; network setup began
39 ms later with 232 samples already captured. Audio first queued at 4.530 seconds,
and the first caption arrived at 5.736 seconds while A remained held. Release was
at 14.029 seconds and the final review arrived at 15.648 seconds, after 112,105
mu-law bytes. Captured Recording and Review screens contain the spoken fixture.
Three Hub navigation reads completed while transcription was active; the OpenAI
socket retired cleanly while DERP remained connected. The two TLS connections
were DERP and transcription, with no third connection opened during the recording.


A separate A press sent the new review once as the next relay connection came
online. Metadata preparation took 1.769 seconds for 16,690 bytes; acceptance took
76 ms. Independent Hub reads found no matching fixture before that press and
one completed user item with one mutation identity afterward, plus the requested
`Cedar Harbor` reply. Its next history refresh stalled after 234 buffered bytes
and timed out at 35.012 seconds. The SDK held a 234-byte outgoing write and again
failed to complete retirement. Automatic runtime recovery restored the transcript
with an empty accepted draft, completed a fresh 33,352-byte read in 3.873 seconds,
and displayed the reply. No second Send press was used for either fixture.

These runs qualify the smaller preparation request, pending status, preserved
send intent, direct live transcription alongside DERP and eventual replies on
this executable. They also reproduce two SDK write stalls and dependence on
runtime recovery. Neither the roughly 17-second large cold read nor the larger
stall-related delays are resolved. The latest physical screen showed both
accepted messages and replies; no new crash-log inspection was made after those
runtime recoveries.

## Firmware boundary

The [public SDK changelog](https://sdk.play.date/changelog/) lists 3.1.2 as the
latest release checked in this investigation. Its listed HTTP fixes do not
establish a repair for this TCP failure. The radio notification issue cited in
the earlier report remains a hypothesis; neither the private radio implementation
nor the origin of the stalled worker has been established.

No firmware image, radio configuration, protected flash or production Hub was
changed. The device retains its enrolled Tailscale identity. Its SDK runtime
recovery remains experimental and does not satisfy repeated end-to-end recovery
qualification.

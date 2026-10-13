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

Both ARM and Simulator targets build with this correction. It has not yet been
installed: the device again disappeared from USB after the 512-byte run.
The packet check establishes the receive-credit behavior only. A faster native
conversation read, avoidance of SDK stalls, voice and final delivery all remain
unqualified.

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
with the 512-byte candidate. That run did not exercise a runtime restart, so its physical restart behavior and the reported
flicker remain unqualified. The crash log read before installation contained no
new recorded crash from the flashing interval; USB disappearance alone cannot
establish a crash.

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

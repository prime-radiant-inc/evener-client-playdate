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
hash-verified. Only the standalone probe bundle was installed; all backed-up
Evener data remained unchanged.

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

## Firmware boundary

The [public SDK changelog](https://sdk.play.date/changelog/) lists 3.1.2 as the
latest release checked in this investigation. Its listed HTTP fixes do not
establish a repair for this TCP failure. The radio notification issue cited in
the earlier report remains a hypothesis; neither the private radio implementation
nor the origin of the stalled worker has been established.

No firmware image, radio configuration, protected flash, production Hub or
Tailscale identity was changed. The production client's restart behavior is
unchanged. It remains experimental and does not satisfy repeated end-to-end
recovery qualification.

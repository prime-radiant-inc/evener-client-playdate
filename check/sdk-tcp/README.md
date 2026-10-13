# SDK TCP receive probe

This standalone C app calls the Playdate SDK TCP API directly. It does not link
Evener's socket adapter, DERP, WireGuard, TLS, audio or provider code. It contains
no credentials and reads no Evener configuration or saved data.

The Mac server sends one MiB of a fixed byte. The device verifies every byte and
sends fixed 117-byte receipts. The server verifies every receipt byte and the
exact expected count. A device PASS establishes the download; compare the transmitted count with the server's
received count as well. Server `send` counts alone do not establish delivery.

## Build and run

Set `PLAYDATE_SDK_PATH` and put the full ARM GNU toolchain with newlib on `PATH`,
then run `make` in this directory. Both hardware and simulator targets are built.
Install `RawTCP.pdx` as a separate app. It does not replace Evener or use its data
namespace. The physical device and Mac must be on the same LAN.

Start the server with the Mac's LAN address (`ipconfig getifaddr en0`):

```sh
python3 server.py --bind YOUR_MAC_LAN_IP
```

Launch the installed app using the SDK USB console:

```text
run /Games/RawTCP.pdx YOUR_MAC_LAN_IP duplex
```

Allow the local network permission if requested. The primary port is 19184.
Each phase ends after a verified download, an SDK failure, closure or 60 seconds.
Restart the server for each experiment. Its maximum
accept/transfer wait is 120 seconds. TCP window diagnostics use macOS's
`TCP_CONNECTION_INFO` structure.

The launch modes are:

- `duplex`: send ten receipts while receiving; at most one pending write.
- `download`: receive the complete MiB before sending one receipt; start the
  server with `--receipts 1` for this control. The server defaults to ten receipts
  for `duplex` and `fence`.
- `fence`: like duplex, but wait for a queued `network.setEnabled(true)` callback
  after each write before reading or writing again. SDK 3.1.2 dispatches that
  operation on the same worker; it is an implementation-specific ordering check.
- `split`: open a receipt sender on port 19185, then a receiver on 19184. Start
  the server with `--split`. This is a plain TCP isolation check, not a proposed
  second encrypted DERP connection: the client must reserve one of its two TLS
  slots for transcription.
- `restart`: run duplex until five seconds without receive progress, then call
  `system.restartGame` to relaunch the download control. Start the server with
  `--receipts 10 1`. The first interrupted phase fails its transfer check; the
  second must verify the full MiB and one receipt on both ends.
- `blocked`: repeatedly send 4096-byte chunks until an outgoing write stops
  progressing for five seconds. Queue a worker callback, then restart into the
  download control two seconds later. Start the server with `--block-first
  --receipts 1`. The server keeps the first connection open and unread throughout
  the control. Check that both pending bytes and the worker flag were still set
  at restart, and that the replacement transfer verified all bytes.

The server's `--chunk 2400 --interval-ms 33` options provide a paced comparison.
This limits application writes, not the TCP receive window; TCP may coalesce
those writes. Capture both sides and compare the exact byte counts.

The app keeps its TCP objects for their whole runtime. At the end it queues a
worker callback before closing, and never releases the object while a worker
might retain it. Restart modes let the SDK tear down the runtime without local
close/release. That cleanup happens after the measured outcome. All modes
use static input/output buffers and a 64 KiB SDK read buffer. Empty availability
polls do not issue zero-length reads.

These checks are opt-in hardware diagnostics, not part of default tests.
See [the recorded hardware observations](../../docs/qualification/2026-10-12-sdk-tcp.md).

# Playdate SDK 3.1.2 TCP lifetime and recovery report

Prepared for an SDK bug report; not submitted upstream. The reproductions below
contain no credentials, application data, Evener dependencies or external service
requirements. Both use synthetic TCP peers on the same machine or LAN.

There are two independently observed failures. They should not be assumed to
share a root cause: a Simulator callback into unloaded game code, and a physical
TCP worker/receive stall with an unreliable runtime-restart recovery path.

## Simulator: close callback survives its game library

Environment: Playdate Simulator 3.1.2, macOS 27.0 (26A428), Apple silicon.
The [independent minimal C app](../../check/sdk-restart/README.md) opens one plain
TCP connection, installs an empty connection-closed callback, then calls
`system.restartGame`. The host holds the connection open and sends nothing.
There is no application close, release, audio, TLS or other game teardown.

Commands from `check/sdk-restart`, with SDK and ARM toolchain configured:

```sh
make
python3 server.py --seconds 60
```

In another terminal:

```sh
"$PLAYDATE_SDK_PATH/bin/Playdate Simulator.app/Contents/MacOS/Playdate Simulator" \
  "$PWD/TCPRestart.pdx" 127.0.0.1 callback 1
```

Allow network access. The app requests restart two seconds after the TCP open
callback. Expected: the new instance initializes and opens its own connection,
with no callback into the unloaded instance.

Observed: four callback-enabled attempts crashed on the first restart. The final
instrumented attempt logged these addresses:

```text
LIFECYCLE INIT mode=callback remaining=1 callback=0x11ee7cb54
LIFECYCLE OPEN error=0 remaining=1
LIFECYCLE RESTART remaining=1
LIFECYCLE TERMINATE restarting=1
LIFECYCLE INIT mode=callback remaining=0 callback=0x12181cb54
EXC_BAD_ACCESS address=0x11ee7cb54
#0 0x11ee7cb54
#1 notifyConnectionClosed + 44
#2 callRunloopCallbacks + 224
#3 pd_update + 440
```

The failing instruction address exactly matches the **previous** library's empty
callback, while the new library's callback is elsewhere. This establishes the
stale callback target in this reproduction. The callback body does not access
application or SDK state.

Control: replacing `callback 1` with `none 5` omitted callback registration and
completed all five automatic restarts, six host accepts, and the final
`READY remaining=0`. The physical build forcibly retains the callback; omission
is not a proposed hardware workaround because an earlier C trampoline diagnostic
found a null callback unsafe there.

On physical firmware 3.1.2, the same callback-enabled app completed five restarts
and opened its sixth connection. Each load used callback address `0x60000fd1`.
That does not show that hardware has correct callback ownership, only that this
particular Simulator failure did not reproduce in that device run. The host did
not receive EOF for the old physical connections before its deadline.

The scenario was motivated by the [SDK 3.1.1 restart report](https://devforum.play.date/t/restarting-game-with-active-tcp-connection-can-crash-simulator-in-certain-conditions/25933).
This implementation is independent of the posted code. The current reproduction
extends the observation to Simulator 3.1.2 and identifies the stale target.

## Hardware: TCP stall and blocked-worker restart

Environment: physical Playdate, PCB 0x01, firmware
`1a63be33-3.1.2-release.213590-gitlab-runner-260916`; SDK 3.1.2 C API.
The [standalone transfer app](../../check/sdk-tcp/README.md) uses plain TCP and
static buffers, checks every downloaded byte, and emits fixed 117-byte receipts.

Ordinary duplex reproduction, from `check/sdk-tcp`:

```sh
make
python3 server.py --bind YOUR_MAC_LAN_IP
```

Launch through the SDK USB console:

```text
run /Games/RawTCP.pdx YOUR_MAC_LAN_IP duplex
```

The peer sends one MiB while the device emits ten receipts. Earlier physical
runs stopped receiving after 2,920–19,064 bytes, with no SDK error and no pending
application send in some cases. The host observed a zero TCP receive window.
A download-first control verified the full MiB; separate plain receive/send
sockets also passed. A queued completion callback after each write did not
prevent the receive stall. Exact cases are in the
[original qualification record](2026-10-12-sdk-tcp.md).

A separate deterministic pressure mode exercises recovery from an SDK worker
blocked on outgoing traffic:

```sh
python3 server.py --bind YOUR_MAC_LAN_IP --block-first --receipts 1
```

```text
run /Games/RawTCP.pdx YOUR_MAC_LAN_IP blocked
```

The host accepts a first connection but never reads it, including during recovery.
The device queues 4,096-byte writes one at a time. After five seconds without
send progress it queues `network.setEnabled(true, callback)`, then waits two
more seconds before restarting into the download control. The queued callback
is a diagnostic completion fence for this SDK implementation, not a documented
public cancellation API. At restart, both the pending write and callback must
still be outstanding. The new connection must download exactly 1,048,576 bytes
and deliver exactly 117 valid receipt bytes.

On October 13, the first repeated-pressure cycle passed. It restarted with
4,096 pending bytes, SDK error zero, and the worker callback outstanding. The
post-restart transfer verified the full MiB and one receipt. The second cycle
reached the same blocked state and initialized the replacement runtime, then
rebooted before its new connection opened. It added this crash entry:

```text
UTC 2026-10-13 16:38:44
r0=2003a23c r1=00000051 r2=2003a23c r3=00001028
r12=00000002 lr=080620b5 pc=08061b20 psr=410f0000
cfsr=00000400 hfsr=40000000 mmfar=00000000 bfar=00000000
heap allocated=175872
```

The SDK's bundled `symbols.db`, hardware ID 0, maps the PC to `fREe` in
`/Core/dlmalloc.c` and the LR to `dlfree`. The probe contains no application
TCP release or explicit heap free. The exact allocation and worker task at
fault are not established by the register dump alone.

The comparison was repeated with `restarting` guards that return from update and
system-event handlers after the restart request, before drawing or scheduling
any cleanup. Asynchronous callbacks remain installed; they may update flags or
log, but do not free or close connections on this path. Again, the first cycle
verified the full MiB and receipt, and the second rebooted after the replacement
runtime began. That added a second allocator fault:

```text
UTC 2026-10-13 16:45:08
r0=60c060c0 r1=00000051 r2=00000018 r3=00001028
r12=00000003 lr=080620b5 pc=08061b10 psr=410f5c00
cfsr=01000000 hfsr=40000000 mmfar=00000000 bfar=00000000
heap allocated=175936
```

The PC again maps to `fREe`, with LR in `dlfree`. Suppressing post-request
rendering and system-event work did not prevent the failure. Each sequence was
stopped on its first failure; neither completed its intended repeated-recovery
qualification. In both failures, the host kept the pressure connection unread,
and did not observe the replacement connection. The exact corruption origin
still requires SDK-side investigation. These allocator faults are not evidence
of the Simulator's specific stale-code-address fault on hardware.

All 62 saved Evener data files stayed byte-identical throughout the standalone
probes. The production client executable was not replaced. The diagnostic apps
were backed up, verified and removed afterward.

## Requested SDK clarification or fix

- Drain or invalidate queued close notifications before unloading the game
  library; a notification must never invoke a prior runtime's code address.
- Ensure runtime teardown cancels or joins pending TCP work before freeing
  memory it can still access. Provide guidance for recovering when a write
  and a queued completion callback never finish.
- Investigate the physical duplex receive stall independently. The older
  [hardware TCP reception report](https://devforum.play.date/t/os-2-7-2-playdate-stops-receiving-data-via-tcp/22675)
  has similar symptoms, but does not establish the same cause in 3.1.2.

The application needs one encrypted connection for its relay and one for speech.
A workaround requiring a third TLS connection would not meet that constraint.
No firmware, radio configuration, protected flash, Hub or production server was
modified during these reproductions. Raw device data and configured application
bundles are excluded from the report and repository.

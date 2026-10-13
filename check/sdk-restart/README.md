# SDK TCP callback lifetime probe

This standalone C program opens one plain TCP connection, installs an empty
close callback and asks the SDK to restart the runtime. It sends no data, uses
no credentials, and does not link Evener. Its separate bundle and data namespace
do not replace the client. The host records each connection and disconnect.

The reproduction was motivated by [the public SDK 3.1.1 report](https://devforum.play.date/t/restarting-game-with-active-tcp-connection-can-crash-simulator-in-certain-conditions/25933).
This is an independent implementation of that scenario. It additionally logs
callback addresses and supports a bounded automatic restart sequence.

## Run

Set `PLAYDATE_SDK_PATH` and the ARM toolchain `PATH`, then run `make` here.
Both device and Simulator targets build. Start a bounded loopback server:

```sh
python3 server.py --seconds 60
```

Launch the Simulator executable with the bundle and arguments:

```sh
"$PLAYDATE_SDK_PATH/bin/Playdate Simulator.app/Contents/MacOS/Playdate Simulator" \
  "$PWD/TCPRestart.pdx" 127.0.0.1 callback 1
```

Allow the network prompt. After opening, the probe waits two seconds, requests
one runtime restart. If it succeeds, the next open emits `READY remaining=0`;
a reproduced fault can stop the sequence before that event.
A restarts manually; the Simulator restart control exercises its own reload path.
There is no application close/release or other teardown on termination.

Repeat with `none 5` as the last two arguments for five restarts without registering
the close callback. This control is **Simulator only**: the device build always
installs the callback because earlier hardware diagnostics found a null callback
unsafe in the SDK's C close trampoline.

For hardware, use `--bind YOUR_MAC_LAN_IP` on the server, install `TCPRestart.pdx`
separately, and launch through the SDK USB console:

```text
run /Games/TCPRestart.pdx YOUR_MAC_LAN_IP callback 5
```

The server listens on port 19186, holds connections open without sending data,
and exits at its deadline. These are explicit diagnostics, not default tests.
A completed restart sequence must have all expected `INIT`, `RESTART` and final
`READY` events, plus corresponding host accepts; a still-running process alone
is not a pass. The last connection closes when the server reaches its deadline.

On macOS, LLDB can capture a crash even when Simulator does not leave a standard
DiagnosticReports file. Use `--batch -o run -k bt -k 'image lookup -n closed'`
before `--`, the Simulator executable and the same bundle arguments.

The [SDK report](../../docs/qualification/sdk-network-report.md) distinguishes
this Simulator callback fault from the device's duplex/worker stall.

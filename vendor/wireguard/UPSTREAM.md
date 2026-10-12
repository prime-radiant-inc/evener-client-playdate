WireGuard core from https://github.com/smartalock/wireguard-lwip

Pinned commit: c54f20dbe76ac8b3411ad21e0ed7deea6f0cfd4d

Only the portable core and reference C crypto are included. The lwIP integration
file is omitted. In wireguard.h, four lwIP includes are replaced with the opaque
network pointer declarations and IPv4 metadata types used by the core.

Compile x25519.c with BYTE_ORDER=1234 and LITTLE_ENDIAN=1234 on Playdate.
The source includes its BSD-3-Clause license and crypto notices.

In x25519.c, mul declares its third parameter as a pointer rather than an
eight-limb array: nb already bounds accesses, including the one-limb a24 input.
This removes GCC 15's misleading array-size diagnostic; operations are unchanged.

The replay-window distance uses uint64_t, matching the packet counters. A
32-bit distance can wrap after a large counter gap and accept an old packet.

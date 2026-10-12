Official TweetNaCl C release 20140427 (public domain).

Sources:
- https://tweetnacl.cr.yp.to/20140427/tweetnacl.c
- https://tweetnacl.cr.yp.to/20140427/tweetnacl.h

SHA-256 before local changes:
- tweetnacl.c: 02e65bc3013ff2168983365e55906bc783c4c7e0a60d8100f17bb303a17175c4
- tweetnacl.h: 43f29ad721d9927b747b0100ab4160c119e7bb180c7c98a66e4bf79d31244287

The caller supplies randombytes(unsigned char *, unsigned long long).

The unused Ed25519 functions and their helpers/constants are guarded by
TWEETNACL_ENABLE_ED25519, which is unset for this box-only build. Their arithmetic
is unchanged. This omits an unused 1904-byte-stack signature function from the
firmware compilation; the box, XSalsa20, Poly1305, and X25519 code is unchanged.
SHA512 is compiled for the acoustic setup comparison code; its arithmetic and
hash implementation are unchanged from upstream.

For Cortex-M7 GCC builds, compile this source with -fno-inline. GCC 15.3 at -O2
otherwise inlines helpers into X25519 and exceeds the firmware frame limit.
With -fno-inline, the largest .su frame is 1472 bytes; the conservative box
call chain sums to 2044 bytes before caller/libc frames.

Official lwIP 2.2.1, tag STABLE-2_2_1_RELEASE.
https://github.com/lwip-tcpip/lwip/releases/tag/STABLE-2_2_1_RELEASE
Archive: https://codeload.github.com/lwip-tcpip/lwip/tar.gz/refs/tags/STABLE-2_2_1_RELEASE
Archive SHA-256: ce0b7461c0ad9602c376f0bf07c5eb7253b48c7bf66f011c6bf3e2a96731c539

The complete src/include, src/core/*.c and src/core/ipv4 directories are
included verbatim. Platform options and architecture definitions live in the
client src/lwipopts.h and src/arch/cc.h. COPYING contains the BSD license.
Only TCP/IPv4 core sources listed in transport.mk are built; no sockets or OS.

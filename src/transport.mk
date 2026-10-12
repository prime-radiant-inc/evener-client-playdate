# Pure C transport; the platform supplies clocks, randomness and logging.
TAILNET_SRC = src/tailnet.c src/tailnet_wg.c src/tailnet_wg_platform.c \
    vendor/wireguard/wireguard.c vendor/wireguard/crypto.c \
    vendor/wireguard/crypto/refc/blake2s.c vendor/wireguard/crypto/refc/chacha20.c \
    vendor/wireguard/crypto/refc/chacha20poly1305.c vendor/wireguard/crypto/refc/poly1305-donna.c \
    vendor/wireguard/crypto/refc/x25519.c vendor/tweetnacl/tweetnacl.c \
    vendor/lwip/src/core/init.c vendor/lwip/src/core/def.c vendor/lwip/src/core/inet_chksum.c \
    vendor/lwip/src/core/ip.c vendor/lwip/src/core/mem.c vendor/lwip/src/core/memp.c \
    vendor/lwip/src/core/netif.c vendor/lwip/src/core/pbuf.c vendor/lwip/src/core/stats.c \
    vendor/lwip/src/core/sys.c vendor/lwip/src/core/timeouts.c \
    vendor/lwip/src/core/tcp.c vendor/lwip/src/core/tcp_in.c vendor/lwip/src/core/tcp_out.c \
    vendor/lwip/src/core/ipv4/ip4.c vendor/lwip/src/core/ipv4/ip4_addr.c
TAILNET_INCDIR = src vendor/wireguard vendor/tweetnacl vendor/lwip/src/include
TAILNET_DEFS = -DBYTE_ORDER=1234 -DLITTLE_ENDIAN=1234
# json.c is shared with the application; include it exactly once.
# ARM: add -fstack-usage -Werror=stack-usage=1536 globally; TweetNaCl needs
# -fno-inline to bound its nested X25519 frames on the firmware's 10 KB stack.

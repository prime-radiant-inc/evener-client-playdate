# Authenticated Tailscale control; platform provides verified TLS and persistence.
# The WireGuard crypto sources in transport.mk are shared; compile them once.
CONTROL_SRC = src/control.c vendor/nghttp2/nghttp2_hd.c \
    vendor/nghttp2/nghttp2_hd_huffman.c vendor/nghttp2/nghttp2_hd_huffman_data.c \
    vendor/nghttp2/nghttp2_buf.c vendor/nghttp2/nghttp2_mem.c \
    vendor/nghttp2/nghttp2_rcbuf.c vendor/nghttp2/nghttp2_helper.c
CONTROL_INCDIR = vendor/nghttp2/include
CONTROL_DEFS = -DNGHTTP2_STATICLIB -DNGHTTP2_EMBEDDED -DNDEBUG

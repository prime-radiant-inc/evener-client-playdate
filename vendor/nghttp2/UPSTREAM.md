HPACK decoder subset from nghttp2 1.67.1.

Source: https://github.com/nghttp2/nghttp2/releases/tag/v1.67.1
Archive: https://github.com/nghttp2/nghttp2/releases/download/v1.67.1/nghttp2-1.67.1.tar.xz

Archive SHA-256: `153972aad57e7bf9d911666df7613f2390acf37ea7e1a97a0c5567e90f98e830`.

Only header compression sources and their support dependencies are included.
Local adaptation in `nghttp2_net.h`: `NGHTTP2_EMBEDDED` selects the upstream
portable network helpers and namespaces their byte-order functions to avoid
system macro collisions, without a socket dependency.
Build with include/ on the include path and NGHTTP2_STATICLIB, NGHTTP2_EMBEDDED, NDEBUG.

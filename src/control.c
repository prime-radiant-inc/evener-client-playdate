#include "control.h"
#include "json.h"
#include "crypto.h"
#include <nghttp2/nghttp2.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Wire format follows Tailscale v1.104.1 controlbase and ts2021. The client uses
 * full, uncompressed map responses so each refresh is independently applicable. */
#define CONTROL_VERSION 148
#define RECORD_MAX 4077
#define WIRE_CAP 32768
#define HTTP_CAP 4096
#define FRAME_CAP 32768
#define HEADER_CAP 16384
#define RENEW_WINDOW_MS (7ULL * 24 * 60 * 60 * 1000)

typedef struct {
    ControlConfig config, result;
    ControlState state;
    ControlStage stage;
    void *stage_context;
    char error[160], auth_url[1024];
    uint32_t started;
    uint8_t h[32], ck[32], ephemeral[32], tx_key[32], rx_key[32];
    uint64_t tx_nonce, rx_nonce, rx_wire_bytes;
    uint8_t wire[WIRE_CAP], incoming[WIRE_CAP], frames[FRAME_CAP];
    size_t wire_length, incoming_length, frames_length;
    uint8_t headers[HEADER_CAP];
    size_t headers_length;
    uint8_t *body;
    size_t body_length;
    nghttp2_hd_inflater *inflater;
    unsigned stream, status;
    bool upgraded, noise_ready, early_done, header_open, end_headers, headers_end_stream;
    bool key_http, chunked, relay_reconciled, current_only, authorized;
    size_t content_length, chunk_remaining, upgrade_header_length;
    char request[2048];
} Control;
static Control *c;
static ControlState stopped_state = CONTROL_STOPPED;

static unsigned be16(const uint8_t *p) { return ((unsigned)p[0] << 8) | p[1]; }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static void put32(uint8_t *p, uint32_t n) { p[0] = n >> 24; p[1] = n >> 16; p[2] = n >> 8; p[3] = n; }
static uint64_t nonce_be(uint64_t n) {
    uint64_t reversed = 0;
    for (int i = 0; i < 8; i++) { reversed = (reversed << 8) | (n & 255); n >>= 8; }
    return reversed;
}
static bool nonzero(const uint8_t *p) { uint8_t n = 0; for (int i = 0; i < 32; i++) n |= p[i]; return n != 0; }
static void hex(char out[65], const uint8_t *key) {
    const char *digits = "0123456789abcdef";
    for (int i = 0; i < 32; i++) { out[i * 2] = digits[key[i] >> 4]; out[i * 2 + 1] = digits[key[i] & 15]; }
    out[64] = 0;
}
static bool unhex(uint8_t *out, const char *s) {
    if (strlen(s) != 64) return false;
    for (int i = 0; i < 32; i++) {
        unsigned value = 0;
        for (int j = 0; j < 2; j++) {
            unsigned char ch = (unsigned char)s[i * 2 + j];
            if (!isxdigit(ch)) return false;
            value = value * 16 + (isdigit(ch) ? ch - '0' : tolower(ch) - 'a' + 10);
        }
        out[i] = value;
    }
    return nonzero(out);
}
static bool key_field(const Json *j, int obj, const char *field, const char *prefix, uint8_t *key) {
    char text[80];
    return json_field(j, obj, field, text, sizeof(text)) && !strncmp(text, prefix, strlen(prefix)) && unhex(key, text + strlen(prefix));
}
static bool raw_key(const Json *j, const char *field, uint8_t *key) { return key_field(j, 0, field, "", key); }
static bool ip4(uint8_t *out, const char *s) {
    unsigned a, b, d, e; int n = 0;
    if (sscanf(s, "%u.%u.%u.%u%n", &a, &b, &d, &e, &n) != 4 || a > 255 || b > 255 || d > 255 || e > 255) return false;
    if (s[n] && strcmp(s + n, "/32")) return false;
    out[0] = a; out[1] = b; out[2] = d; out[3] = e;
    return true;
}
/* Civil date conversion avoids a platform timezone or libc time_t dependency. */
static uint64_t timestamp(const char *s) {
    int y, m, d, hour, minute, sec, n = 0;
    if (sscanf(s, "%d-%d-%dT%d:%d:%d%n", &y, &m, &d, &hour, &minute, &sec, &n) != 6) return UINT64_MAX;
    if (y == 1) return 0;
    if (y < 1970 || y > 9999 || m < 1 || m > 12 || d < 1 || d > 31 || hour < 0 || hour > 23 || minute < 0 || minute > 59 || sec < 0 || sec > 59) return UINT64_MAX;
    if (s[n] == '.') { n++; while (isdigit((unsigned char)s[n])) n++; }
    if (strcmp(s + n, "Z")) return UINT64_MAX;
    y -= m <= 2;
    int era = y / 400, year = y - era * 400;
    int day = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t days = (int64_t)era * 146097 + year * 365 + year / 4 - year / 100 + day - 719468;
    return (uint64_t)(days * 86400 + hour * 3600 + minute * 60 + sec) * 1000;
}
static uint64_t expiry(const Json *j, int obj, const char *field) {
    char value[64]; int t = json_get(j, obj, field);
    if (t < 0 || json_is(j, t, "null")) return 0;
    return json_text(j, t, value, sizeof(value)) ? timestamp(value) : UINT64_MAX;
}

bool control_configure(ControlConfig *out, const char *text, size_t length) {
    Json j;
    if (!out || !tn_configure(&out->tailnet, text, length) || !json_parse(&j, text, length)) return false;
    bool ok = raw_key(&j, "machine_private_key", out->machine_private) && raw_key(&j, "control_public_key", out->control_public) &&
        json_field(&j, 0, "peer_id", out->peer_id, sizeof(out->peer_id)) && out->peer_id[0] &&
        json_field(&j, 0, "node_id", out->node_id, sizeof(out->node_id)) && out->node_id[0] &&
        json_field(&j, 0, "enrolled_os", out->enrolled_os, sizeof(out->enrolled_os)) && out->enrolled_os[0];
    if (!json_field(&j, 0, "hostname", out->hostname, sizeof(out->hostname))) snprintf(out->hostname, sizeof(out->hostname), "playdate-derp-spike");
    memset(out->staged_node_private, 0, 32);
    if (json_get(&j, 0, "staged_node_private_key") >= 0) ok = ok && raw_key(&j, "staged_node_private_key", out->staged_node_private);
    out->node_expiry_ms = expiry(&j, 0, "node_key_expiry");
    out->peer_expiry_ms = expiry(&j, 0, "peer_key_expiry");
    out->derp_region = (unsigned)json_int(&j, json_get(&j, 0, "derp_region_id"));
    ok = ok && out->node_expiry_ms != UINT64_MAX && out->peer_expiry_ms != UINT64_MAX && out->derp_region > 0 && out->derp_region < 65536;
    json_free(&j);
    return ok;
}

void control_stop(void) {
    if (c) {
        if (c->inflater) nghttp2_hd_inflate_del(c->inflater);
        if (c->body) { crypto_zero(c->body, CONTROL_MAX_MAP + 1); free(c->body); }
        crypto_zero(c, sizeof(*c)); free(c); c = NULL;
    }
    stopped_state = CONTROL_STOPPED;
}
void control_fail(const char *reason) {
    if (!c) { stopped_state = CONTROL_FAILED; return; }
    snprintf(c->error, sizeof(c->error), "%s", reason);
    c->state = CONTROL_FAILED;
}
ControlState control_state(void) { return c ? c->state : stopped_state; }
const char *control_error(void) { return c ? c->error : "Control stopped"; }
const char *control_auth_url(void) { return c && c->state == CONTROL_REAUTH ? c->auth_url : ""; }
bool control_authorized(void) { return c && c->authorized; }
const ControlConfig *control_result(void) { return c && (c->state == CONTROL_READY || c->state == CONTROL_KEY_READY) ? &c->result : NULL; }
const uint8_t *control_output(size_t *length) { *length = c ? c->wire_length : 0; return c ? c->wire : NULL; }
void control_consume(size_t n) { if (c && n <= c->wire_length) { memmove(c->wire, c->wire + n, c->wire_length - n); c->wire_length -= n; } }
static bool queue(const void *p, size_t n) {
    if (n > sizeof(c->wire) - c->wire_length) { control_fail("Control output overflow"); return false; }
    memcpy(c->wire + c->wire_length, p, n); c->wire_length += n; return true;
}
static bool record(const uint8_t *p, size_t n) {
    while (n) {
        size_t part = n > RECORD_MAX ? RECORD_MAX : n;
        if (c->tx_nonce == UINT64_MAX || c->wire_length + part + 19 > sizeof(c->wire)) { control_fail("Control record output overflow"); return false; }
        uint8_t *out = c->wire + c->wire_length;
        out[0] = 4; out[1] = (part + 16) >> 8; out[2] = (part + 16) & 255;
        wireguard_aead_encrypt(out + 3, p, part, NULL, 0, nonce_be(c->tx_nonce++), c->tx_key);
        c->wire_length += part + 19; p += part; n -= part;
    }
    return true;
}
static bool frame(unsigned type, unsigned flags, unsigned stream, const uint8_t *p, size_t n) {
    uint8_t hdr[9] = { n >> 16, n >> 8, n, type, flags, 0, 0, 0, 0 };
    put32(hdr + 5, stream);
    return record(hdr, sizeof(hdr)) && (!n || record(p, n));
}
static void mix_hash(const void *p, size_t n) {
    blake2s_ctx h;
    blake2s_init(&h, 32, NULL, 0); blake2s_update(&h, c->h, 32); blake2s_update(&h, p, n); blake2s_final(&h, c->h);
}
/* Noise HKDF uses HMAC-BLAKE2s, which is distinct from keyed BLAKE2s. */
static void hmac(uint8_t out[32], const uint8_t key[32], const void *a, size_t an, const void *b, size_t bn) {
    uint8_t pad[64], digest[32]; blake2s_ctx h;
    for (int i = 0; i < 64; i++) pad[i] = (i < 32 ? key[i] : 0) ^ 0x36;
    blake2s_init(&h, 32, NULL, 0); blake2s_update(&h, pad, 64); blake2s_update(&h, a, an); blake2s_update(&h, b, bn); blake2s_final(&h, digest);
    for (int i = 0; i < 64; i++) pad[i] ^= 0x36 ^ 0x5c;
    blake2s_init(&h, 32, NULL, 0); blake2s_update(&h, pad, 64); blake2s_update(&h, digest, 32); blake2s_final(&h, out);
    crypto_zero(pad, sizeof(pad)); crypto_zero(digest, sizeof(digest));
}
static void hkdf(const void *input, size_t n, uint8_t first[32], uint8_t second[32]) {
    uint8_t prk[32], one = 1, two = 2;
    hmac(prk, c->ck, input, n, NULL, 0);
    hmac(first, prk, &one, 1, NULL, 0); hmac(second, prk, first, 32, &two, 1);
    crypto_zero(prk, sizeof(prk));
}
static bool mix_dh(const uint8_t private[32], const uint8_t public[32], uint8_t key[32]) {
    uint8_t dh[32], next[32];
    wireguard_x25519(dh, private, public);
    if (!nonzero(dh)) { control_fail("Control rejected invalid X25519 key"); return false; }
    hkdf(dh, 32, next, key); memcpy(c->ck, next, 32);
    crypto_zero(dh, 32); crypto_zero(next, 32); return true;
}
static void public_key(uint8_t out[32], const uint8_t private[32]) { static const uint8_t base[32] = {9}; wireguard_x25519(out, private, base); }
static void new_key(uint8_t key[32]) { tn_random(key, 32); key[0] &= 248; key[31] = (key[31] & 127) | 64; }
static char peer_target[256], enrollment_url[1024];
void control_set_peer_target(const char *host) {
    snprintf(peer_target, sizeof(peer_target), "%s", host);
    size_t n = strlen(peer_target);
    if (n && peer_target[n - 1] == '.') peer_target[--n] = 0;
    for (size_t i = 0; i < n; i++) peer_target[i] = (char)tolower((unsigned char)peer_target[i]);
}
void control_set_auth_url(const char *url) { snprintf(enrollment_url, sizeof(enrollment_url), "%s", url); }
void control_create_identity(ControlConfig *out, uint16_t peer_port) {
    memset(out, 0, sizeof(*out));
    new_key(out->machine_private); new_key(out->tailnet.node_private); new_key(out->tailnet.disco_private);
    public_key(out->tailnet.node_public, out->tailnet.node_private);
    public_key(out->tailnet.disco_public, out->tailnet.disco_private);
    tn_random(out->tailnet.seed, sizeof(out->tailnet.seed));
    out->tailnet.peer_port = peer_port; out->derp_region = out->tailnet.derp_region = 1;
    snprintf(out->hostname, sizeof(out->hostname), "playdate-%02x%02x%02x", out->tailnet.node_public[0], out->tailnet.node_public[1], out->tailnet.node_public[2]);
    snprintf(out->enrolled_os, sizeof(out->enrolled_os), "playdate");
}
static void base64(char *out, const uint8_t *p, size_t n) {
    static const char chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t used = 0;
    for (size_t i = 0; i < n; i += 3) {
        unsigned a = p[i], b = i + 1 < n ? p[i + 1] : 0, d = i + 2 < n ? p[i + 2] : 0;
        out[used++] = chars[a >> 2]; out[used++] = chars[((a & 3) << 4) | (b >> 4)];
        out[used++] = i + 1 < n ? chars[((b & 15) << 2) | (d >> 6)] : '=';
        out[used++] = i + 2 < n ? chars[d & 63] : '=';
    }
    out[used] = 0;
}
static bool begin(const ControlConfig *config) {
    Control *next = calloc(1, sizeof(*next));
    if (!next) { control_fail("Cannot allocate control engine"); return false; }
    next->config = *config; next->result = *config;
    control_stop(); c = next;
    c->body = malloc(CONTROL_MAX_MAP + 1);
    if (!c->body) { control_fail("Cannot allocate control response"); return false; }
    c->started = tn_now_ms();
    return true;
}
bool control_start_key(const ControlConfig *config) {
    if (!begin(config)) return false;
    c->state = CONTROL_KEY_FETCH;
    int n = snprintf(c->request, sizeof(c->request), "GET /key?v=%d HTTP/1.1\r\nHost: " CONTROL_HOST "\r\nConnection: close\r\nAccept-Encoding: identity\r\n\r\n", CONTROL_VERSION);
    return queue(c->request, (size_t)n);
}
static bool start_noise(const ControlConfig *config, ControlStage stage, void *context, bool current_only) {
    uint8_t init[101], key[32], pub[32]; char encoded[137], prologue[64];
    if (!begin(config)) return false;
    config = &c->config;
    c->state = CONTROL_CONNECTING; c->stage = stage; c->stage_context = context; c->current_only = current_only;
    if (!nonzero(config->machine_private) || !nonzero(config->control_public)) { control_fail("Missing isolated control identity"); return false; }
    blake2s(c->h, 32, NULL, 0, "Noise_IK_25519_ChaChaPoly_BLAKE2s", 33); memcpy(c->ck, c->h, 32);
    int n = snprintf(prologue, sizeof(prologue), "Tailscale Control Protocol v%d", CONTROL_VERSION); mix_hash(prologue, (size_t)n); mix_hash(config->control_public, 32);
    init[0] = CONTROL_VERSION >> 8; init[1] = CONTROL_VERSION & 255; init[2] = 1; init[3] = 0; init[4] = 96;
    new_key(c->ephemeral); public_key(init + 5, c->ephemeral); mix_hash(init + 5, 32);
    if (!mix_dh(c->ephemeral, config->control_public, key)) return false;
    public_key(pub, config->machine_private); wireguard_aead_encrypt(init + 37, pub, 32, c->h, 32, 0, key); mix_hash(init + 37, 48);
    if (!mix_dh(config->machine_private, config->control_public, key)) return false;
    wireguard_aead_encrypt(init + 85, NULL, 0, c->h, 32, 0, key); mix_hash(init + 85, 16); crypto_zero(key, 32);
    base64(encoded, init, sizeof(init));
    n = snprintf(c->request, sizeof(c->request), "POST /ts2021 HTTP/1.1\r\nHost: " CONTROL_HOST "\r\nConnection: Upgrade\r\nUpgrade: tailscale-control-protocol\r\nX-Tailscale-Handshake: %s\r\nContent-Length: 0\r\n\r\n", encoded);
    return queue(c->request, (size_t)n);
}

bool control_start(const ControlConfig *config, ControlStage stage, void *context) { return start_noise(config, stage, context, false); }
bool control_start_current(const ControlConfig *config) { return start_noise(config, NULL, NULL, true); }

static size_t hpack_integer(uint8_t *p, unsigned prefix, unsigned bits, size_t value) {
    unsigned limit = (1u << bits) - 1; size_t n = 1;
    if (value < limit) { p[0] = prefix | value; return n; }
    p[0] = prefix | limit; value -= limit;
    while (value >= 128) { p[n++] = (value & 127) | 128; value >>= 7; }
    p[n++] = value; return n;
}
static size_t hpack_string(uint8_t *p, const char *s) {
    size_t length = strlen(s), n = hpack_integer(p, 0, 7, length);
    memcpy(p + n, s, length); return n + length;
}
static size_t hpack_literal(uint8_t *p, unsigned index, const char *name, const char *value) {
    size_t n = hpack_integer(p, 0, 4, index);
    if (!index) n += hpack_string(p + n, name);
    return n + hpack_string(p + n, value);
}
static bool request(bool registration) {
    char node[65], old[65], disco[65], hostname[260], os[68]; uint8_t pub[32]; size_t hn = 0;
    const uint8_t *private = (!c->current_only && nonzero(c->config.staged_node_private)) ? c->config.staged_node_private : c->config.tailnet.node_private;
    public_key(pub, private); hex(node, pub); hex(old, c->config.tailnet.node_public); hex(disco, c->config.tailnet.disco_public);
    if (!json_quote(c->config.hostname, hostname, sizeof(hostname)) || !json_quote(c->config.enrolled_os, os, sizeof(os))) { control_fail("Invalid control hostname"); return false; }
    int n;
    if (registration) {
        c->state = CONTROL_REGISTERING;
        static char followup[2050];
        if (!json_quote(c->current_only ? "" : enrollment_url, followup, sizeof(followup))) { control_fail("Invalid enrollment URL"); return false; }
        n = snprintf(c->request, sizeof(c->request), "{\"Version\":%d,\"NodeKey\":\"nodekey:%s\",\"OldNodeKey\":\"nodekey:%s\",\"Followup\":%s,\"Hostinfo\":{\"Hostname\":%s,\"OS\":%s,\"BackendLogID\":\"playdate-native\"}}", CONTROL_VERSION, node, old, followup, hostname, os);
    } else {
        c->state = CONTROL_MAPPING;
        n = snprintf(c->request, sizeof(c->request), "{\"Version\":%d,\"NodeKey\":\"nodekey:%s\",\"Stream\":false,\"DiscoKey\":\"discokey:%s\",\"Hostinfo\":{\"Hostname\":%s,\"OS\":%s,\"BackendLogID\":\"playdate-native\",\"NetInfo\":{\"PreferredDERP\":%u}}}", CONTROL_VERSION, node, disco, hostname, os, c->config.derp_region);
    }
    if (n < 0 || (size_t)n >= sizeof(c->request)) { control_fail("Control request exceeds bound"); return false; }
    /* Never index identity headers: no dynamic encoder state is necessary. */
    c->headers[hn++] = 0x83; c->headers[hn++] = 0x87;
    hn += hpack_literal(c->headers + hn, 1, NULL, CONTROL_HOST);
    hn += hpack_literal(c->headers + hn, 4, NULL, registration ? "/machine/register" : "/machine/map");
    hn += hpack_literal(c->headers + hn, 31, NULL, "application/json");
    snprintf(hostname, sizeof(hostname), "nodekey:%s", node);
    hn += hpack_literal(c->headers + hn, 0, "ts-lb", hostname);
    c->stream += 2; c->status = 0; c->body_length = 0; c->headers_length = 0; c->end_headers = false;
    return frame(1, 4, c->stream, c->headers, hn) && frame(0, 1, c->stream, (uint8_t *)c->request, (size_t)n);
}
static void reauth(const char *reason) { snprintf(c->error, sizeof(c->error), "%s", reason); c->state = CONTROL_REAUTH; }
static bool parse_node(const Json *j, int node, bool self) {
    uint8_t key[32], address[4]; char id[64], value[128];
    const char *expected_id = self ? c->config.node_id : c->config.peer_id;
    if (node < 0 || !json_field(j, node, "StableID", id, sizeof(id)) || !id[0] || (expected_id[0] && strcmp(id, expected_id)) ||
        !key_field(j, node, "Key", "nodekey:", key)) return false;
    int list = json_get(j, node, "Addresses");
    bool found = false;
    for (int t = json_first(j, list); t >= 0 && t < json_next(j, list); t = json_next(j, t)) {
        if (json_text(j, t, value, sizeof(value)) && ip4(address, value)) { found = true; break; }
    }
    if (!found) return false;
    uint64_t expires = expiry(j, node, "KeyExpiry");
    if (expires == UINT64_MAX) return false;
    if (json_is(j, json_get(j, node, "Expired"), "true") || (expires && expires <= tn_unix_ms())) {
        reauth(self ? "Playdate node key expired; enroll again" : "Selected peer key expired; renew the peer"); return false;
    }
    if (self) {
        uint8_t expected[32];
        const uint8_t *private = (!c->current_only && nonzero(c->config.staged_node_private)) ? c->config.staged_node_private : c->config.tailnet.node_private;
        public_key(expected, private);
        if (!crypto_equal(key, expected, 32)) return false;
        if (!key_field(j, node, "DiscoKey", "discokey:", expected) || !crypto_equal(expected, c->config.tailnet.disco_public, 32)) return false;
        if (!json_is(j, json_get(j, node, "MachineAuthorized"), "true")) { reauth("Playdate device approval required"); return false; }
        snprintf(c->result.node_id, sizeof(c->result.node_id), "%s", id);
        memcpy(c->result.tailnet.node_private, private, 32); memcpy(c->result.tailnet.node_public, key, 32); memcpy(c->result.tailnet.node_ip, address, 4);
        c->result.node_expiry_ms = expires;
        if (!c->current_only) memset(c->result.staged_node_private, 0, 32);
    } else {
        snprintf(c->result.peer_id, sizeof(c->result.peer_id), "%s", id);
        if (!key_field(j, node, "DiscoKey", "discokey:", c->result.tailnet.peer_disco) || !nonzero(c->result.tailnet.peer_disco)) return false;
        memcpy(c->result.tailnet.peer_public, key, 32); memcpy(c->result.tailnet.peer_ip, address, 4); c->result.peer_expiry_ms = expires;
        c->result.derp_region = (unsigned)json_int(j, json_get(j, node, "HomeDERP"));
        if (!c->result.derp_region) {
            unsigned region = 0; int used = 0;
            if (!json_field(j, node, "DERP", value, sizeof(value)) || sscanf(value, "127.3.3.40:%u%n", &region, &used) != 1 || value[used] || !region || region > 65535) return false;
            c->result.derp_region = region;
        }
    }
    c->result.tailnet.derp_region = (uint16_t)c->result.derp_region;
    return true;
}
static bool peer_matches(const Json *j, int node) {
    char value[256];
    if (c->config.peer_id[0]) return json_field(j, node, "StableID", value, sizeof(value)) && !strcmp(value, c->config.peer_id);
    if (!peer_target[0]) return false;
    uint8_t address[4], target[4];
    if (ip4(target, peer_target)) {
        int addresses = json_get(j, node, "Addresses");
        for (int t = json_first(j, addresses); t >= 0 && t < json_next(j, addresses); t = json_next(j, t))
            if (json_text(j, t, value, sizeof(value)) && ip4(address, value) && !memcmp(address, target, 4)) return true;
        return false;
    }
    if (!json_field(j, node, "Name", value, sizeof(value))) return false;
    size_t n = strlen(value);
    if (n && value[n - 1] == '.') value[--n] = 0;
    for (size_t i = 0; i < n; i++) value[i] = (char)tolower((unsigned char)value[i]);
    if (!strchr(peer_target, '.')) { char *dot = strchr(value, '.'); if (dot) *dot = 0; }
    return !strcmp(value, peer_target);
}
static bool parse_map(const Json *j) {
    if (!parse_node(j, json_get(j, 0, "Node"), true)) return false;
    int peers = json_get(j, 0, "Peers"); bool found = false;
    for (int node = json_first(j, peers); node >= 0 && node < json_next(j, peers); node = json_next(j, node)) {
        if (!peer_matches(j, node)) continue;
        if (found || !parse_node(j, node, false)) return false;
        found = true;
    }
    if (!found) { control_fail("Hub absent from tailnet map. Check its address and access."); return false; }
    char region_key[16]; snprintf(region_key, sizeof(region_key), "%u", c->result.derp_region);
    int map = json_get(j, 0, "DERPMap"), regions = json_get(j, map, "Regions"), region = json_get(j, regions, region_key), nodes = json_get(j, region, "Nodes");
    char host[256], cert[256]; bool relay_found = false;
    for (int node = json_first(j, nodes); node >= 0 && node < json_next(j, nodes); node = json_next(j, node)) {
        if (json_is(j, json_get(j, node, "STUNOnly"), "true") || json_is(j, json_get(j, node, "InsecureForTests"), "true") ||
            !json_field(j, node, "HostName", host, sizeof(host)) || !host[0]) continue;
        if (json_field(j, node, "CertName", cert, sizeof(cert)) && cert[0]) continue;
        for (const char *p = host; *p; p++) if (!(isalnum((unsigned char)*p) || *p == '.' || *p == '-')) return false;
        int port = json_int(j, json_get(j, node, "DERPPort")); if (!port) port = 443;
        if (port < 1 || port > 65535) return false;
        if (strcmp(c->result.tailnet.derp_host, host) || c->result.tailnet.derp_port != port) memset(c->result.tailnet.derp_public, 0, 32);
        snprintf(c->result.tailnet.derp_host, sizeof(c->result.tailnet.derp_host), "%s", host); c->result.tailnet.derp_port = port; relay_found = true; break;
    }
    return relay_found;
}
static bool finish_response(void) {
    if (!c->end_headers || c->status != 200) {
        if (c->status == 401 || c->status == 403) reauth("Control authorization required; enroll again");
        else control_fail("Control HTTP response rejected");
        return false;
    }
    size_t offset = 0;
    if (c->state == CONTROL_MAPPING) {
        if (c->body_length < 4) { control_fail("Incomplete map frame"); return false; }
        uint8_t *b = c->body;
        size_t n = (size_t)b[0] | (size_t)b[1] << 8 | (size_t)b[2] << 16 | (size_t)b[3] << 24;
        if (n != c->body_length - 4 || !n) { control_fail("Invalid map frame length"); return false; }
        offset = 4;
    }
    c->body[c->body_length] = 0;
    Json j;
    if (!json_parse(&j, (char *)c->body + offset, c->body_length - offset)) { control_fail("Invalid control JSON"); return false; }
#ifdef CONTROL_DIAGNOSTICS
    extern void control_diagnostic_map(const char *, size_t);
    if (c->state == CONTROL_MAPPING) control_diagnostic_map((char *)c->body + offset, c->body_length - offset);
#endif
    bool ok;
    if (c->state == CONTROL_REGISTERING) {
        char error[160];
        if (json_field(&j, 0, "Error", error, sizeof(error)) && error[0]) { reauth("Control rejected enrollment; enroll again"); ok = false; }
        else if (json_field(&j, 0, "AuthURL", c->auth_url, sizeof(c->auth_url)) && c->auth_url[0]) { reauth("Playdate authorization required; enroll again"); ok = false; }
        else if (json_is(&j, json_get(&j, 0, "NodeKeyExpired"), "true")) { reauth("Playdate key expired; enroll again"); ok = false; }
        else if (!json_is(&j, json_get(&j, 0, "MachineAuthorized"), "true")) { reauth("Playdate device approval required"); ok = false; }
        else { c->authorized = true; json_free(&j); return request(false); }
    } else {
        ok = parse_map(&j);
        if (ok && c->result.derp_region != c->config.derp_region) {
            if (c->relay_reconciled) { control_fail("Peer relay changed during refresh; reconnect"); ok = false; }
            else {
                c->relay_reconciled = true; c->config.derp_region = c->result.derp_region;
                json_free(&j); return request(false);
            }
        } else if (ok) c->state = CONTROL_READY;
        else if (c->state != CONTROL_REAUTH && c->state != CONTROL_FAILED) control_fail("Incomplete authenticated network map");
    }
    json_free(&j); return ok;
}
static bool decode_headers(void) {
    size_t offset = 0; int flags = 0;
    while (!(flags & NGHTTP2_HD_INFLATE_FINAL)) {
        nghttp2_nv field;
        ssize_t used = nghttp2_hd_inflate_hd2(c->inflater, &field, &flags, c->headers + offset, c->headers_length - offset, 1);
        if (used < 0 || (!used && !(flags & (NGHTTP2_HD_INFLATE_EMIT | NGHTTP2_HD_INFLATE_FINAL)))) { control_fail("Invalid HTTP/2 headers"); return false; }
        offset += (size_t)used;
        if ((flags & NGHTTP2_HD_INFLATE_EMIT) && field.namelen == 7 && !memcmp(field.name, ":status", 7)) {
            if (field.valuelen != 3 || !isdigit(field.value[0]) || !isdigit(field.value[1]) || !isdigit(field.value[2])) { control_fail("Invalid HTTP/2 response status"); return false; }
            c->status = (field.value[0] - '0') * 100 + (field.value[1] - '0') * 10 + field.value[2] - '0';
        }
    }
    nghttp2_hd_inflate_end_headers(c->inflater); c->headers_length = 0; c->header_open = false; c->end_headers = true;
    if (c->status == 401 || c->status == 403) { reauth("Control authorization required; enroll again"); return true; }
    return !c->headers_end_stream || finish_response();
}
static bool handle_frame(const uint8_t *f, size_t length) {
    unsigned type = f[3], flags = f[4], stream = be32(f + 5) & 0x7fffffff; const uint8_t *p = f + 9; size_t n = length - 9;
    if (c->header_open && (type != 9 || stream != c->stream)) { control_fail("Interrupted HTTP/2 header block"); return false; }
    if (type == 4) {
        if (stream || ((flags & 1) ? n != 0 : n % 6 != 0)) { control_fail("Invalid HTTP/2 settings"); return false; }
        return (flags & 1) || frame(4, 1, 0, NULL, 0);
    }
    if (type == 6) {
        if (stream || n != 8) { control_fail("Invalid control ping"); return false; }
        return (flags & 1) || frame(6, 1, 0, p, n);
    }
    if (type == 7 || (type == 3 && stream == c->stream)) { control_fail("Control stream closed; reconnect"); return false; }
    if (type == 5) { control_fail("Unexpected HTTP/2 push"); return false; }
    if (type != 0 && type != 1 && type != 9) return true;
    if (stream != c->stream || !stream) { control_fail("Unexpected HTTP/2 response stream"); return false; }
    size_t start = 0, end = n;
    if ((type == 0 || type == 1) && (flags & 8)) {
        if (!n || p[0] >= n) { control_fail("Invalid HTTP/2 padding"); return false; }
        start = 1; end -= p[0];
    }
    if (type == 1 && (flags & 32)) start += 5;
    if (start > end) { control_fail("Invalid HTTP/2 priority"); return false; }
    if (type == 1 || type == 9) {
        if (type == 1) { c->headers_end_stream = (flags & 1) != 0; c->header_open = true; }
        if (end - start > HEADER_CAP - c->headers_length) { control_fail("Control headers exceed bound"); return false; }
        memcpy(c->headers + c->headers_length, p + start, end - start); c->headers_length += end - start;
        return !(flags & 4) || decode_headers();
    }
    if (!c->end_headers || c->status != 200) { control_fail("Unexpected control response data"); return false; }
    if (end - start > CONTROL_MAX_MAP - c->body_length) { control_fail("Control map exceeds device bound"); return false; }
    memcpy(c->body + c->body_length, p + start, end - start); c->body_length += end - start;
    if (n) {
        uint8_t window[4]; put32(window, (uint32_t)n);
        if (!frame(8, 0, 0, window, 4) || !frame(8, 0, stream, window, 4)) return false;
    }
    return !(flags & 1) || finish_response();
}
static bool frames(void) {
    while (c->frames_length >= 9) {
        if (!c->early_done) {
            if (!memcmp(c->frames, "\xff\xff\xffTS", 5)) {
                size_t early = be32(c->frames + 5);
                if (early > 4096) { control_fail("EarlyNoise exceeds bound"); return false; }
                if (c->frames_length < early + 9) return true;
                memmove(c->frames, c->frames + early + 9, c->frames_length - early - 9); c->frames_length -= early + 9;
            }
            c->early_done = true; continue;
        }
        size_t length = ((size_t)c->frames[0] << 16 | (size_t)c->frames[1] << 8 | c->frames[2]) + 9;
        if (length > 16384 + 9) { control_fail("HTTP/2 frame exceeds negotiated bound"); return false; }
        if (c->frames_length < length) return true;
        if (!handle_frame(c->frames, length)) return false;
        memmove(c->frames, c->frames + length, c->frames_length - length); c->frames_length -= length;
        if (c->state == CONTROL_READY || c->state == CONTROL_REAUTH) return true;
    }
    return true;
}
static bool handshake(void) {
    uint8_t key[32], empty[1];
    if (c->incoming_length < 51) return true;
    if (c->incoming[0] != 2 || be16(c->incoming + 1) != 48) { control_fail("Control Noise handshake rejected"); return false; }
    mix_hash(c->incoming + 3, 32);
    if (!mix_dh(c->ephemeral, c->incoming + 3, key) || !mix_dh(c->config.machine_private, c->incoming + 3, key)) return false;
    if (!wireguard_aead_decrypt(empty, c->incoming + 35, 16, c->h, 32, 0, key)) { control_fail("Control Noise authentication failed"); return false; }
    mix_hash(c->incoming + 35, 16); hkdf(NULL, 0, c->tx_key, c->rx_key);
    crypto_zero(key, 32); crypto_zero(c->ephemeral, 32); crypto_zero(c->ck, 32);
    memmove(c->incoming, c->incoming + 51, c->incoming_length - 51); c->incoming_length -= 51; c->noise_ready = true;
    if (nghttp2_hd_inflate_new(&c->inflater)) { control_fail("Cannot initialize HPACK"); return false; }
    /* Disable push and dynamic header indexing. SETTINGS table bound is also
     * enforced in the decoder; no peer can grow it beyond this limit. */
    nghttp2_hd_inflate_change_table_size(c->inflater, 0);
    static const uint8_t settings[] = {0, 1, 0, 0, 0, 0, 0, 2, 0, 0, 0, 0};
    if (!record((const uint8_t *)"PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n", 24) || !frame(4, 0, 0, settings, sizeof(settings))) return false;
    uint64_t now = tn_unix_ms();
    if (!c->current_only && !nonzero(c->config.staged_node_private) && c->config.node_expiry_ms && now && c->config.node_expiry_ms <= now + RENEW_WINDOW_MS) {
        new_key(c->config.staged_node_private);
        if (!c->stage || !c->stage(&c->config, c->stage_context)) { control_fail("Persist replacement identity before renewal"); return false; }
    }
    c->stream = UINT32_MAX; /* First request uses stream 1. */
    return request(true);
}

static bool equal_header(const char *p, const char *name, size_t n) {
    if (strlen(name) != n) return false;
    for (size_t i = 0; i < n; i++) if (tolower((unsigned char)p[i]) != tolower((unsigned char)name[i])) return false;
    return true;
}
static bool http_headers(void) {
    if (c->incoming_length >= sizeof(c->incoming)) return false;
    c->incoming[c->incoming_length] = 0;
    char *end = strstr((char *)c->incoming, "\r\n\r\n");
    if (!end) { if (c->incoming_length > HTTP_CAP) control_fail("Control HTTP headers exceed bound"); return c->state != CONTROL_FAILED; }
    size_t size = (size_t)(end - (char *)c->incoming) + 4;
    if (size > HTTP_CAP) { control_fail("Control HTTP headers exceed bound"); return false; }
    unsigned status = 0;
    if (sscanf((char *)c->incoming, "HTTP/1.%*u %u", &status) != 1) { control_fail("Invalid control HTTP status"); return false; }
    bool upgrade = false, length_known = false;
    char *line = strstr((char *)c->incoming, "\r\n") + 2;
    while (line < end) {
        char *next = strstr(line, "\r\n"), *colon = memchr(line, ':', (size_t)(next - line));
        if (!colon) { control_fail("Invalid control HTTP header"); return false; }
        char *value = colon + 1; while (value < next && (*value == ' ' || *value == '\t')) value++;
        if (equal_header(line, "Upgrade", (size_t)(colon - line))) upgrade = equal_header(value, "tailscale-control-protocol", (size_t)(next - value));
        if (equal_header(line, "Content-Length", (size_t)(colon - line))) {
            size_t n = 0;
            for (char *p = value; p < next; p++) {
                if (!isdigit((unsigned char)*p) || n > 4096) { control_fail("Invalid control key length"); return false; }
                n = n * 10 + (size_t)(*p - '0');
            }
            if (!n || n > 4096) { control_fail("Control key response exceeds bound"); return false; }
            c->content_length = n; length_known = true;
        }
        if (equal_header(line, "Transfer-Encoding", (size_t)(colon - line))) c->chunked = equal_header(value, "chunked", (size_t)(next - value));
        line = next + 2;
    }
    if (c->state == CONTROL_KEY_FETCH) {
        if (status != 200 || (!length_known && !c->chunked) || (length_known && c->chunked)) { control_fail("Control key HTTP response rejected"); return false; }
        c->key_http = true;
    } else {
        if (status != 101 || !upgrade) { control_fail("Control HTTP upgrade rejected"); return false; }
        c->upgraded = true; c->upgrade_header_length = size;
    }
    memmove(c->incoming, c->incoming + size, c->incoming_length - size); c->incoming_length -= size;
    return true;
}
static bool key_response(void) {
    if (!c->key_http) { if (!http_headers() || !c->key_http) return c->state != CONTROL_FAILED; }
    bool done = false;
    if (!c->chunked) {
        if (c->incoming_length < c->content_length) return true;
        memcpy(c->body, c->incoming, c->content_length); c->body_length = c->content_length; done = true;
    } else {
        while (c->incoming_length) {
            if (!c->chunk_remaining) {
                c->incoming[c->incoming_length] = 0;
                char *end = strstr((char *)c->incoming, "\r\n");
                if (!end) return true;
                size_t length = (size_t)(end - (char *)c->incoming); size_t n = 0;
                if (!length || length > 8) { control_fail("Invalid control key chunk"); return false; }
                for (size_t i = 0; i < length; i++) {
                    unsigned char ch = c->incoming[i];
                    if (!isxdigit(ch)) { control_fail("Invalid control key chunk"); return false; }
                    n = n * 16 + (isdigit(ch) ? ch - '0' : tolower(ch) - 'a' + 10);
                }
                memmove(c->incoming, c->incoming + length + 2, c->incoming_length - length - 2); c->incoming_length -= length + 2;
                if (!n) { done = true; break; }
                if (n > 4096 - c->body_length) { control_fail("Control key response exceeds bound"); return false; }
                c->chunk_remaining = n;
            }
            if (c->incoming_length < c->chunk_remaining + 2) return true;
            if (memcmp(c->incoming + c->chunk_remaining, "\r\n", 2)) { control_fail("Invalid key chunk terminator"); return false; }
            memcpy(c->body + c->body_length, c->incoming, c->chunk_remaining); c->body_length += c->chunk_remaining;
            size_t used = c->chunk_remaining + 2; memmove(c->incoming, c->incoming + used, c->incoming_length - used); c->incoming_length -= used; c->chunk_remaining = 0;
        }
    }
    if (!done) return true;
    Json j;
    if (!json_parse(&j, (char *)c->body, c->body_length)) { control_fail("Invalid control key response"); return false; }
    bool ok = key_field(&j, 0, "publicKey", "mkey:", c->result.control_public);
    json_free(&j);
    if (!ok) { control_fail("Control key response has no Noise key"); return false; }
    c->state = CONTROL_KEY_READY; return true;
}
static bool process(void) {
    if (c->state == CONTROL_KEY_FETCH) return key_response();
    if (!c->upgraded && (!http_headers() || !c->upgraded)) return c->state != CONTROL_FAILED;
    if (!c->noise_ready && (!handshake() || !c->noise_ready)) return c->state != CONTROL_FAILED;
    while (c->incoming_length >= 3) {
        size_t length = be16(c->incoming + 1);
        if (c->incoming[0] != 4 || length < 16 || length > 4093) {
            char diagnostic[192];
            snprintf(diagnostic, sizeof(diagnostic), "Control record rejected: type %u; size %u; buffered %u; state %u; records %llu; wire %llu; upgrade %u",
                (unsigned)c->incoming[0], (unsigned)length, (unsigned)c->incoming_length, (unsigned)c->state,
                (unsigned long long)c->rx_nonce, (unsigned long long)c->rx_wire_bytes, (unsigned)c->upgrade_header_length);
            tn_log(diagnostic);
            control_fail("Invalid control Noise record"); return false;
        }
        if (c->incoming_length < length + 3) return true;
        if (length - 16 > sizeof(c->frames) - c->frames_length || c->rx_nonce == UINT64_MAX) { control_fail("Control plaintext exceeds bound"); return false; }
        if (!wireguard_aead_decrypt(c->frames + c->frames_length, c->incoming + 3, length, NULL, 0, nonce_be(c->rx_nonce++), c->rx_key)) { control_fail("Control record authentication failed"); return false; }
        c->rx_wire_bytes += length + 3;
        c->frames_length += length - 16;
        memmove(c->incoming, c->incoming + length + 3, c->incoming_length - length - 3); c->incoming_length -= length + 3;
        if (!frames()) return false;
        if (c->state == CONTROL_READY || c->state == CONTROL_REAUTH) return true;
    }
    return true;
}
size_t control_input(const uint8_t *bytes, size_t length) {
    if (!c || c->state == CONTROL_STOPPED || c->state == CONTROL_FAILED || c->state == CONTROL_READY || c->state == CONTROL_KEY_READY || c->state == CONTROL_REAUTH) return 0;
    size_t consumed = 0;
    while (length && c->state != CONTROL_FAILED && c->state != CONTROL_REAUTH && c->state != CONTROL_READY && c->state != CONTROL_KEY_READY) {
        size_t n = sizeof(c->incoming) - c->incoming_length - 1;
        if (n > length) n = length;
        if (!n) { control_fail("Control input exceeds bound"); break; }
        memcpy(c->incoming + c->incoming_length, bytes, n); c->incoming_length += n;
        consumed += n; bytes += n; length -= n;
        if (!process()) break;
    }
    return consumed;
}
void control_tick(void) {
    if (c && (c->state == CONTROL_KEY_FETCH || c->state == CONTROL_CONNECTING || c->state == CONTROL_REGISTERING || c->state == CONTROL_MAPPING) && (uint32_t)(tn_now_ms() - c->started) > 45000) control_fail("Control refresh timed out; reconnect");
}

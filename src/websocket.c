#include "websocket.h"
#include "tailnet.h"
#include "tailnet_device.h"
#include "random.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static uint32_t rotate(uint32_t n, unsigned bits) { return (n << bits) | (n >> (32 - bits)); }

// SHA-1 is required by RFC 6455's upgrade handshake.
static void sha1(const uint8_t *data, size_t length, uint8_t out[20]) {
    uint32_t h[5] = { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0 };
    size_t total = ((length + 9 + 63) / 64) * 64;
    for (size_t block = 0; block < total; block += 64) {
        uint32_t w[80] = {0};
        for (unsigned i = 0; i < 64; i++) {
            size_t offset = block + i;
            uint8_t b = offset < length ? data[offset] : offset == length ? 0x80 : 0;
            if (offset >= total - 8) b = (uint8_t)(((uint64_t)length * 8) >> ((total - 1 - offset) * 8));
            w[i / 4] |= (uint32_t)b << (24 - (i % 4) * 8);
        }
        for (unsigned i = 16; i < 80; i++) w[i] = rotate(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (unsigned i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20) { f = (b & c) | (~b & d); k = 0x5a827999; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ed9eba1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8f1bbcdc; }
            else { f = b ^ c ^ d; k = 0xca62c1d6; }
            uint32_t next = rotate(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rotate(b, 30); b = a; a = next;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }
    for (unsigned i = 0; i < 20; i++) out[i] = (uint8_t)(h[i / 4] >> (24 - (i % 4) * 8));
}

bool ws_initialize(const char *seed) { return random_initialize(seed); }
void ws_random(uint8_t *out, size_t length) { random_fill(out, length); }

size_t base64_encode(const uint8_t *bytes, size_t length, char *out) {
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t n = 0;
    for (size_t i = 0; i < length; i += 3) {
        uint32_t v = (uint32_t)bytes[i] << 16;
        if (i + 1 < length) v |= (uint32_t)bytes[i + 1] << 8;
        if (i + 2 < length) v |= bytes[i + 2];
        out[n++] = alphabet[v >> 18];
        out[n++] = alphabet[(v >> 12) & 63];
        out[n++] = i + 1 < length ? alphabet[(v >> 6) & 63] : '=';
        out[n++] = i + 2 < length ? alphabet[v & 63] : '=';
    }
    out[n] = 0;
    return n;
}

static void failed(WebSocket *ws, const char *message) {
    snprintf(ws->error, sizeof(ws->error), "%s", message);
    ws->state = WS_FAILED;
    app_log("TCP %s %p failed: %s", ws->host, ws->connection, ws->error);
}

void ws_close(WebSocket *ws) {
    if (ws->tunneled) tn_close();
    else net_close(ws->connection);
    ws->connection = NULL;
    free(ws->message);
    ws->message = NULL;
    ws->message_length = ws->message_capacity = 0;
    ws->state = WS_CLOSED;
}

static void begin_upgrade(WebSocket *ws) {
    uint8_t nonce[16], digest[20];
    char key[25], challenge[80];
    ws_random(nonce, sizeof(nonce));
    base64_encode(nonce, sizeof(nonce), key);
    snprintf(challenge, sizeof(challenge), "%s258EAFA5-E914-47DA-95CA-C5AB0DC85B11", key);
    sha1((uint8_t *)challenge, strlen(challenge), digest);
    base64_encode(digest, sizeof(digest), ws->accept);
    int n = snprintf((char *)ws->send, sizeof(ws->send),
        "GET %s HTTP/1.1\r\nHost: %s:%d\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\nAuthorization: Bearer %s\r\n\r\n",
        ws->path, ws->host, ws->port, key, ws->authorization);
    if (n < 0 || (size_t)n >= sizeof(ws->send)) { failed(ws, "Connection headers are too long"); return; }
    ws->send_length = n;
    ws->send_offset = 0;
    ws->state = WS_UPGRADING;
}

void ws_connect(WebSocket *ws, const char *host, int port, bool tls, const char *path,
    const char *authorization, size_t limit, WSMessage callback) {
    ws_close(ws);
    net_tick();
    memset(ws, 0, sizeof(*ws));
    if (strlen(host) >= sizeof(ws->host) || strlen(path) >= sizeof(ws->path) ||
        strlen(authorization) >= sizeof(ws->authorization)) { failed(ws, "Connection configuration is too long"); return; }
    snprintf(ws->host, sizeof(ws->host), "%s", host);
    snprintf(ws->path, sizeof(ws->path), "%s", path);
    snprintf(ws->authorization, sizeof(ws->authorization), "%s", authorization);
    ws->port = port; ws->tls = tls; ws->message_limit = limit;
    ws->on_message = callback; ws->header_needed = 2;
    ws->started = ws->last_received = ws->last_ping = app_time();
    ws->state = WS_CONNECTING;
    ws->tunneled = config.use_tailnet && !strcmp(host, config.hub_host) && port == config.hub_port;
    if (ws->tunneled) {
        if (tls) { failed(ws, "Use the encrypted tailnet transport with an HTTP Hub URL"); return; }
        tailnet_device_start();
    } else {
        ws->connection = net_open(host, port, tls);
        if (!ws->connection) failed(ws, "Network connection is not available");
    }
}

static bool queue_frame(WebSocket *ws, unsigned opcode, const uint8_t *data, size_t length) {
    if (ws->state != WS_OPEN || ws->send_length || length + 14 > sizeof(ws->send)) return false;
    size_t n = 0;
    ws->send[n++] = 0x80 | opcode;
    if (length < 126) ws->send[n++] = 0x80 | length;
    else { ws->send[n++] = 0x80 | 126; ws->send[n++] = length >> 8; ws->send[n++] = length; }
    uint8_t mask[4]; ws_random(mask, sizeof(mask));
    memcpy(ws->send + n, mask, 4); n += 4;
    for (size_t i = 0; i < length; i++) ws->send[n++] = data[i] ^ mask[i % 4];
    ws->send_length = n; ws->send_offset = 0;
    return true;
}

size_t ws_pending(const WebSocket *ws) {
    return ws->tunneled ? tn_pending() : net_pending(ws->connection);
}
bool ws_writable(const WebSocket *ws) {
    return ws->state == WS_OPEN && !ws->send_length && !ws->pong_pending && !ws->peer_closed &&
        ws_pending(ws) < 8192;
}
bool ws_send(WebSocket *ws, const char *message) {
    return ws_writable(ws) && queue_frame(ws, 1, (const uint8_t *)message, strlen(message));
}

static bool header_value(const char *headers, const char *name, char *out, size_t capacity) {
    size_t key_length = strlen(name);
    for (const char *p = strstr(headers, "\r\n"); p && p[2]; p = strstr(p + 2, "\r\n")) {
        const char *start = p + 2;
        if (strncasecmp(start, name, key_length) || start[key_length] != ':') continue;
        start += key_length + 1;
        while (*start == ' ' || *start == '\t') start++;
        const char *end = strstr(start, "\r\n");
        if (!end || (size_t)(end - start) >= capacity) return false;
        while (end > start && (end[-1] == ' ' || end[-1] == '\t')) end--;
        memcpy(out, start, end - start); out[end - start] = 0;
        return true;
    }
    return false;
}

static void upgrade(WebSocket *ws) {
    char accept[64], value[128];
    int status = 0;
    sscanf(ws->upgrade, "HTTP/1.1 %d", &status);
    if (status != 101) {
        char reason[96]; snprintf(reason, sizeof(reason), "Server refused WebSocket (HTTP %d)", status);
        failed(ws, reason); return;
    }
    if (!header_value(ws->upgrade, "Sec-WebSocket-Accept", accept, sizeof(accept)) || strcmp(accept, ws->accept) ||
        !header_value(ws->upgrade, "Upgrade", value, sizeof(value)) || strcasecmp(value, "websocket")) {
        failed(ws, "Invalid WebSocket upgrade"); return;
    }
    if (header_value(ws->upgrade, "Sec-WebSocket-Extensions", value, sizeof(value))) {
        failed(ws, "Unexpected WebSocket extension"); return;
    }
    ws->state = WS_OPEN;
    // The request buffer contained the credential and no longer needs it.
    memset(ws->send, 0, ws->send_length);
    ws->send_length = ws->send_offset = 0;
}

static void finish_frame(WebSocket *ws) {
    if (ws->opcode == 8) ws->peer_closed = true;
    else if (ws->opcode == 9) {
        memcpy(ws->pong, ws->control, ws->payload_length);
        ws->pong_length = ws->payload_length;
        ws->pong_pending = true;
    } else if (ws->opcode < 8 && ws->final) {
        if (ws->message) ws->message[ws->message_length] = 0;
        ws->fragmented = false;
        if (ws->on_message) ws->on_message(ws, ws->message ? ws->message : "", ws->message_length);
        ws->message_length = 0;
    }
    ws->header_length = 0; ws->header_needed = 2;
    ws->payload_read = ws->payload_length = 0;
}

static bool reserve_message(WebSocket *ws, size_t additional) {
    if (additional > ws->message_limit - ws->message_length) return false;
    size_t need = ws->message_length + additional + 1;
    if (need <= ws->message_capacity) return true;
    size_t capacity = ws->message_capacity ? ws->message_capacity : 4096;
    while (capacity < need) capacity *= 2;
    if (capacity > ws->message_limit + 1) capacity = ws->message_limit + 1;
    char *message = realloc(ws->message, capacity);
    if (!message) return false;
    ws->message = message; ws->message_capacity = capacity;
    return true;
}

static void receive_byte(WebSocket *ws, uint8_t byte) {
    if (ws->state == WS_UPGRADING) {
        if (ws->upgrade_length + 1 >= sizeof(ws->upgrade)) { failed(ws, "Server headers exceed the buffer"); return; }
        ws->upgrade[ws->upgrade_length++] = byte; ws->upgrade[ws->upgrade_length] = 0;
        if (ws->upgrade_length >= 4 && !memcmp(ws->upgrade + ws->upgrade_length - 4, "\r\n\r\n", 4)) upgrade(ws);
        return;
    }
    if (ws->state != WS_OPEN) return;
    if (ws->header_length < ws->header_needed) {
        ws->header[ws->header_length++] = byte;
        if (ws->header_length == 2) {
            ws->final = ws->header[0] >> 7;
            ws->opcode = ws->header[0] & 15;
            uint8_t length = ws->header[1] & 127;
            if ((ws->header[0] & 0x70) || (ws->header[1] & 0x80) ||
                !(ws->opcode == 0 || ws->opcode == 1 || ws->opcode == 8 || ws->opcode == 9 || ws->opcode == 10) ||
                (ws->opcode >= 8 && (!ws->final || length > 125))) {
                failed(ws, "Invalid WebSocket frame"); return;
            }
            ws->header_needed = length == 126 ? 4 : length == 127 ? 10 : 2;
        }
        if (ws->header_length != ws->header_needed) return;
        ws->payload_length = ws->header[1] & 127;
        if (ws->header_needed > 2) {
            ws->payload_length = 0;
            for (size_t i = 2; i < ws->header_needed; i++) ws->payload_length = (ws->payload_length << 8) | ws->header[i];
        }
        ws->payload_read = 0;
        if (ws->opcode < 8) {
            if ((ws->opcode == 0 && !ws->fragmented) || (ws->opcode == 1 && ws->fragmented) ||
                ws->payload_length > ws->message_limit || !reserve_message(ws, (size_t)ws->payload_length)) {
                failed(ws, "WebSocket message exceeds the client limit"); return;
            }
            if (ws->opcode == 1) ws->fragmented = !ws->final;
        }
        if (!ws->payload_length) finish_frame(ws);
        return;
    }
    if (ws->opcode >= 8) ws->control[ws->payload_read] = byte;
    else ws->message[ws->message_length++] = byte;
    if (++ws->payload_read == ws->payload_length) finish_frame(ws);
}

static size_t available(const WebSocket *ws) {
    return ws->tunneled ? tn_available() : net_available(ws->connection);
}
void ws_update(WebSocket *ws) {
    if (ws->state == WS_CONNECTING && !ws->tunneled && net_waiting(ws->connection))
        ws->started = app_time();
    net_tick();
    if (ws->state == WS_CLOSED || ws->state == WS_FAILED) return;
    if (!ws->tunneled && net_waiting(ws->connection)) return;
    if (ws->tunneled) {
        if (tn_state() == TN_FAILED) { failed(ws, tn_error()); return; }
        if (ws->state == WS_CONNECTING && tn_state() == TN_READY && tn_tcp_state() == TN_TCP_CLOSED)
            tn_open();
        if (ws->state == WS_CONNECTING && tn_tcp_state() == TN_TCP_OPEN) begin_upgrade(ws);
        if (tn_tcp_state() == TN_TCP_FAILED || tn_tcp_state() == TN_TCP_EOF) ws->peer_closed = true;
    } else {
        if (ws->state == WS_CONNECTING && net_state(ws->connection) == NS_OPEN) begin_upgrade(ws);
        if (net_state(ws->connection) == NS_CLOSED || net_state(ws->connection) == NS_FAILED)
            ws->peer_closed = true;
    }
    float now = app_time();
    if ((ws->state == WS_CONNECTING || ws->state == WS_UPGRADING) && now - ws->started > 40) {
        failed(ws, ws->tunneled ? tailnet_device_status() : "Connection timed out"); return;
    }
    if (ws->state == WS_OPEN && now - ws->last_received > 60) { failed(ws, "Connection stopped responding"); return; }
    if (ws->state == WS_OPEN && !ws->send_length) {
        if (ws->pong_pending && queue_frame(ws, 10, ws->pong, ws->pong_length)) ws->pong_pending = false;
        else if (now - ws->last_ping > 15 && queue_frame(ws, 9, NULL, 0)) ws->last_ping = now;
    }
    for (int budget = 0; budget < 8 && ws->send_length && !ws->peer_closed; budget++) {
        size_t length = ws->send_length - ws->send_offset;
        if (length > 2048) length = 2048;
        int n = ws->tunneled ? tn_write(ws->send + ws->send_offset, length) :
            net_write(ws->connection, ws->send + ws->send_offset, length);
        if (!n) break;
        if (n < 0 || (size_t)n > length) {
            failed(ws, "Network write failed"); return;
        }
        ws->send_offset += n;
        if (ws->send_offset == ws->send_length) ws->send_length = ws->send_offset = 0;
    }
    // All transports are polled serially on the game loop.
    static uint8_t bytes[2048];
    for (int budget = 0; budget < 32 && ws->state != WS_FAILED; budget++) {
        size_t count = available(ws);
        if (!count) break;
        if (count > sizeof(bytes)) count = sizeof(bytes);
        int n = ws->tunneled ? (int)tn_read(bytes, count) : net_read(ws->connection, bytes, count);
        if (n < 0) { failed(ws, "Network read failed"); return; }
        if (!n) break;
        ws->last_received = now;
        for (int i = 0; i < n && ws->state != WS_FAILED; i++) receive_byte(ws, bytes[i]);
    }
    if (ws->peer_closed && !available(ws)) failed(ws, ws->tunneled ? "Tailnet connection closed" : net_error(ws->connection));
}

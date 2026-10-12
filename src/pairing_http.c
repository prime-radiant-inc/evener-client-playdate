#include "pairing_http.h"
#include "app.h"
#include "json.h"
#include "net_socket.h"
#include "tailnet.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static NetSocket *socket;
static bool active, opening, allowed, asking, denied;
static uintptr_t permission_generation;
static float opened, retry;
static char request[1024], input[4096], body[3072], expected_code[PAIRING_COMPARISON_SIZE], status[160];
static size_t request_length, sent, received;
static void close_stream(void) {
    net_close(socket); socket = NULL;
    if (config.use_tailnet) tn_close();
    opening = false; sent = received = 0;
}
void pairing_http_stop(void) {
    close_stream(); active = allowed = asking = denied = false; permission_generation++;
    memset(request, 0, sizeof(request)); memset(input, 0, sizeof(input)); memset(body, 0, sizeof(body));
}
void pairing_http_start(const PairingInvitation *invitation, const char public_key[65], const char comparison[PAIRING_COMPARISON_SIZE]) {
    pairing_http_stop();
    char invitation_hex[49], payload[256];
    for (unsigned i = 0; i < 24; i++) snprintf(invitation_hex + i * 2, 3, "%02x", invitation->secret[i]);
    int n = snprintf(payload, sizeof(payload), "{\"invitation\":\"%s\",\"publicKey\":\"%s\",\"client\":\"Playdate\"}", invitation_hex, public_key);
    int total = snprintf(request, sizeof(request), "POST /api/device-pairing HTTP/1.1\r\nHost: %s:%u\r\nContent-Type: application/json\r\nContent-Length: %d\r\nConnection: close\r\n\r\n%s", invitation->destination.host, invitation->destination.port, n, payload);
    active = n > 0 && (size_t)n < sizeof(payload) && total > 0 && (size_t)total < sizeof(request);
    request_length = active ? (size_t)total : 0; retry = 0;
    snprintf(expected_code, sizeof(expected_code), "%s", comparison);
    snprintf(status, sizeof(status), "Connecting to the Hub for approval...");
}
static void permission(bool granted, void *context) {
    if ((uintptr_t)context != permission_generation) return;
    asking = false; allowed = granted; denied = !granted;
}
static void reconnect(const char *message) {
    close_stream(); retry = app_time() + 1.2f;
    snprintf(status, sizeof(status), "%s", message);
}
static int unhex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}
static enum PairingHTTPResult reply(uint8_t *envelope, size_t capacity, size_t *length) {
    Json json; char state[24], comparison[PAIRING_COMPARISON_SIZE];
    if (!json_parse(&json, body, strlen(body))) { reconnect("Invalid Hub reply. Reconnecting..."); return PAIRING_HTTP_WAIT; }
    bool parsed = json_field(&json, 0, "state", state, sizeof(state));
    enum PairingHTTPResult result = PAIRING_HTTP_WAIT;
    if (parsed && (!strcmp(state, "expired") || !strcmp(state, "rejected"))) {
        snprintf(status, sizeof(status), "%s", !strcmp(state, "expired") ? "Invitation expired. Create a new one in the browser." : "Another device was approved. Create a new invitation.");
        result = PAIRING_HTTP_ENDED;
    } else if (parsed && (!strcmp(state, "pending") || !strcmp(state, "approved"))) {
        if (!json_field(&json, 0, "comparison", comparison, sizeof(comparison)) || strcmp(comparison, expected_code)) {
            snprintf(status, sizeof(status), "The Hub code did not match. Start setup again."); result = PAIRING_HTTP_ENDED;
        } else if (!strcmp(state, "approved")) {
            // Reuse the response buffer after tokenization; JSON tokens still
            // refer to body, so this scratch cannot overwrite the parsed text.
            bool ok = json_field(&json, 0, "envelope", input, sizeof(input));
            size_t n = ok ? strlen(input) : 0;
            ok = ok && n && !(n & 1) && n / 2 <= capacity;
            for (size_t i = 0; ok && i < n / 2; i++) {
                int a = unhex(input[i * 2]), b = unhex(input[i * 2 + 1]);
                if (a < 0 || b < 0) ok = false; else envelope[i] = (uint8_t)(a * 16 + b);
            }
            if (ok) { *length = n / 2; result = PAIRING_HTTP_APPROVED; }
            else { snprintf(status, sizeof(status), "Invalid approval. Start setup again."); result = PAIRING_HTTP_ENDED; }
        } else snprintf(status, sizeof(status), "If all lines match, approve in the browser.");
    } else snprintf(status, sizeof(status), "Unexpected Hub reply. Reconnecting...");
    json_free(&json); close_stream(); retry = app_time() + 1.2f;
    if (result != PAIRING_HTTP_WAIT) active = false;
    return result;
}
// The device requests close-delimited HTTP; accept ordinary Content-Length and
// chunked bodies as well so a reverse proxy does not change the claim semantics.
static bool response_body(bool eof, int *status_code) {
    input[received] = 0;
    char *end = strstr(input, "\r\n\r\n");
    if (!end) return false;
    size_t offset = (size_t)(end + 4 - input), available = received - offset;
    if (sscanf(input, "HTTP/%*u.%*u %d", status_code) != 1) { *status_code = 400; return true; }
    bool chunked = false; long declared = -1;
    for (char *line = strstr(input, "\r\n") + 2; line < end;) {
        char *next = strstr(line, "\r\n"); if (!next) return false;
        if (!strncasecmp(line, "Content-Length:", 15)) declared = strtol(line + 15, NULL, 10);
        if (!strncasecmp(line, "Transfer-Encoding:", 18) && !strncasecmp(line + 18 + strspn(line + 18, " \t"), "chunked", 7)) chunked = true;
        line = next + 2;
    }
    if (chunked) {
        size_t at = offset, length = 0;
        while (at < received) {
            char *line_end = strstr(input + at, "\r\n"); if (!line_end) return false;
            char *number_end; unsigned long n = strtoul(input + at, &number_end, 16);
            if (number_end == input + at || (number_end != line_end && *number_end != ';') || n >= sizeof(body) - length) { *status_code = 400; return true; }
            at = (size_t)(line_end + 2 - input);
            if (!n) { body[length] = 0; return true; }
            if (n + 2 > received - at) return false;
            if (memcmp(input + at + n, "\r\n", 2)) { *status_code = 400; return true; }
            memcpy(body + length, input + at, n); length += n; at += n + 2;
        }
        return false;
    }
    if (declared >= (long)sizeof(body)) { *status_code = 400; return true; }
    if (declared >= 0) { if (available < (size_t)declared) return false; available = (size_t)declared; }
    else if (!eof) return false;
    if (available >= sizeof(body)) { *status_code = 400; return true; }
    memcpy(body, input + offset, available); body[available] = 0; return true;
}
enum PairingHTTPResult pairing_http_update(uint8_t *envelope, size_t capacity, size_t *length) {
    if (!active) return PAIRING_HTTP_ENDED;
    if (app_time() < retry) return PAIRING_HTTP_WAIT;
    if (!opening) {
        if (config.use_tailnet) {
            if (tn_state() != TN_READY || !tn_open()) return PAIRING_HTTP_WAIT;
        } else {
            if (denied) { snprintf(status, sizeof(status), "Allow Hub network access in Settings, then retry."); active = false; return PAIRING_HTTP_ENDED; }
            if (!allowed) {
                if (!asking) {
                    asking = true; permission_generation++;
                    enum accessReply access = pd->network->tcp->requestAccess(config.hub_host, config.hub_port, config.hub_tls,
                        "Pair with your Evener Hub.", permission, (void *)permission_generation);
                    if (access != kAccessAsk) permission(access == kAccessAllow, (void *)permission_generation);
                }
                if (!allowed) return PAIRING_HTTP_WAIT;
            }
            if (!net_idle()) return PAIRING_HTTP_WAIT;
            socket = net_open(config.hub_host, config.hub_port, config.hub_tls);
            if (!socket) { retry = app_time() + 1.2f; return PAIRING_HTTP_WAIT; }
        }
        opening = true; opened = app_time(); sent = received = 0;
    }
    bool connected = config.use_tailnet ? tn_tcp_state() == TN_TCP_OPEN : net_state(socket) == NS_OPEN;
    if (connected && sent < request_length) {
        int n = config.use_tailnet ? tn_write(request + sent, request_length - sent) : net_write(socket, request + sent, request_length - sent);
        if (n < 0) { reconnect("Connection interrupted. Retrying approval..."); return PAIRING_HTTP_WAIT; }
        sent += (size_t)n;
    }
    int n = config.use_tailnet ? (int)tn_read(input + received, sizeof(input) - received - 1) : net_read(socket, input + received, sizeof(input) - received - 1);
    if (n > 0) received += (size_t)n;
    bool eof = config.use_tailnet ? tn_tcp_state() == TN_TCP_EOF || tn_tcp_state() == TN_TCP_FAILED : net_state(socket) == NS_CLOSED || net_state(socket) == NS_FAILED;
    int code;
    if (response_body(eof, &code)) {
        if (code == 200) return reply(envelope, capacity, length);
        if (code >= 400 && code < 500) {
            close_stream(); active = false;
            snprintf(status, sizeof(status), "Hub refused this invitation. Create a new one."); return PAIRING_HTTP_ENDED;
        }
        reconnect("Hub unavailable. Retrying approval...");
    } else if (eof || n < 0 || received == sizeof(input) - 1 || app_time() - opened > 12) reconnect("Connection interrupted. Retrying approval...");
    return PAIRING_HTTP_WAIT;
}
const char *pairing_http_status(void) { return status; }

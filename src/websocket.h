#ifndef EVENER_WEBSOCKET_H
#define EVENER_WEBSOCKET_H
#include "app.h"
#include "net_socket.h"

enum WSState { WS_CLOSED, WS_CONNECTING, WS_UPGRADING, WS_OPEN, WS_FAILED };
typedef struct WebSocket WebSocket;
typedef void (*WSMessage)(WebSocket *, const char *, size_t);
struct WebSocket {
    NetSocket *connection;
    enum WSState state;
    char host[256], path[256], authorization[640], accept[32], error[192];
    int port;
    bool tls, peer_closed, fragmented, tunneled;
    float started, last_received, last_ping;
    uint8_t send[65536], header[14], control[125], pong[125];
    size_t pong_length;
    bool pong_pending;
    size_t send_length, send_offset, header_length, header_needed, control_length;
    uint64_t payload_length, payload_read;
    uint8_t opcode, final, control_opcode;
    char *message;
    size_t message_length, message_capacity, message_limit;
    char upgrade[4096];
    size_t upgrade_length;
    WSMessage on_message;
};
bool ws_initialize(const char *seed);
void ws_random(uint8_t *bytes, size_t length);
size_t base64_encode(const uint8_t *bytes, size_t length, char *out);
void ws_connect(WebSocket *ws, const char *host, int port, bool tls, const char *path,
    const char *authorization, size_t message_limit, WSMessage callback);
void ws_close(WebSocket *ws);
void ws_update(WebSocket *ws);
bool ws_send(WebSocket *ws, const char *message);
bool ws_writable(const WebSocket *ws);
size_t ws_pending(const WebSocket *ws);
#endif

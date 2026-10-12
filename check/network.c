/* Host TCP boundary for the real receiver/poller against an opt-in local Hub.
 * Only loopback is dialed; this does not exercise SDK TLS or native Tailscale. */
#include "platform.h"
#include "net_socket.h"
#include "tailnet.h"
#include "settings.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

struct NetSocket { int fd; enum NetState state; };
static NetSocket stream = {.fd = -1};
static struct sockaddr_in target;
static bool enabled, drop_reply = true;
unsigned network_opens;
void check_network_start(const char *endpoint) {
    unsigned port; char extra;
    CHECK(sscanf(endpoint, "http://127.0.0.1:%u%c", &port, &extra) == 1 && port > 0 && port <= 65535);
    target.sin_family = AF_INET; target.sin_port = htons((uint16_t)port);
    target.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
}
NetSocket *net_open(const char *host, int port, bool tls) {
    (void)host; (void)port; CHECK(!tls && stream.fd == -1);
    stream.fd = socket(AF_INET, SOCK_STREAM, 0); CHECK(stream.fd >= 0);
    CHECK(!connect(stream.fd, (struct sockaddr *)&target, sizeof(target)));
    CHECK(fcntl(stream.fd, F_SETFL, O_NONBLOCK) != -1);
    stream.state = NS_OPEN; network_opens++; return &stream;
}
void net_close(NetSocket *s) {
    if (!s) return;
    if (s->fd >= 0) CHECK(!close(s->fd));
    s->fd = -1; s->state = NS_CLOSED;
}
bool net_idle(void) { return stream.fd == -1; }
enum NetState net_state(const NetSocket *s) { return s ? s->state : NS_CLOSED; }
int net_read(NetSocket *s, void *bytes, size_t length) {
    if (!s || s->fd < 0) return 0;
    ssize_t n = recv(s->fd, bytes, length < 83 ? length : 83, 0);
    if (n > 0 && drop_reply) { drop_reply = false; net_close(s); return 0; }
    if (!n) s->state = NS_CLOSED;
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
    return (int)n;
}
int net_write(NetSocket *s, const void *bytes, size_t length) {
    CHECK(s && s->fd >= 0);
    ssize_t n = send(s->fd, bytes, length < 37 ? length : 37, 0);
    return n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : (int)n;
}
bool tailnet_device_initialize(void) { return settings_bind_peer("synthetic-approved-peer"); }
void tailnet_device_start(void) { enabled = true; }
void tailnet_device_stop(void) { enabled = false; net_close(&stream); }
void tailnet_device_update(void) {}
const char *tailnet_device_auth_url(void) { return ""; }
TailnetState tn_state(void) { return enabled ? TN_READY : TN_STOPPED; }
bool tn_open(void) { return net_open("", 0, false) != NULL; }
void tn_close(void) { net_close(&stream); }
int tn_write(const void *bytes, size_t length) { return net_write(&stream, bytes, length); }
size_t tn_read(void *bytes, size_t capacity) { int n = net_read(&stream, bytes, capacity); return n > 0 ? (size_t)n : 0; }
TailnetTCPState tn_tcp_state(void) { return stream.state == NS_OPEN ? TN_TCP_OPEN : TN_TCP_EOF; }

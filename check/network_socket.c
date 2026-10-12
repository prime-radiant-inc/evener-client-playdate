/* SDK boundary check for stalled writes and callback-owned connection lifetime. */
#include "platform.h"
#include "net_socket.h"
#include <string.h>

struct TCPConnection { size_t pending, incoming; bool receive_paused; unsigned open_work, send_work; TCPConnectionCallback *closed; TCPOpenCallback *opened; void *context; };
static TCPConnection connections[10];
static void (*fence_callback)(PDNetErr);
static unsigned created, closes, released, worker_sequence, fence_sequence;
static TCPConnection *create(const char *host, int port, bool tls) {
    (void)host; (void)port; (void)tls;
    CHECK(created < 10); return &connections[created++];
}
static PDNetErr open_connection(TCPConnection *connection, TCPOpenCallback callback, void *context) {
    connection->open_work = ++worker_sequence; connection->opened = callback; connection->context = context; return NET_OK;
}
static void complete_open(unsigned index) {
    TCPConnection *c = &connections[index]; c->open_work = 0;
    c->opened(c, NET_OK, c->context); net_tick();
}
static void enabled(bool value, void (*callback)(PDNetErr)) {
    CHECK(value && callback && !fence_callback); fence_callback = callback;
    fence_sequence = ++worker_sequence;
}
static void complete_fence(void) {
    CHECK(fence_callback);
    // Commands before this callback have completed, even if the SDK counter
    // stayed positive or an open command omitted its own callback.
    for (unsigned i = 0; i < created; i++) {
        if (connections[i].open_work < fence_sequence) connections[i].open_work = 0;
        if (connections[i].send_work < fence_sequence) connections[i].send_work = 0;
    }
    void (*callback)(PDNetErr) = fence_callback; fence_callback = NULL;
    callback(NET_OK); net_tick();
}
static PDNetErr close_connection(TCPConnection *connection) {
    // SDK close invalidates the handle immediately, before its worker command.
    CHECK(!connection->open_work && !connection->send_work);
    closes++; return NET_OK;
}
static void release(TCPConnection *connection) { CHECK(!connection->open_work && !connection->send_work); released++; }
static void set_closed(TCPConnection *connection, TCPConnectionCallback *callback) { connection->closed = callback; }
static void configure(TCPConnection *connection, int value) { (void)connection; (void)value; }
static PDNetErr no_error(TCPConnection *connection) { (void)connection; return NET_OK; }
static size_t pending(TCPConnection *connection) { return connection->pending; }
static size_t available(TCPConnection *connection) { return connection->incoming; }
static int read_bytes(TCPConnection *connection, void *bytes, size_t count) {
    if (!count) {
        if (connection->receive_paused) { connection->incoming = 4; connection->receive_paused = false; }
        return 0;
    }
    CHECK(count <= connection->incoming && count <= 4);
    memcpy(bytes, "next", count); connection->incoming -= count;
    return (int)count;
}
static int write_bytes(TCPConnection *connection, const void *bytes, size_t count) {
    (void)bytes; connection->send_work = ++worker_sequence; connection->pending += count; return (int)count;
}
static WifiStatus connected(void) { return kWifiConnected; }
static struct playdate_tcp tcp = {
    .newConnection = create, .open = open_connection, .close = close_connection, .release = release,
    .setReadTimeout = configure, .setReadBufferSize = configure, .setConnectTimeout = configure,
    .setConnectionClosedCallback = set_closed, .getError = no_error,
    .getBytesAvailable = available, .getSentBytesPending = pending, .write = write_bytes, .read = read_bytes,
};
static struct playdate_network network = {.tcp = &tcp, .getStatus = connected, .setEnabled = enabled};

int main(void) {
    pd->network = &network;
    NetSocket *first = net_open("fixture", 443, true);
    complete_open(0);
    CHECK(net_state(first) == NS_OPEN);
    connections[0].receive_paused = true;
    char incoming[4];
    CHECK(net_available(NULL) == 0);
    CHECK(net_read(first, incoming, sizeof(incoming)) == 0);
    CHECK(net_read(first, incoming, sizeof(incoming)) == sizeof(incoming));
    CHECK(!memcmp(incoming, "next", sizeof(incoming)));
    CHECK(net_write(first, "data", 4) == 4); net_tick();
    fixture_time = 19; net_tick(); CHECK(net_state(first) == NS_OPEN);
    connections[0].pending = 2; net_tick();
    fixture_time = 38; net_tick(); CHECK(net_state(first) == NS_OPEN);
    fixture_time = 39; net_tick(); CHECK(net_state(first) == NS_FAILED);
    CHECK(strstr(net_error(first), "no progress"));
    net_close(first); CHECK(closes == 0 && released == 0);
    CHECK(connections[0].closed);
    connections[0].closed(&connections[0], NET_OK); net_tick();
    CHECK(released == 0);
    complete_fence(); CHECK(closes == 1 && released == 0);
    CHECK(!net_idle());
    complete_fence(); CHECK(released == 1 && net_idle());
    NetSocket *second = net_open("fixture", 443, true);
    complete_open(1);
    CHECK(created == 2 && net_state(second) == NS_OPEN);

    CHECK(net_write(second, "data", 4) == 4); net_tick();
    net_close(second); CHECK(closes == 1);
    fixture_time = 59; net_tick(); CHECK(closes == 1 && released == 1);
    complete_fence(); CHECK(closes == 2 && released == 1);
    complete_fence(); CHECK(released == 2 && net_idle());

    NetSocket *third = net_open("fixture", 443, true);
    complete_open(2);
    CHECK(net_write(third, "data", 4) == 4); net_tick();
    fixture_time = 78; connections[2].pending = 0;
    CHECK(net_write(third, "data", 4) == 4);
    fixture_time = 80; net_tick(); CHECK(net_state(third) == NS_OPEN);
    connections[2].pending = 0; net_tick();
    fixture_time = 100; net_tick(); CHECK(net_state(third) == NS_OPEN);
    net_close(third); CHECK(closes == 2 && released == 2);
    complete_fence(); CHECK(closes == 3 && released == 2);
    connections[2].closed(&connections[2], NET_OK); net_tick(); CHECK(released == 2);
    complete_fence(); CHECK(released == 3 && net_idle());
    NetSocket *fourth = net_open("fixture", 443, true);
    complete_open(3);
    CHECK(net_write(fourth, "data", 4) == 4);
    connections[3].pending = 0; // SDK clears this at EOF while a send may still be queued.
    connections[3].closed(&connections[3], NET_OK); net_close(fourth);
    CHECK(released == 3 && !net_idle());
    complete_fence(); CHECK(closes == 4 && released == 3);
    complete_fence(); CHECK(released == 4 && net_idle());
    NetSocket *fifth = net_open("fixture", 443, true);
    CHECK(net_state(fifth) == NS_CONNECTING);
    fixture_time = 150; net_close(fifth); // Open accepted but never called back.
    CHECK(closes == 4 && released == 4);
    complete_fence(); CHECK(closes == 5 && released == 4);
    complete_fence(); CHECK(released == 5 && net_idle());
    NetSocket *sixth = net_open("fixture", 443, true);
    complete_open(5); CHECK(net_state(sixth) == NS_OPEN);
    net_close(sixth); CHECK(closes == 6 && released == 5);
    complete_fence(); CHECK(released == 6 && net_idle());
    NetSocket *seventh = net_open("fixture", 443, true); complete_open(6);
    NetSocket *eighth = net_open("fixture", 443, true); complete_open(7);
    CHECK(net_write(seventh, "data", 4) == 4); net_close(seventh);
    CHECK(net_write(eighth, "later", 5) == 5); net_close(eighth);
    complete_fence(); CHECK(closes == 7 && released == 6);
    CHECK(connections[7].send_work); // The first fence cannot cover a later write.
    complete_fence(); CHECK(closes == 8 && released == 7);
    complete_fence(); CHECK(released == 8 && net_idle());

    NetSocket *ninth = net_open("fixture", 443, true); complete_open(8);
    NetSocket *tenth = net_open("fixture", 443, true); complete_open(9);
    CHECK(net_write(ninth, "data", 4) == 4); net_close(ninth);
    net_close(tenth); CHECK(closes == 9 && released == 8);
    complete_fence(); CHECK(closes == 10 && released == 8);
    // Tenth closed after the first fence was queued and needs the next fence.
    complete_fence(); CHECK(released == 10 && net_idle());
    puts("PASS stalled writes, progress reset, callback loss, close ordering, remote close and shared fences");
    return 0;
}

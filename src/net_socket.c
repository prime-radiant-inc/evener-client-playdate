#include "app.h"
#include "net_socket.h"
#include <stdio.h>
#include <string.h>

struct NetSocket {
    TCPConnection *connection;
    enum NetState state;
    bool queued, tls, opening, closed, closing, retired, finished, work_finished;
    int port;
    unsigned generation, reads, read_bytes, written_bytes;
    unsigned close_attempts;
    unsigned work_command, close_command;
    size_t pending_bytes;
    PDNetErr close_result;
    float retired_at, retire_report_at, write_progress_at;
    char error[96], host[256];
};
static NetSocket sockets[12];
static unsigned next_generation, next_command, fence_command;
static bool fence_pending;

static bool before(unsigned command, unsigned fence) {
    return command && (int32_t)(fence - command) > 0;
}
static void worker_completed(PDNetErr error) {
    fence_pending = false;
    if (error != NET_OK) app_log("Network worker completed with Wi-Fi error %d", error);
    for (unsigned i = 0; i < sizeof(sockets) / sizeof(sockets[0]); i++) {
        NetSocket *s = &sockets[i];
        if (!s->connection) continue;
        if (before(s->work_command, fence_command)) {
            s->work_finished = true;
            if (s->retired && s->opening) {
                app_log("Network open finished without callback for %s #%u", s->host, s->generation);
                s->opening = false;
            }
        }
        if (before(s->close_command, fence_command)) {
            s->finished = true;
            app_log("Network local close completed for %s #%u", s->host, s->generation);
        }
    }
}
static void finish_work(void) {
    if (fence_pending) return;
    // SDK 3.1.2 queues this idempotent operation on the same worker as TCP
    // opens, writes and closes. Its callback establishes completion without
    // consuming another TCP slot. The SDK has one global callback slot.
    fence_pending = true;
    fence_command = ++next_command;
    pd->network->setEnabled(true, worker_completed);
}

static void failed(NetSocket *s, const char *operation, int error) {
    snprintf(s->error, sizeof(s->error), "%s (%d)", operation, error);
    s->state = NS_FAILED;
    app_log("Network %s #%u: %s; reads %u/%u; written %u", s->host, s->generation,
        s->error, s->reads, s->read_bytes, s->written_bytes);
}
static void opened(TCPConnection *connection, PDNetErr error, void *context) {
    NetSocket *s = context;
    if (s->connection != connection) return;
    s->opening = false;
    s->work_finished = true;
    if (error != NET_OK) {
        // Failed opens have no subsequent close callback in the SDK.
        s->closed = s->finished = true;
        failed(s, "Network connection failed", error);
    } else if (!s->closed) {
        s->state = NS_OPEN;
        app_log("Network %s #%u connected", s->host, s->generation);
    }
}
static void closed(TCPConnection *connection, PDNetErr error) {
    for (unsigned i = 0; i < sizeof(sockets) / sizeof(sockets[0]); i++) {
        NetSocket *s = &sockets[i];
        if (s->connection != connection) continue;
        // A notification racing local close cannot release queued send work.
        // Keep this callback installed: the SDK C trampoline requires one.
        if (s->closing) return;
        s->closed = true;
        if (error != NET_OK) failed(s, "Network connection closed", error);
        else s->state = NS_CLOSED;
        return;
    }
}
static void admit(NetSocket *s) {
    const struct playdate_tcp *tcp = pd->network->tcp;
    TCPConnection *connection = tcp->newConnection(s->host, s->port, s->tls);
    s->queued = false;
    if (!connection) {
        s->state = NS_FAILED;
        snprintf(s->error, sizeof(s->error), "Network connection is not available");
        app_log("Network %s #%u: %s", s->host, s->generation, s->error);
        return;
    }
    s->connection = connection; s->opening = true;
    app_log("Network opening %s:%d #%u; Wi-Fi status %d", s->host, s->port, s->generation,
        pd->network->getStatus());
    tcp->setReadTimeout(connection, 0);
    tcp->setReadBufferSize(connection, 65536);
    tcp->setConnectTimeout(connection, 20000);
    tcp->setConnectionClosedCallback(connection, closed);
    s->work_command = ++next_command;
    PDNetErr error = tcp->open(connection, opened, s);
    if (error != NET_OK) {
        s->opening = false; s->closed = s->finished = true;
        failed(s, "Could not start network connection", error);
    }
}

void net_tick(void) {
    for (unsigned i = 0; i < sizeof(sockets) / sizeof(sockets[0]); i++) {
        NetSocket *s = &sockets[i];
        PDNetErr send_error = s->connection && !s->opening && !s->closed ?
            pd->network->tcp->getError(s->connection) : NET_OK;
        if (send_error < 0 && s->state != NS_FAILED) failed(s, "Network send failed", send_error);
        if (s->connection && !s->opening && !s->closed && !s->closing) {
            size_t pending = net_pending(s);
            if (!pending || pending != s->pending_bytes) {
                s->pending_bytes = pending; s->write_progress_at = app_time();
            } else if (s->state != NS_FAILED && app_time() - s->write_progress_at >= 20) {
                failed(s, "Network write made no progress", NET_WRITE_TIMEOUT);
            }
        }
        if (!s->retired) continue;
        float now = app_time();
        if (s->connection && now >= s->retire_report_at && now - s->retired_at <= 10) {
            app_log("Network retiring %s #%u after %.1fs; opening %d; closing %d; closed %d; pending %u; available %u; SDK error %d; Wi-Fi status %d",
                s->host, s->generation, (double)(now - s->retired_at), s->opening, s->closing, s->closed,
                (unsigned)net_pending(s), (unsigned)net_available(s), pd->network->tcp->getError(s->connection),
                pd->network->getStatus());
            s->retire_report_at = now + 2;
        }
        if (!s->connection) { memset(s, 0, sizeof(*s)); continue; }
        if (s->finished) {
            // Failed open or the post-close worker callback confirms that no
            // queued command can still use this object. Remote EOF does not.
            pd->network->tcp->release(s->connection);
            memset(s, 0, sizeof(*s));
        } else if (!s->work_finished || s->opening) {
            // Close invalidates the SDK handle immediately. Its worker must
            // finish earlier sends/opens first, even when pending reports zero
            // after remote EOF or an open result callback was never delivered.
            finish_work();
        } else if (!s->closing) {
            s->closing = true;
            PDNetErr error = pd->network->tcp->close(s->connection);
            s->close_attempts++;
            if (s->close_attempts == 1 || error != s->close_result)
                app_log("Network close %s #%u returned %d; attempt %u; pending %u", s->host, s->generation,
                    error, s->close_attempts, (unsigned)net_pending(s));
            s->close_result = error;
            if (error == NET_OK) s->close_command = ++next_command;
            else {
                if (error == NET_BUSY || error == NET_WRITE_BUSY) s->closing = false;
                // Preserve callback ownership even when close reports an error.
            }
            if (s->close_command) finish_work();
        } else if (s->close_command) {
            finish_work();
        }
    }
    unsigned total = 0, tls = 0;
    NetSocket *next = NULL;
    for (unsigned i = 0; i < sizeof(sockets) / sizeof(sockets[0]); i++) {
        NetSocket *s = &sockets[i];
        if (s->connection) { total++; if (s->tls) tls++; }
        if (s->opening || (s->retired && s->connection)) return;
        if (s->queued && (!next || (int32_t)(s->generation - next->generation) < 0)) next = s;
    }
    // Retired handles finish closing before another handshake uses their slot.
    if (next && total < 4 && (!next->tls || tls < 2)) admit(next);
}
NetSocket *net_open(const char *host, int port, bool tls) {
    net_tick();
    NetSocket *s = NULL;
    for (unsigned i = 0; i < sizeof(sockets) / sizeof(sockets[0]); i++) {
        if (!sockets[i].connection && !sockets[i].queued && sockets[i].state == NS_CLOSED) { s = &sockets[i]; break; }
    }
    if (!s) return NULL;
    *s = (NetSocket){ .state = NS_CONNECTING, .queued = true, .tls = tls, .port = port };
    s->generation = ++next_generation;
    snprintf(s->host, sizeof(s->host), "%s", host);
    app_log("Network queued %s:%d #%u", host, port, s->generation);
    net_tick();
    return s;
}
void net_close(NetSocket *s) {
    if (s && !s->retired) {
        app_log("Network retiring %s #%u; state %d; reads %u/%u; written %u; pending %u; SDK error %d",
            s->host, s->generation, s->state, s->reads, s->read_bytes, s->written_bytes,
            (unsigned)net_pending(s), s->connection ? pd->network->tcp->getError(s->connection) : NET_OK);
        s->retired = true;
        s->retired_at = app_time(); s->retire_report_at = s->retired_at + 2;
    }
    net_tick();
}
bool net_waiting(const NetSocket *s) { return s && s->queued && !s->retired; }
bool net_recovery_needed(void) {
    for (unsigned i = 0; i < sizeof(sockets) / sizeof(sockets[0]); i++) {
        const NetSocket *s = &sockets[i];
        // Allow the SDK's 20-second connect deadline and normal worker cleanup.
        // A retained handle must not be closed or freed to force recovery.
        if (s->retired && s->connection && app_time() - s->retired_at >= 30) return true;
    }
    return false;
}
bool net_idle(void) {
    for (unsigned i = 0; i < sizeof(sockets) / sizeof(sockets[0]); i++)
        if (sockets[i].connection || sockets[i].queued || sockets[i].state != NS_CLOSED) return false;
    return true;
}
enum NetState net_state(const NetSocket *s) { return s ? s->state : NS_CLOSED; }
const char *net_error(const NetSocket *s) { return s && s->error[0] ? s->error : "Network connection closed"; }
size_t net_available(const NetSocket *s) {
    if (!s || !s->connection) return 0;
    size_t count = pd->network->tcp->getBytesAvailable(s->connection);
    if (!count) {
        // The SDK read path checks whether receive work needs resuming even
        // when its ring is empty. This poll does not wait for incoming data.
        char byte;
        pd->network->tcp->read(s->connection, &byte, 0);
    }
    return count;
}
size_t net_pending(const NetSocket *s) { return s && s->connection ? pd->network->tcp->getSentBytesPending(s->connection) : 0; }
int net_read(NetSocket *s, void *bytes, size_t capacity) {
    size_t count = net_available(s);
    if (!count) return 0;
    if (count > capacity) count = capacity;
    int n = pd->network->tcp->read(s->connection, bytes, count);
    if (n == NET_BUSY || n == NET_READ_BUSY || n == NET_READ_TIMEOUT) return 0;
    if (n < 0) failed(s, "Network read failed", n);
    if (n > 0) { s->reads++; s->read_bytes += (unsigned)n; }
    if (n > (int)count) { failed(s, "Network read exceeded requested length", n); return -1; }
    return n;
}
int net_write(NetSocket *s, const void *bytes, size_t length) {
    if (!s || s->state != NS_OPEN || s->retired) return -1;
    // Leave the SDK worker a chance to receive between sends. Completion
    // fences, rather than this counter, protect the handle during retirement.
    if (net_pending(s)) return 0;
    int n = pd->network->tcp->write(s->connection, bytes, length);
    if (n == NET_BUSY || n == NET_WRITE_BUSY) return 0;
    if (n < 0) failed(s, "Network write failed", n);
    if (n > 0) {
        s->written_bytes += (unsigned)n;
        s->work_command = ++next_command; s->work_finished = false;
        s->pending_bytes = net_pending(s); s->write_progress_at = app_time();
    }
    if (n > (int)length) { failed(s, "Network write exceeded requested length", n); return -1; }
    return n;
}

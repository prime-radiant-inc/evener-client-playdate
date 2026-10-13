#include "pd_api.h"
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define PORT 19184
#define TOTAL (1024u * 1024u)
#define RECEIPTS 10u
#define LOCKSTEP_BYTES 1200u

static PlaydateAPI *pd;
static const struct playdate_tcp *tcp;
static TCPConnection *connection, *sender;
static LCDFont *font;
static bool requested, allowed, started, connected, closed, done, passed;
static bool download_only, fence_writes, worker_pending, cleanup_queued, resume_reads, lockstep;
static bool split, sender_requested, sender_allowed, sender_started, sender_connected, sender_closed;
static bool restart_on_stall, block_writes, pressure_fence, restarting;
static size_t pressure_pending;
static float pressure_progress, pressure_fence_at;
static unsigned char pressure[4096];
static unsigned received, transmitted, writes;
static float start_time, last_read, next_report;
static unsigned char buffer[4096], receipt[117];
static char result[96], host[256] = "127.0.0.1", mode[16] = "duplex";

static float now(void) { return pd->system->getCurrentTimeMilliseconds() / 1000.0f; }

static TCPConnection *outbound(void) { return split ? sender : connection; }

static void report(const char *event) {
    pd->system->logToConsole("RAW %s t=%.3f rx=%u tx=%u writes=%u pending=%u available=%u error=%d closed=%d worker=%d read_idle=%.3f",
        event, (double)(now() - start_time), received, transmitted, writes,
        outbound() ? (unsigned)tcp->getSentBytesPending(outbound()) : 0,
        connection ? (unsigned)tcp->getBytesAvailable(connection) : 0,
        connection ? tcp->getError(connection) : 0, closed, worker_pending,
        (double)(now() - last_read));
}

static void closed_callback(TCPConnection *conn, PDNetErr error) {
    if (conn == sender) sender_closed = true;
    else closed = true;
    pd->system->logToConsole("RAW CLOSED sender=%d callback_error=%d", conn == sender, error);
}

static void open_callback(TCPConnection *conn, PDNetErr error, void *context) {
    (void)context;
    if (conn == sender) sender_connected = error == NET_OK;
    else { connected = error == NET_OK; last_read = now(); }
    pd->system->logToConsole("RAW OPEN sender=%d callback_error=%d", conn == sender, error);
}

static void close_after_worker(PDNetErr error) {
    pd->system->logToConsole("RAW FINAL_WORKER callback_error=%d", error);
    if (connection && !closed) {
        PDNetErr close_error = tcp->close(connection);
        pd->system->logToConsole("RAW CLOSE returned=%d", close_error);
    }
    if (sender && !sender_closed) {
        PDNetErr close_error = tcp->close(sender);
        pd->system->logToConsole("RAW CLOSE sender returned=%d", close_error);
    }
    /* Keep both connection objects alive until app exit so no pending SDK
     * task can observe a released object. */
}

static void finish(bool success, const char *reason) {
    if (done) return;
    passed = success;
    done = true;
    snprintf(result, sizeof(result), "%s", reason);
    report(success ? "PASS" : "FAIL");
    pd->system->logToConsole("RAW result=%s", result);
}

static void write_finished(PDNetErr error) {
    worker_pending = false;
    if (error != NET_OK) finish(false, "Worker callback error");
}

static void access_callback(bool granted, void *context) {
    if (context) sender_allowed = granted;
    else allowed = granted;
    pd->system->logToConsole("RAW ACCESS allowed=%d", granted);
    if (!granted) finish(false, "Access denied");
}

static void restart_download(const char *event) {
    static char arguments[288];
    report(event);
    snprintf(arguments, sizeof(arguments), "%s download", host);
    done = cleanup_queued = restarting = true;
    snprintf(result, sizeof(result), "Restarting into download control");
    pd->system->restartGame(arguments);
}

static void pump_pressure(float time) {
    size_t pending = tcp->getSentBytesPending(connection);
    if (!pending || pending != pressure_pending) pressure_progress = time;
    pressure_pending = pending;
    if (pending && time - pressure_progress >= 5 && !pressure_fence) {
        pressure_fence = worker_pending = true;
        pressure_fence_at = time;
        report("BLOCKED_WRITE_FENCE");
        pd->network->setEnabled(true, write_finished);
    }
    if (pressure_fence && time - pressure_fence_at >= 2) {
        restart_download("RESTART_BLOCKED_WRITE"); return;
    }
    if (!pending && !pressure_fence) {
        if (transmitted >= TOTAL) { finish(false, "Host did not block writes"); return; }
        int written = tcp->write(connection, pressure, sizeof(pressure));
        if (written != sizeof(pressure)) { finish(false, "Pressure write failed"); return; }
        transmitted += (unsigned)written;
        writes++;
    }
    if (time >= next_report) { report("PROGRESS"); next_report = time + 1; }
}

static void pump(void) {
    float time = now();
    if (time - start_time >= (lockstep ? 180 : 60)) { finish(false, "Experiment deadline"); return; }
    if (!connected) return;
    if (restart_on_stall && time - last_read >= 5) {
        restart_download("RESTART_AFTER_STALL"); return;
    }
    if (split && (sender_closed || tcp->getError(sender) != NET_OK)) {
        finish(false, "Sender connection failed"); return;
    }
    if (closed) { finish(false, "Closed before complete download"); return; }
    if (tcp->getError(connection) != NET_OK) { finish(false, "SDK error"); return; }
    if (block_writes) { pump_pressure(time); return; }
    if (worker_pending) return;
    for (unsigned budget = 0; budget < 65536;) {
        size_t available = tcp->getBytesAvailable(connection);
        if (!available) {
            if (resume_reads) tcp->read(connection, buffer, 0);
            break;
        }
        size_t count = available < sizeof(buffer) ? available : sizeof(buffer);
        int read = tcp->read(connection, buffer, count);
        if (read < 0) { finish(false, "SDK read failed"); return; }
        if (!read) break;
        for (int i = 0; i < read; i++) {
            if (buffer[i] != 0x3c) { finish(false, "Payload corruption"); return; }
        }
        received += (unsigned)read;
        budget += (unsigned)read;
        last_read = time;
        if (received > TOTAL) { finish(false, "Excess input"); return; }
    }
    unsigned expected_writes = download_only ? 1u : RECEIPTS;
    bool receipt_ready = !download_only || received == TOTAL;
    if (lockstep) {
        expected_writes = (TOTAL + LOCKSTEP_BYTES - 1) / LOCKSTEP_BYTES;
        unsigned receipt_after = (writes + 1) * LOCKSTEP_BYTES;
        if (receipt_after > TOTAL) receipt_after = TOTAL;
        receipt_ready = received >= receipt_after;
    }
    if (receipt_ready && writes < expected_writes
            && tcp->getSentBytesPending(outbound()) == 0) {
        int written = tcp->write(outbound(), receipt, sizeof(receipt));
        if (written != sizeof(receipt)) { finish(false, "SDK write did not accept receipt"); return; }
        transmitted += (unsigned)written;
        writes++;
        if (fence_writes) {
            worker_pending = true;
            pd->network->setEnabled(true, write_finished);
        }
    }
    if (received == TOTAL && writes == expected_writes && !worker_pending && tcp->getSentBytesPending(outbound()) == 0) {
        finish(true, "One MiB verified");
        return;
    }
    if (time >= next_report) { report("PROGRESS"); next_report = time + 1; }
}

static void line(const char *text, int y) {
    pd->graphics->drawText(text, strlen(text), kASCIIEncoding, 8, y);
}

static void open_connection(TCPConnection **target, int port) {
    *target = tcp->newConnection(host, port, false);
    if (!*target) { finish(false, "No SDK connection"); return; }
    tcp->setReadTimeout(*target, 0);
    tcp->setReadBufferSize(*target, 65536);
    tcp->setConnectTimeout(*target, 20000);
    tcp->setConnectionClosedCallback(*target, closed_callback);
    PDNetErr error = tcp->open(*target, open_callback, NULL);
    pd->system->logToConsole("RAW START port=%d download=%d fence=%d split=%d restart=%d open_return=%d",
        port, download_only, fence_writes, split, restart_on_stall, error);
    if (error != NET_OK) finish(false, "SDK open failed");
}

static int update(void *context) {
    (void)context;
    if (restarting) return 0;
    if (!requested && !done) {
        requested = true;
        enum accessReply reply = tcp->requestAccess(host, PORT, false,
            "Test synthetic TCP traffic with your local Mac.", access_callback, NULL);
        if (reply != kAccessAsk) access_callback(reply == kAccessAllow, NULL);
    }
    if (allowed && split && !sender_requested && !done) {
        sender_requested = true;
        enum accessReply reply = tcp->requestAccess(host, PORT + 1, false,
            "Send synthetic receipts over a separate TCP connection.", access_callback, &sender);
        if (reply != kAccessAsk) access_callback(reply == kAccessAllow, &sender);
    }
    if (sender_allowed && !sender_started && !done) {
        sender_started = true;
        start_time = last_read = next_report = now();
        open_connection(&sender, PORT + 1);
    }
    if (allowed && (!split || sender_connected) && !started && !done) {
        started = true;
        if (!split) start_time = last_read = next_report = now();
        open_connection(&connection, PORT);
    }
    if ((started || sender_started) && !done) pump();
    if (restarting) return 0;
    if (done && !cleanup_queued && !worker_pending) {
        cleanup_queued = true;
        pd->network->setEnabled(true, close_after_worker);
    }
    pd->graphics->clear(kColorWhite);
    if (font) pd->graphics->setFont(font);
    line("Raw SDK TCP probe", 8);
    const char *mode_label = "Duplex: 10 receipts";
    if (download_only) mode_label = "Download control";
    else if (fence_writes) mode_label = "Duplex with worker fence";
    else if (lockstep) mode_label = "One received block per receipt";
    else if (resume_reads) mode_label = "Duplex with receive resume";
    else if (split) mode_label = "Separate receive/send sockets";
    else if (restart_on_stall) mode_label = "Restart after receive stall";
    else if (block_writes) mode_label = "Restart after blocked write";
    line(mode_label, 36);
    char text[96];
    snprintf(text, sizeof(text), "Received %u / %u", received, TOTAL);
    line(text, 76);
    snprintf(text, sizeof(text), "Sent %u (%u receipts)", transmitted, writes);
    line(text, 106);
    const char *status_label = "Connecting";
    if (done) status_label = passed ? "PASS" : "FAIL";
    else if (connected) status_label = "Running";
    line(status_label, 146);
    line(result, 176);
    return 1;
}

#ifdef _WINDLL
__declspec(dllexport)
#endif
int eventHandler(PlaydateAPI *api, PDSystemEvent event, uint32_t arg) {
    (void)arg;
    if (restarting) return 0;
    if (event == kEventInit) {
        pd = api;
        tcp = pd->network->tcp;
        const char *path = NULL;
        const char *args = pd->system->getLaunchArgs(&path);
        if (args && *args) sscanf(args, "%255s %15s", host, mode);
        download_only = strcmp(mode, "download") == 0;
        fence_writes = strcmp(mode, "fence") == 0;
        resume_reads = strcmp(mode, "resume") == 0;
        lockstep = strcmp(mode, "lockstep") == 0;
        split = strcmp(mode, "split") == 0;
        restart_on_stall = strcmp(mode, "restart") == 0;
        block_writes = strcmp(mode, "blocked") == 0;
        memset(pressure, 0xa5, sizeof(pressure));
        memset(receipt, 0xa5, sizeof(receipt));
        const char *error = NULL;
        font = pd->graphics->loadFont("/System/Fonts/Roobert-10-Bold.pft", &error);
        pd->display->setRefreshRate(30);
        pd->system->setUpdateCallback(update, NULL);
    }
    return 0;
}

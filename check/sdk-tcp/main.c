#include "pd_api.h"
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define PORT 19184
#define TOTAL (1024u * 1024u)
#define RECEIPTS 10u

static PlaydateAPI *pd;
static const struct playdate_tcp *tcp;
static TCPConnection *connection;
static LCDFont *font;
static bool requested, allowed, started, connected, closed, done, passed;
static bool download_only, fence_writes, worker_pending, cleanup_queued;
static unsigned received, transmitted, writes;
static float start_time, last_read, next_report;
static unsigned char buffer[4096], receipt[117];
static char result[96], host[256] = "127.0.0.1", mode[16] = "duplex";

static float now(void) { return pd->system->getCurrentTimeMilliseconds() / 1000.0f; }

static void report(const char *event) {
    pd->system->logToConsole("RAW %s t=%.3f rx=%u tx=%u writes=%u pending=%u available=%u error=%d closed=%d worker=%d read_idle=%.3f",
        event, (double)(now() - start_time), received, transmitted, writes,
        connection ? (unsigned)tcp->getSentBytesPending(connection) : 0,
        connection ? (unsigned)tcp->getBytesAvailable(connection) : 0,
        connection ? tcp->getError(connection) : 0, closed, worker_pending,
        (double)(now() - last_read));
}

static void closed_callback(TCPConnection *conn, PDNetErr error) {
    (void)conn;
    closed = true;
    pd->system->logToConsole("RAW CLOSED callback_error=%d", error);
}

static void open_callback(TCPConnection *conn, PDNetErr error, void *context) {
    (void)conn; (void)context;
    connected = error == NET_OK;
    last_read = now();
    pd->system->logToConsole("RAW OPEN callback_error=%d", error);
}

static void close_after_worker(PDNetErr error) {
    pd->system->logToConsole("RAW FINAL_WORKER callback_error=%d", error);
    if (connection && !closed) {
        PDNetErr close_error = tcp->close(connection);
        pd->system->logToConsole("RAW CLOSE returned=%d", close_error);
    }
    /* Keep the connection alive until the app exits. No pending SDK task can
     * observe a released object during this single-connection experiment. */
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
    (void)context;
    allowed = granted;
    pd->system->logToConsole("RAW ACCESS allowed=%d", granted);
    if (!granted) finish(false, "Access denied");
}

static void pump(void) {
    float time = now();
    if (time - start_time >= 60) { finish(false, "Experiment deadline"); return; }
    if (!connected) return;
    if (closed) { finish(false, "Closed before complete download"); return; }
    if (tcp->getError(connection) != NET_OK) { finish(false, "SDK error"); return; }
    if (worker_pending) return;
    for (unsigned budget = 0; budget < 65536;) {
        size_t available = tcp->getBytesAvailable(connection);
        if (!available) break;
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
    if ((!download_only || received == TOTAL) && writes < (download_only ? 1u : RECEIPTS)
            && tcp->getSentBytesPending(connection) == 0) {
        int written = tcp->write(connection, receipt, sizeof(receipt));
        if (written != sizeof(receipt)) { finish(false, "SDK write did not accept receipt"); return; }
        transmitted += (unsigned)written;
        writes++;
        if (fence_writes) {
            worker_pending = true;
            pd->network->setEnabled(true, write_finished);
        }
    }
    if (received == TOTAL && writes && !worker_pending && tcp->getSentBytesPending(connection) == 0) {
        finish(true, "One MiB verified");
        return;
    }
    if (time >= next_report) { report("PROGRESS"); next_report = time + 1; }
}

static void line(const char *text, int y) {
    pd->graphics->drawText(text, strlen(text), kASCIIEncoding, 8, y);
}

static int update(void *context) {
    (void)context;
    if (!requested && !done) {
        requested = true;
        enum accessReply reply = tcp->requestAccess(host, PORT, false,
            "Test synthetic TCP traffic with your local Mac.", access_callback, NULL);
        if (reply != kAccessAsk) access_callback(reply == kAccessAllow, NULL);
    }
    if (allowed && !started && !done) {
        started = true;
        start_time = last_read = next_report = now();
        connection = tcp->newConnection(host, PORT, false);
        if (!connection) finish(false, "No SDK connection");
        else {
            tcp->setReadTimeout(connection, 0);
            tcp->setReadBufferSize(connection, 65536);
            tcp->setConnectTimeout(connection, 20000);
            tcp->setConnectionClosedCallback(connection, closed_callback);
            PDNetErr error = tcp->open(connection, open_callback, NULL);
            pd->system->logToConsole("RAW START download=%d fence=%d open_return=%d", download_only, fence_writes, error);
            if (error != NET_OK) finish(false, "SDK open failed");
        }
    }
    if (started && !done) pump();
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
    if (event == kEventInit) {
        pd = api;
        tcp = pd->network->tcp;
        const char *path = NULL;
        const char *args = pd->system->getLaunchArgs(&path);
        if (args && *args) sscanf(args, "%255s %15s", host, mode);
        download_only = strcmp(mode, "download") == 0;
        fence_writes = strcmp(mode, "fence") == 0;
        memset(receipt, 0xa5, sizeof(receipt));
        const char *error = NULL;
        font = pd->graphics->loadFont("/System/Fonts/Roobert-10-Bold.pft", &error);
        pd->display->setRefreshRate(30);
        pd->system->setUpdateCallback(update, NULL);
    }
    return 0;
}

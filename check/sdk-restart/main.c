#include "pd_api.h"
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

/* Independent reproduction of an open TCP connection across runtime restart.
 * There are no writes, TLS, dependencies, application data or credentials. */
static PlaydateAPI *pd;
static TCPConnection *connection;
static LCDFont *font;
static bool requested, allowed, started, connected, restarting, reported;
static float connected_at;
static char host[256] = "127.0.0.1", mode[16] = "callback";
static unsigned remaining;

static void closed(TCPConnection *conn, PDNetErr error) {
    (void)conn;
    (void)error;
}

static void opened(TCPConnection *conn, PDNetErr error, void *context) {
    (void)conn;
    (void)context;
    connected = error == NET_OK;
    connected_at = pd->system->getCurrentTimeMilliseconds() / 1000.0f;
    pd->system->logToConsole("LIFECYCLE OPEN error=%d remaining=%u", error, remaining);
}

static void access_result(bool granted, void *context) {
    (void)context;
    allowed = granted;
    pd->system->logToConsole("LIFECYCLE ACCESS granted=%d", granted);
}

static int update(void *context) {
    (void)context;
    if (restarting) return 0;
    if (!requested) {
        requested = true;
        enum accessReply reply = pd->network->tcp->requestAccess(host, 19186, false,
            "Test TCP callback lifetime with a local synthetic server.", access_result, NULL);
        if (reply != kAccessAsk) access_result(reply == kAccessAllow, NULL);
    }
    if (allowed && !started) {
        started = true;
        connection = pd->network->tcp->newConnection(host, 19186, false);
        if (connection) {
            if (!strcmp(mode, "callback"))
                pd->network->tcp->setConnectionClosedCallback(connection, closed);
            PDNetErr error = pd->network->tcp->open(connection, opened, NULL);
            pd->system->logToConsole("LIFECYCLE START mode=%s open_return=%d", mode, error);
        }
    }
    PDButtons pressed;
    pd->system->getButtonState(NULL, &pressed, NULL);
    float time = pd->system->getCurrentTimeMilliseconds() / 1000.0f;
    if (connected && !reported) {
        reported = true;
        pd->system->logToConsole("LIFECYCLE READY remaining=%u", remaining);
    }
    if (connected && ((pressed & kButtonA) || (remaining && time - connected_at >= 2))) {
        static char arguments[304];
        snprintf(arguments, sizeof(arguments), "%s %s %u", host, mode, remaining ? remaining - 1 : 0);
        restarting = true;
        pd->system->logToConsole("LIFECYCLE RESTART remaining=%u", remaining);
        pd->system->restartGame(arguments);
        return 0;
    }
    pd->graphics->clear(kColorWhite);
    if (font) pd->graphics->setFont(font);
    const char *label = connected ? "Open TCP: A restarts runtime" : "Waiting for local TCP server";
    pd->graphics->drawText(label, strlen(label), kASCIIEncoding, 8, 20);
    pd->graphics->drawText(mode, strlen(mode), kASCIIEncoding, 8, 60);
    return 1;
}

#ifdef _WINDLL
__declspec(dllexport)
#endif
int eventHandler(PlaydateAPI *api, PDSystemEvent event, uint32_t arg) {
    (void)arg;
    if (event == kEventInit) {
        pd = api;
        const char *path = NULL;
        const char *arguments = pd->system->getLaunchArgs(&path);
        if (arguments && *arguments) sscanf(arguments, "%255s %15s %u", host, mode, &remaining);
#if TARGET_PLAYDATE
        /* Omitting the callback is a Simulator comparison only. The physical
         * SDK C close trampoline has previously faulted without a callback. */
        snprintf(mode, sizeof(mode), "callback");
#endif
        const char *error = NULL;
        font = pd->graphics->loadFont("/System/Fonts/Roobert-10-Bold.pft", &error);
        pd->system->setUpdateCallback(update, NULL);
        pd->system->logToConsole("LIFECYCLE INIT mode=%s remaining=%u callback=%p", mode, remaining, (void *)closed);
    } else if (event == kEventTerminate) {
        pd->system->logToConsole("LIFECYCLE TERMINATE restarting=%d", restarting);
    }
    return 0;
}

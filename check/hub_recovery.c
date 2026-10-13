/* Real Hub cache/read recovery with a deterministic WebSocket boundary. */
#include "platform.h"
#include "hub.h"
#include "settings.h"
#include "websocket.h"
#include "json.h"
#include <dirent.h>
#include <string.h>
#include <unistd.h>

static WebSocket *transport;
static unsigned request_id;
static char method[64];
enum VoiceState voice_state;
const char *tailnet_device_status(void) { return ""; }
void ws_connect(WebSocket *ws, const char *host, int port, bool tls, const char *path,
        const char *authorization, size_t limit, WSMessage callback) {
    (void)host; (void)port; (void)tls; (void)path; (void)authorization; (void)limit;
    transport = ws; ws->state = WS_OPEN; ws->on_message = callback;
}
void ws_close(WebSocket *ws) { ws->state = WS_CLOSED; }
void ws_update(WebSocket *ws) { (void)ws; }
bool ws_writable(const WebSocket *ws) { return ws->state == WS_OPEN; }
void ws_random(uint8_t *bytes, size_t length) { memset(bytes, 0xa5, length); }
bool ws_send(WebSocket *ws, const char *message) {
    CHECK(ws == transport);
    Json j; CHECK(json_parse(&j, message, strlen(message)));
    request_id = json_int(&j, json_get(&j, 0, "id"));
    CHECK(json_field(&j, 0, "method", method, sizeof(method)));
    json_free(&j); return true;
}
static void reply(const char *result) {
    char bytes[16384];
    int n = snprintf(bytes, sizeof(bytes), "{\"id\":%u,\"result\":%s}", request_id, result);
    CHECK(n > 0 && n < sizeof(bytes));
    transport->on_message(transport, bytes, n);
}
static void page(const char *snapshot, int first, int count, bool older) {
    char result[12000];
    int n = snprintf(result, sizeof(result), "{\"snapshot\":{\"incarnation\":\"%s\"},\"%s\":\"older\",%s[{\"items\":[",
        snapshot, older ? "nextCursor" : "olderCursor", older ? "\"data\":" : "\"thread\":{\"turns\":");
    for (int i = 0; i < count; i++) {
        int used = snprintf(result + n, sizeof(result) - n, "%s{\"type\":\"userMessage\",\"id\":\"row%d\","
            "\"position\":{\"entry\":%d},\"text\":\"sentinel%d\"}", i ? "," : "", first + i, first + i, first + i);
        CHECK(used > 0 && used < sizeof(result) - n); n += used;
    }
    CHECK(snprintf(result + n, sizeof(result) - n, "]}]%s}", older ? "" : "}") < sizeof(result) - n);
    reply(result);
}
static void read_latest(const char *snapshot, int count) {
    hub_connect(); hub_update(); CHECK(!strcmp(method, "initialize"));
    reply("{\"protocolVersion\":\"evener-appwire-v7\"}");
    hub_update(); CHECK(!strcmp(method, "initialized"));
    hub_update(); CHECK(!strcmp(method, "thread/read"));
    page(snapshot, 1000, count, false); CHECK(hub_hydrated);
}
int main(int argc, char **argv) {
    bool ordinary = argc > 1 && !strcmp(argv[1], "ordinary-selection");
    bool unpinned = argc > 1 && !strcmp(argv[1], "unpinned-window");
    char directory[] = "/tmp/playdate-hub-recovery-XXXXXX"; CHECK(mkdtemp(directory)); CHECK(!chdir(directory));
    PairingSettings hub = destination("fixture"); hub.tailnet = false;
    bundle(&hub); CHECK(settings_initialize());
    NavigationRow row = {.ref = "fixture-session", .title = "fixture-title"};
    CHECK(hub_select(&row)); read_latest("snapshot-a", 1); CHECK(chat_count == 1);
    hub_older(); hub_update(); CHECK(!strcmp(method, "thread/turns/list"));
    reply("{\"snapshot\":{\"incarnation\":\"snapshot-a\"},\"data\":[{\"items\":[{\"type\":\"userMessage\","
        "\"id\":\"old\",\"position\":{\"entry\":1},\"text\":\"sentinel-old\"}]}]}");
    CHECK(chat_count == 2 && !strcmp(chat[0].key, "old"));
    char snapshot[128];
    fail_flush = true; CHECK(!hub_save_view(snapshot, sizeof(snapshot))); fail_flush = false;
    CHECK(hub_save_view(snapshot, sizeof(snapshot)));
    hub_close(); hub_reset_profile();
    CHECK(ordinary ? hub_select(&row) : hub_restore_view(&row, snapshot));
    CHECK(chat_count == 2 && !hub_hydrated);
    read_latest("snapshot-a", 1);
    CHECK(chat_count == 2 && !strcmp(chat[0].key, "old"));
    CHECK(hub_save_view(snapshot, sizeof(snapshot)));
    hub_close(); hub_reset_profile(); CHECK(hub_restore_view(&row, snapshot));
    read_latest("snapshot-b", 1); CHECK(chat_count == 1 && !strcmp(chat[0].key, "row1000"));
    read_latest("snapshot-full", 24);
    for (int first = 976; first >= 928; first -= 24) {
        hub_older(); hub_update(); CHECK(!strcmp(method, "thread/turns/list"));
        page("snapshot-full", first, 24, true);
    }
    CHECK(chat_count == CHAT_CAPACITY && !strcmp(chat[0].key, "row928"));
    CHECK(hub_save_view(snapshot, sizeof(snapshot)));
    hub_close(); hub_reset_profile(); CHECK(hub_restore_view(&row, snapshot));
    if (!unpinned) hub_set_reading_anchor("row928");
    read_latest("snapshot-full", 24);
    CHECK(chat_count == CHAT_CAPACITY && !strcmp(chat[0].key, "row928"));
    hub_close(); hub_reset_profile();
    DIR *dir = opendir("."); CHECK(dir); struct dirent *entry;
    while ((entry = readdir(dir))) if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, "..")) CHECK(!unlink(entry->d_name));
    closedir(dir); CHECK(!chdir("/")); CHECK(!rmdir(directory));
    puts("PASS saved older window, full-capacity anchor, incarnation change and failed checkpoint");
    return 0;
}

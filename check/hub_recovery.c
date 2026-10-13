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
static char method[64], mutation[65];
static bool includes_turns;
static unsigned submissions;
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
    int params = json_get(&j, 0, "params");
    if (!strcmp(method, "thread/read")) includes_turns = json_is(&j, json_get(&j, params, "includeTurns"), "true");
    if (!strcmp(method, "turn/start") || !strcmp(method, "turn/queue")) {
        submissions++;
        CHECK(json_field(&j, params, "clientMutationId", mutation, sizeof(mutation)));
    }
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
static void connect_hub(void) {
    hub_connect(); hub_update(); CHECK(!strcmp(method, "initialize"));
    reply("{\"protocolVersion\":\"evener-appwire-v7\"}");
    hub_update(); CHECK(!strcmp(method, "initialized"));
}
static void read_latest(const char *snapshot, int count) {
    connect_hub();
    hub_update(); CHECK(!strcmp(method, "thread/read"));
    page(snapshot, 1000, count, false); CHECK(hub_hydrated);
}
static void metadata(const char *accepted, bool authoritative) {
    char result[1024];
    int n = snprintf(result, sizeof(result), "{\"thread\":{\"status\":{\"type\":\"idle\"},"
        "\"evener\":{\"instanceId\":\"fixture-instance\",\"capabilities\":{\"send\":true},"
        "\"mutationStateAuthoritative\":%s,\"pendingMutations\":[{\"clientMutationId\":\"%s\"}]}}}",
        authoritative ? "true" : "false", accepted);
    CHECK(n > 0 && n < sizeof(result)); reply(result);
}
static void prepare_after_reconnect(void) {
    connect_hub();
    hub_update(); CHECK(!strcmp(method, "thread/read") && !includes_turns);
}
static void send_metadata(void) {
    int saved_count = chat_count;
    char anchor[192]; snprintf(anchor, sizeof(anchor), "%s", chat[0].key);
    CHECK(draft_set("sentinel-input", true));
    hub_send(); CHECK(hub_send_pending()); hub_update();
    CHECK(!strcmp(method, "thread/read") && !includes_turns);
    metadata("", true);
    CHECK(chat_count == saved_count && !strcmp(chat[0].key, anchor));
    CHECK(hub_has_older && hub_hydrated && hub_send_pending());
    hub_update(); CHECK(!strcmp(method, "turn/start") && submissions == 1);
    CHECK(draft.state == DRAFT_SENDING && draft_load(selected_ref));
    CHECK(draft.state == DRAFT_UNCERTAIN && !strcmp(draft.mutation, mutation));
    char sent[65]; snprintf(sent, sizeof(sent), "%s", mutation);
    hub_close(); CHECK(draft.state == DRAFT_UNCERTAIN);
    prepare_after_reconnect(); metadata("", true);
    CHECK(!hub_hydrated && chat_count == saved_count);
    hub_update(); CHECK(submissions == 2 && !strcmp(sent, mutation));
    hub_close(); prepare_after_reconnect(); metadata("", false);
    hub_update(); CHECK(submissions == 2 && draft.state == DRAFT_UNCERTAIN && !hub_send_pending());
    hub_close(); prepare_after_reconnect(); metadata(sent, true);
    CHECK(draft.state == DRAFT_EMPTY && !hub_send_pending() && submissions == 2);
    CHECK(chat_count == saved_count && !strcmp(chat[0].key, anchor));
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
    send_metadata();
    hub_close(); hub_reset_profile();
    DIR *dir = opendir("."); CHECK(dir); struct dirent *entry;
    while ((entry = readdir(dir))) if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, "..")) CHECK(!unlink(entry->d_name));
    closedir(dir); CHECK(!chdir("/")); CHECK(!rmdir(directory));
    puts("PASS saved older window, full-capacity anchor, incarnation change, failed checkpoint and metadata-only send recovery");
    return 0;
}

/* Real Hub cache/read recovery with a deterministic WebSocket boundary. */
#include "platform.h"
#include "hub.h"
#include "settings.h"
#include "websocket.h"
#include "json.h"
#include "cache.h"
#include <dirent.h>
#include <string.h>
#include <unistd.h>

static WebSocket *transport;
static unsigned request_id;
static char method[64], mutation[65];
static bool includes_turns;
static unsigned submissions;
static int item_limit, before_entry;
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
    if ((!strcmp(method, "thread/read") && includes_turns) || !strcmp(method, "thread/turns/list")) {
        item_limit = json_int(&j, json_get(&j, params, "itemLimit")); CHECK(item_limit == 10);
        before_entry = json_int(&j, json_get(&j, json_get(&j, params, "before"), "entry"));
        CHECK(json_is(&j, json_get(&j, params, "includeConversation"), "true"));
        const char *excluded[] = {"includeThinking", "includeToolCalls", "includeToolCallIntents", "includeSystemMessages", "includeDiagnostics"};
        for (unsigned i = 0; i < sizeof(excluded) / sizeof(*excluded); i++) CHECK(json_is(&j, json_get(&j, params, excluded[i]), "false"));
    }
    if (!strcmp(method, "turn/start") || !strcmp(method, "turn/queue")) {
        submissions++;
        CHECK(json_field(&j, params, "clientMutationId", mutation, sizeof(mutation)));
    }
    json_free(&j); return true;
}
static void reply_to(unsigned id, const char *result) {
    char bytes[16384];
    int n = snprintf(bytes, sizeof(bytes), "{\"id\":%u,\"result\":%s}", id, result);
    CHECK(n > 0 && n < sizeof(bytes));
    transport->on_message(transport, bytes, n);
}
static void reply(const char *result) { reply_to(request_id, result); }
static void page(const char *snapshot, int first, int count, bool older) {
    char result[12000];
    char cursor[32] = "";
    if (first > 900) snprintf(cursor, sizeof(cursor), "older-%d", first);
    int n = snprintf(result, sizeof(result), "{\"snapshot\":{\"incarnation\":\"%s\"},\"%s\":\"%s\",%s[{\"items\":[",
        snapshot, older ? "nextCursor" : "olderCursor", cursor, older ? "\"data\":" : "\"thread\":{\"turns\":");
    for (int i = 0; i < count; i++) {
        int used = snprintf(result + n, sizeof(result) - n, "%s{\"type\":\"userMessage\",\"id\":\"row%d\","
            "\"position\":{\"entry\":%d},\"text\":\"sentinel%d\"}", i ? "," : "", first + i, first + i, first + i);
        CHECK(used > 0 && used < sizeof(result) - n); n += used;
    }
    CHECK(snprintf(result + n, sizeof(result) - n, "]}]%s}", older ? "" : "}") < sizeof(result) - n);
    reply(result);
}
static void connect_hub(void) {
    hub_connect(); CHECK(!hub_foreground_busy()); hub_update(); CHECK(!strcmp(method, "initialize"));
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
static void request_priorities(void) {
    hub_close(); hub_reset_profile(); connect_hub();
    hub_update(); CHECK(!strcmp(method, "evener/navigation/read"));
    unsigned obsolete_navigation = request_id;
    NavigationRow row = {.ref = "priority-session"}; CHECK(hub_select(&row));
    CHECK(hub_foreground_busy()); hub_update(); CHECK(!strcmp(method, "thread/read"));
    unsigned selected_read = request_id;
    reply_to(obsolete_navigation, "{\"data\":{}}");
    CHECK(hub_foreground_busy() && hub_loading && request_id == selected_read && !hub_hydrated);
    hub_set_prefetch_paused(true); page("priority-snapshot", 1000, 10, false);
    hub_update(); CHECK(request_id == selected_read && !hub_foreground_busy() && !hub_loading);
    hub_set_prefetch_paused(false); voice_state = VOICE_RECORDING;
    hub_update(); CHECK(request_id == selected_read); voice_state = VOICE_OFF;
    hub_update(); CHECK(!strcmp(method, "thread/turns/list") && !hub_loading && !hub_foreground_busy());
    unsigned obsolete_history = request_id;
    CHECK(draft_set("priority-input", true)); hub_send(); hub_update();
    CHECK(!strcmp(method, "thread/read") && !includes_turns && hub_foreground_busy());
    int revision = chat_revision;
    reply_to(obsolete_history, "{\"snapshot\":{\"incarnation\":\"priority-snapshot\"},\"data\":[]}");
    CHECK(chat_revision == revision && hub_foreground_busy());
    metadata("", true);
    hub_close(); CHECK(!hub_foreground_busy()); hub_reset_profile();
}
static SDFile *(*open_file)(const char *, FileOptions);
static char written_path[128];
static SDFile *remember_write(const char *path, FileOptions mode) {
    if (mode & kFileWrite) snprintf(written_path, sizeof(written_path), "%s", path);
    return open_file(path, mode);
}
static void save_page_path(const HistoryCache *history, int page_number, const ChatMessage *rows, int count, char *path, size_t capacity) {
    const struct playdate_file *original_files = pd->file;
    struct playdate_file files = *original_files; open_file = files.open;
    files.open = remember_write; pd->file = &files;
    CHECK(cache_save_history_page(history, page_number, rows, count));
    pd->file = original_files;
    char *suffix = strrchr(written_path, '.'); CHECK(suffix && !strcmp(suffix, ".tmp")); *suffix = 0;
    snprintf(path, capacity, "%s", written_path);
}
static void corrupt_page(const HistoryCache *history, int page_number) {
    ChatMessage rows[HISTORY_PAGE_ITEMS] = {0}; int count;
    CHECK(cache_load_history_page(history, page_number, rows, &count));
    char path[128]; save_page_path(history, page_number, rows, count, path, sizeof(path));
    for (int i = 0; i < count; i++) free(rows[i].text);
    FILE *file = fopen(path, "r+b"); CHECK(file && !fseek(file, -1, SEEK_END));
    int byte = fgetc(file); CHECK(byte != EOF && !fseek(file, -1, SEEK_END));
    CHECK(fputc(byte ^ 0xff, file) != EOF && !fclose(file));
    CHECK(!cache_load_history_page(history, page_number, rows, &count));
}
static void copy_file(const char *from, const char *to) {
    FILE *source = fopen(from, "rb"), *target = fopen(to, "wb"); CHECK(source && target);
    char bytes[1024]; size_t count;
    while ((count = fread(bytes, 1, sizeof(bytes), source))) CHECK(fwrite(bytes, 1, count, target) == count);
    CHECK(!ferror(source) && !fclose(source) && !fclose(target));
}
static void cache_failures(void) {
    NavigationRow row = {.ref = "failure-session"}; CHECK(hub_select(&row)); read_latest("failure-snapshot", 10);
    int revision = chat_revision;
    hub_update(); CHECK(!strcmp(method, "thread/turns/list"));
    fail_write = true; page("failure-snapshot", 990, 10, true); fail_write = false;
    CHECK(chat_count == 10 && chat_revision == revision && chat[0].entry == 1000);
    HistoryCache history; CHECK(cache_load_history(&history) && history.oldest == history.newest);
    unsigned failed_request = request_id; hub_update(); CHECK(request_id == failed_request);
    hub_older(); hub_update(); CHECK(!strcmp(method, "thread/turns/list"));
    page("failure-snapshot", 990, 10, true); CHECK(chat_count == 20 && chat[0].entry == 990);
    CHECK(cache_load_history(&history)); int committed_oldest = history.oldest;
    hub_update(); fail_rename = true; page("failure-snapshot", 980, 10, true); fail_rename = false;
    CHECK(cache_load_history(&history) && history.oldest == committed_oldest && chat_count == 20);
    ChatMessage staged[HISTORY_PAGE_ITEMS] = {0}; int staged_count;
    CHECK(cache_load_history_page(&history, committed_oldest - 1, staged, &staged_count) && staged_count == 10);
    for (int i = 0; i < staged_count; i++) free(staged[i].text);
    hub_close(); hub_reset_profile(); CHECK(hub_select(&row)); read_latest("failure-snapshot", 10);
    CHECK(chat_count == 20 && chat[0].entry == 990);
    hub_update(); CHECK(before_entry == 990); page("failure-snapshot", 980, 10, true);
    CHECK(cache_load_history(&history) && history.oldest == committed_oldest - 1 && chat_count == 20);
    hub_latest(); hub_update(); page("failure-snapshot", 1000, 10, false); CHECK(chat_count == 10);
    CHECK(cache_load_history(&history)); committed_oldest = history.oldest;
    corrupt_page(&history, history.newest - 1);
    revision = chat_revision; hub_older(); CHECK(chat_revision == revision && chat[0].entry == 1000);
    hub_update(); CHECK(!strcmp(method, "thread/turns/list") && before_entry == 1000);
    page("failure-snapshot", 990, 10, true);
    CHECK(chat[0].entry == 990 && chat_count == 20);
    CHECK(cache_load_history(&history) && history.oldest == committed_oldest);
    hub_update(); CHECK(!strcmp(method, "thread/turns/list"));
    char error[128]; int length = snprintf(error, sizeof(error), "{\"id\":%u,\"error\":{\"code\":-32001}}", request_id);
    transport->on_message(transport, error, length);
    hub_update(); CHECK(!strcmp(method, "thread/read") && includes_turns && chat_count == 20);
    page("failure-snapshot", 1000, 10, false); CHECK(chat_count == 20 && chat[0].entry == 990);
    hub_close(); hub_reset_profile();
}
static void live_history(void) {
    NavigationRow row = {.ref = "live-history-session"}; CHECK(hub_select(&row)); read_latest("live-snapshot", 10);
    for (int first = 990; first >= 900; first -= 10) { hub_update(); page("live-snapshot", first, 10, true); }
    for (int i = 0; i < 10; i++) { hub_set_reading_anchor(chat[0].key); hub_older(); }
    CHECK(chat[0].entry == 900 && hub_has_newer);
    int revision = chat_revision;
    for (int entry = 1010; entry <= 1030; entry++) {
        char notification[1024];
        int length = snprintf(notification, sizeof(notification), "{\"method\":\"history/updated\",\"params\":{"
            "\"ref\":\"live-history-session\",\"snapshot\":{\"incarnation\":\"live-snapshot\"},\"bootGeneration\":\"\",\"epoch\":0,"
            "\"items\":[{\"type\":\"agentMessage\",\"id\":\"row%d\",\"position\":{\"entry\":%d},\"text\":\"sentinel-live-%d\"}]}}", entry, entry, entry);
        CHECK(length > 0 && length < sizeof(notification)); transport->on_message(transport, notification, length);
        CHECK(chat[0].entry == 900 && chat_revision == revision);
    }
    for (int i = 0; hub_has_newer && i < 30; i++) { hub_set_reading_anchor(chat[chat_count - 1].key); hub_newer(); }
    CHECK(!hub_has_newer && chat[chat_count - 1].entry == 1030 && chat_count <= CHAT_CAPACITY);
    hub_close(); hub_reset_profile();
}
static void generation_cleanup(void) {
    NavigationRow row = {.ref = "cleanup-session"}; CHECK(hub_select(&row)); read_latest("cleanup-a", 10);
    hub_update(); page("cleanup-a", 990, 10, true);
    HistoryCache retired; CHECK(cache_load_history(&retired));
    read_latest("cleanup-b", 10);
    HistoryCache current; CHECK(cache_load_history(&current) && current.previous_generation == retired.generation);
    ChatMessage rows[HISTORY_PAGE_ITEMS] = {0}; int count;
    cache_reclaim_history(&current, retired.generation);
    CHECK(cache_load_history_page(&retired, retired.oldest, rows, &count));
    for (int i = 0; i < count; i++) free(rows[i].text);
    fail_unlink = true; cache_reclaim_history(&current, current.generation); fail_unlink = false;
    CHECK(cache_load_history_page(&retired, retired.oldest, rows, &count));
    char retired_path[128], unrelated_path[128];
    save_page_path(&retired, retired.oldest, rows, count, retired_path, sizeof(retired_path));
    HistoryCache unrelated = retired; unrelated.generation += 100;
    save_page_path(&unrelated, retired.oldest, rows, count, unrelated_path, sizeof(unrelated_path));
    copy_file(unrelated_path, retired_path);
    cache_reclaim_history(&current, current.generation);
    CHECK(!access(retired_path, F_OK));
    CHECK(cache_save_history_page(&retired, retired.oldest, rows, count)); CHECK(!unlink(unrelated_path));
    for (int i = 0; i < count; i++) free(rows[i].text);
    for (int i = 0; i < 20 && (current.previous_generation || current.cleanup_generation); i++) {
        cache_reclaim_history(&current, current.generation);
        CHECK(cache_load_history(&current));
    }
    CHECK(!current.previous_generation && !current.cleanup_generation);
    CHECK(!cache_load_history_page(&retired, retired.oldest, rows, &count));
    CHECK(cache_load_history_page(&current, current.newest, rows, &count) && count == 10);
    for (int i = 0; i < count; i++) free(rows[i].text);
    CHECK(chat_count == 10 && chat[0].entry == 1000);
    hub_close(); hub_reset_profile();
}
static void contiguous_window(void) {
    CHECK(chat_count <= CHAT_CAPACITY);
    for (int i = 1; i < chat_count; i++) CHECK(chat[i].entry == chat[i - 1].entry + 1);
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
    read_latest("snapshot-full", 10);
    int revision = chat_revision;
    for (int first = 990; first >= 900; first -= 10) {
        hub_update(); CHECK(!strcmp(method, "thread/turns/list") && !hub_loading && !hub_foreground_busy());
        page("snapshot-full", first, 10, true);
        CHECK(chat_count == 10 && chat_revision == revision && chat[0].entry == 1000);
    }
    unsigned fetched = request_id;
    for (int i = 0; i < 10; i++) { hub_set_reading_anchor(chat[0].key); hub_older(); contiguous_window(); }
    CHECK(chat_count == CHAT_CAPACITY && chat[0].entry == 900 && !hub_has_older && hub_has_newer);
    CHECK(request_id == fetched);
    for (int i = 0; hub_has_newer && i < 20; i++) { hub_set_reading_anchor(chat[chat_count - 1].key); hub_newer(); contiguous_window(); }
    CHECK(!hub_has_newer && chat[chat_count - 1].entry == 1009 && request_id == fetched);
    for (int i = 0; hub_has_older && i < 20; i++) { hub_set_reading_anchor(chat[0].key); hub_older(); contiguous_window(); }
    CHECK(chat[0].entry == 900 && chat_count == CHAT_CAPACITY);
    CHECK(hub_save_view(snapshot, sizeof(snapshot)));
    hub_close(); hub_reset_profile(); CHECK(hub_restore_view(&row, snapshot));
    if (!unpinned) hub_set_reading_anchor("row900");
    read_latest("snapshot-full", 10);
    CHECK(chat_count == CHAT_CAPACITY && chat[0].entry == 900);
    hub_set_reading_anchor(chat[chat_count - 1].key); hub_newer(); CHECK(hub_has_older);
    send_metadata();
    hub_close(); hub_reset_profile();
    request_priorities();
    cache_failures();
    live_history();
    generation_cleanup();
    DIR *dir = opendir("."); CHECK(dir); struct dirent *entry;
    while ((entry = readdir(dir))) if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, "..")) CHECK(!unlink(entry->d_name));
    closedir(dir); CHECK(!chdir("/")); CHECK(!rmdir(directory));
    puts("PASS ten-item read, background archive, bounded bidirectional history, restart, cache faults and request priority");
    return 0;
}

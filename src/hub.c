#include "hub.h"
#include "tailnet_device.h"
#include "app.h"
#include "json.h"
#include "websocket.h"
#include "cache.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

NavigationRow navigation[NAV_PAGE_SIZE];
int navigation_count, navigation_offset, navigation_remaining, navigation_revision;
enum BrowseKind browse_kind;
int browse_tier;
char browse_project[REF_CAPACITY], browse_title[160];
ChatMessage chat[CHAT_CAPACITY];
int chat_count, chat_revision;
char selected_ref[REF_CAPACITY], selected_title[160], session_status[32], hub_status[256];
bool hub_ready, hub_loading, hub_has_older, hub_hydrated;

enum Request { REQUEST_NONE, REQUEST_INIT, REQUEST_NAV, REQUEST_READ, REQUEST_OLDER, REQUEST_SEND, REQUEST_RESUME, REQUEST_PREPARE_SEND };
enum { HISTORY_PAGE_ITEMS = 8 };
static const char history_content[] = "\"includeConversation\":true,\"includeThinking\":false,\"includeToolCalls\":false,\"includeToolCallIntents\":false,\"includeSystemMessages\":false,\"includeDiagnostics\":false";
static WebSocket socket;
static enum Request pending;
static unsigned serial, request_id, browse_generation, requested_browse;
static char request_ref[REF_CAPACITY], request_mutation[65], older_cursor[2048];
static char incarnation[128], boot_generation[128], instance[128];
static int epoch, requested_offset, retry_delay = 1;
static float request_time, reconnect_at, browse_at, cache_at;
static int saved_revision = -1;
static bool initialized, want_browse, want_read, want_older, want_send, replay_allowed, send_prepared;
static bool can_send, can_queue, resume_required, resume_foldable, suspended, rebase_older, replace_chat;
static char outgoing[57344], quoted_text[DRAFT_CAPACITY * 6 + 3];
static char text_buffer[32768], reading_anchor[192];

static bool same(const char *a, const char *b) { return strcmp(a, b) == 0; }
static void status(const char *message) { snprintf(hub_status, sizeof(hub_status), "%s", message); }
static void connected_status(void) {
    if (want_send) status("Preparing to send...");
    else if (draft.state == DRAFT_UNCERTAIN) status("Checking delivery...");
    else if (selected_ref[0] && (!hub_hydrated || want_read)) status("Loading conversation...");
    else status("Connected");
}
static void save_chat(void) {
    if (hub_hydrated && selected_ref[0] && saved_revision != chat_revision) {
        if (cache_save_chat()) saved_revision = chat_revision;
        else app_log("Could not save conversation cache");
        cache_at = app_time() + 2;
    }
}
static void remove_chat(int index) {
    free(chat[index].text);
    memmove(chat + index, chat + index + 1, (chat_count - index - 1) * sizeof(*chat));
    chat_count--;
    chat_revision++;
}
static void clear_chat(void) { while (chat_count) remove_chat(chat_count - 1); }
static void clear_overlays(const char *round, const char *stream) {
    for (int i = chat_count - 1; i >= 0; i--)
        if (chat[i].overlay && (!round || same(chat[i].round, round)) &&
            (!stream || same(chat[i].stream, stream))) remove_chat(i);
}
static int compare_chat(const void *left, const void *right) {
    const ChatMessage *a = left, *b = right;
    if (a->overlay != b->overlay) return a->overlay ? 1 : -1;
    if (a->entry != b->entry) return a->entry < b->entry ? -1 : 1;
    if (a->item != b->item) return a->item < b->item ? -1 : 1;
    return (a->sub > b->sub) - (a->sub < b->sub);
}
static bool set_text(ChatMessage *row, const char *value, bool clipped) {
    size_t length = strlen(value);
    char *copy = realloc(row->text, length + 1);
    if (!copy) { status("Not enough memory for this message"); return false; }
    row->text = copy;
    memcpy(copy, value, length + 1);
    row->clipped = clipped;
    chat_revision++;
    return true;
}
static void settle_draft(void) {
    bool selected = same(draft.ref, selected_ref);
    bool saved = draft_clear();
    if (selected) {
        status(saved ? "Message accepted" : "Message accepted; saving receipt failed. Rechecking is safe.");
        want_send = replay_allowed = false;
    }
}
static void observe_mutation(const Json *j, int object) {
    if (!draft.mutation[0] || !same(draft.ref, selected_ref)) return;
    if (json_is(j, json_get(j, object, "clientMutationId"), draft.mutation)) settle_draft();
}
static void chat_item(const Json *j, int item, int overlay, bool backfill) {
    observe_mutation(j, item);
    int type = json_get(j, item, "type");
    bool user = json_is(j, type, "userMessage") ||
        (json_is(j, type, "steering") && json_is(j, json_get(j, item, "source"), "user"));
    if (!user && !json_is(j, type, "agentMessage")) return;
    char key[192];
    if (!json_field(j, overlay >= 0 ? overlay : item, overlay >= 0 ? "key" : "transcriptKey", key, sizeof(key)) || !key[0])
        if (!json_field(j, item, "id", key, sizeof(key)) || !key[0]) return;
    int index = -1;
    for (int i = 0; i < chat_count; i++) if (same(chat[i].key, key)) { index = i; break; }
    int version = json_int(j, json_get(j, item, "version"));
    if (index >= 0 && version < chat[index].version) return;
    if (index < 0) {
        if (chat_count == CHAT_CAPACITY) {
            // Backfill moves the window toward older messages; Latest restores its tail.
            int remove = backfill ? chat_count - 1 : 0;
            if (same(chat[remove].key, reading_anchor)) remove = backfill ? 0 : chat_count - 1;
            remove_chat(remove);
            if (!backfill) hub_has_older = rebase_older = true;
        }
        index = chat_count++;
        memset(&chat[index], 0, sizeof(chat[index]));
        snprintf(chat[index].key, sizeof(chat[index].key), "%s", key);
    }
    ChatMessage *row = &chat[index];
    row->user = user; row->overlay = overlay >= 0; row->version = version;
    int position = json_get(j, item, "position");
    row->entry = json_int(j, json_get(j, position, "entry"));
    row->item = json_int(j, json_get(j, position, "item"));
    row->sub = json_int(j, json_get(j, position, "sub"));
    json_field(j, item, "roundId", row->round, sizeof(row->round));
    if (overlay >= 0) {
        json_field(j, overlay, "streamId", row->stream, sizeof(row->stream));
        json_field(j, overlay, "roundId", row->round, sizeof(row->round));
    }
    bool complete = json_field(j, item, "text", text_buffer, sizeof(text_buffer)) || json_get(j, item, "text") < 0;
    set_text(row, text_buffer, !complete);
    if (!row->overlay && !user && row->round[0]) {
        char round[128]; snprintf(round, sizeof(round), "%s", row->round);
        clear_overlays(round, NULL);
    }
    qsort(chat, chat_count, sizeof(*chat), compare_chat);
}
static void chat_array(const Json *j, int array, bool backfill) {
    for (int i = json_first(j, array); i >= 0 && i < json_next(j, array); i = json_next(j, i))
        chat_item(j, i, -1, backfill);
}
static void turns(const Json *j, int array, bool backfill) {
    for (int i = json_first(j, array); i >= 0 && i < json_next(j, array); i = json_next(j, i))
        chat_array(j, json_get(j, i, "items"), backfill);
}
static void capabilities(const Json *j, int object) {
    can_send = json_is(j, json_get(j, object, "send"), "true");
    can_queue = json_is(j, json_get(j, object, "queue"), "true");
}
static void thread_metadata(const Json *j, int thread) {
    json_field(j, json_get(j, thread, "status"), "type", session_status, sizeof(session_status));
    int evener = json_get(j, thread, "evener");
    json_field(j, evener, "instanceId", instance, sizeof(instance));
    capabilities(j, json_get(j, evener, "capabilities"));
    resume_required = json_is(j, json_get(j, evener, "resumeRequired"), "true");
    resume_foldable = json_is(j, json_get(j, evener, "resumeOnlyFoldable"), "true");
    int mutations = json_get(j, evener, "pendingMutations");
    for (int i = json_first(j, mutations); i >= 0 && i < json_next(j, mutations); i = json_next(j, i)) observe_mutation(j, i);
    int preview = json_get(j, json_get(j, evener, "queue"), "preview");
    for (int i = json_first(j, preview); i >= 0 && i < json_next(j, preview); i = json_next(j, i)) observe_mutation(j, i);
    replay_allowed = json_is(j, json_get(j, evener, "mutationStateAuthoritative"), "true");
}
static void navigation_result(const Json *j, int result) {
    if (requested_browse != browse_generation) return;
    int data = json_get(j, result, "data"), entities = json_get(j, data, "entities");
    int containers = json_get(j, data, "containers"), children = -1;
    for (int i = json_first(j, containers); i >= 0 && i < json_next(j, containers); i = json_next(j, i)) {
        int owner = json_get(j, i, "owner");
        if (json_is(j, json_get(j, owner, "kind"), "resource_root")) { children = json_get(j, i, "children"); break; }
    }
    navigation_count = 0;
    for (int i = json_first(j, children); i >= 0 && i < json_next(j, children) && navigation_count < NAV_PAGE_SIZE; i = json_next(j, i)) {
        char key[512]; if (!json_text(j, i, key, sizeof(key))) continue;
        for (int e = json_first(j, entities); e >= 0 && e < json_next(j, entities); e = json_next(j, e)) {
            if (!json_is(j, json_get(j, e, "key"), key)) continue;
            NavigationRow row = {0}; int value = json_get(j, e, "value");
            bool project = browse_kind == BROWSE_PROJECTS;
            if (!json_field(j, value, project ? "key" : "ref", row.ref, sizeof(row.ref))) break;
            json_field(j, value, project ? "name" : "title", row.title, sizeof(row.title));
            if (!row.title[0]) snprintf(row.title, sizeof(row.title), "Untitled session");
            if (project) snprintf(row.state, sizeof(row.state), "%d sessions", json_int(j, json_get(j, value, "session_count")));
            else json_field(j, value, "state", row.state, sizeof(row.state));
            navigation[navigation_count++] = row;
            break;
        }
    }
    int metadata = json_get(j, data, "metadata");
    navigation_remaining = json_int(j, json_get(j, metadata, "remaining"));
    navigation_offset = requested_offset;
    navigation_revision++;
    if (!cache_save_navigation()) app_log("Could not save session list cache");
    connected_status();
}
static void read_result(const Json *j, int result, bool backfill) {
    if (!same(request_ref, selected_ref)) return;
    char incoming[128];
    json_field(j, json_get(j, result, "snapshot"), "incarnation", incoming, sizeof(incoming));
    if (backfill && incarnation[0] && !same(incoming, incarnation)) { want_read = true; return; }
    if (!backfill) {
        if (replace_chat || (incarnation[0] && !same(incoming, incarnation))) clear_chat();
        replace_chat = false;
        snprintf(incarnation, sizeof(incarnation), "%s", incoming);
        json_field(j, result, "bootGeneration", boot_generation, sizeof(boot_generation));
        epoch = json_int(j, json_get(j, result, "epoch"));
        clear_overlays(NULL, NULL);
        int thread = json_get(j, result, "thread");
        thread_metadata(j, thread);
        turns(j, json_get(j, thread, "turns"), false);
        chat_array(j, json_get(j, json_get(j, result, "changes"), "items"), false);
        int overlay = json_get(j, result, "overlay");
        for (int i = json_first(j, overlay); i >= 0 && i < json_next(j, overlay); i = json_next(j, i))
            chat_item(j, json_get(j, i, "item"), i, false);
        hub_hydrated = true;
        app_log("Hub read: %d chat messages; session %s", chat_count, session_status);
    } else turns(j, json_get(j, result, "data"), true);
    if (!backfill && chat_count) rebase_older = true;
    else if (backfill) rebase_older = false;
    json_field(j, result, backfill ? "nextCursor" : "olderCursor", older_cursor, sizeof(older_cursor));
    hub_has_older = older_cursor[0] != 0;
    if (hub_has_older && chat_count < 4) want_older = true;
    if (draft.state == DRAFT_UNCERTAIN && replay_allowed) want_send = true;
    connected_status();
}
static void submission_result(const Json *j, int result, int error) {
    static Draft current;
    current = draft;
    bool switched = !same(draft.ref, request_ref);
    if (switched && !draft_load(request_ref)) { status("Could not read the submitted draft"); return; }
    if (!same(draft.mutation, request_mutation)) { if (switched) draft = current; return; }
    int receipt = json_get(j, result, "receipt");
    if (json_is(j, json_get(j, receipt, "clientMutationId"), draft.mutation) &&
        (json_is(j, json_get(j, receipt, "disposition"), "applied") ||
         json_is(j, json_get(j, receipt, "disposition"), "replayed"))) settle_draft();
    else {
        int data = json_get(j, error, "data"), outcome = json_get(j, data, "mutationOutcome");
        bool ours = json_is(j, json_get(j, data, "clientMutationId"), draft.mutation);
        int code = json_int(j, json_get(j, error, "code"));
        bool refused = (ours && json_is(j, outcome, "notAccepted")) || code == -32600 || code == -32602;
        draft.state = refused ? DRAFT_READY : DRAFT_UNCERTAIN;
        if (refused) draft.mutation[0] = draft.instance[0] = draft.method[0] = 0;
        draft_save();
        if (same(draft.ref, selected_ref)) {
            char message[192]; json_field(j, error, "message", message, sizeof(message));
            status(message[0] ? message : "Delivery unconfirmed; draft saved");
            replay_allowed = false;
        }
    }
    if (switched) draft = current;
}
static void notification(const Json *j, int method, int params) {
    if (json_is(j, method, "evener/navigation/invalidated")) {
        want_browse = true; return;
    }
    if (!selected_ref[0] || !json_is(j, json_get(j, params, "ref"), selected_ref)) return;
    if (json_is(j, method, "history/updated")) {
        char incoming[128]; json_field(j, json_get(j, params, "snapshot"), "incarnation", incoming, sizeof(incoming));
        if (!hub_hydrated || !same(incoming, incarnation) ||
            !json_is(j, json_get(j, params, "bootGeneration"), boot_generation) ||
            json_int(j, json_get(j, params, "epoch")) != epoch) { want_read = true; return; }
        chat_array(j, json_get(j, params, "items"), false);
    } else if (json_is(j, method, "overlay/upserted")) {
        int overlay = json_get(j, params, "item");
        chat_item(j, json_get(j, overlay, "item"), overlay, false);
    } else if (json_is(j, method, "overlay/delta") && json_is(j, json_get(j, params, "field"), "text")) {
        for (int i = 0; i < chat_count; i++) if (chat[i].overlay && json_is(j, json_get(j, params, "key"), chat[i].key)) {
            size_t n = strlen(chat[i].text);
            utf8_copy(text_buffer, sizeof(text_buffer), chat[i].text, n);
            bool full = json_field(j, params, "delta", text_buffer + n, sizeof(text_buffer) - n);
            set_text(&chat[i], text_buffer, chat[i].clipped || !full);
            break;
        }
    } else if (json_is(j, method, "overlay/reset")) {
        char stream[128]; if (json_field(j, params, "streamId", stream, sizeof(stream))) clear_overlays(NULL, stream);
    } else if (json_is(j, method, "overlay/end")) {
        char round[128]; if (json_field(j, params, "roundId", round, sizeof(round))) clear_overlays(round, NULL);
    } else if (json_is(j, method, "thread/status/changed")) {
        json_field(j, json_get(j, params, "status"), "type", session_status, sizeof(session_status));
        int caps = json_get(j, params, "capabilities");
        if (caps >= 0) capabilities(j, caps);
    } else if (json_is(j, method, "evener/thread/resync")) want_read = true;
}
static void message(WebSocket *ws, const char *text, size_t length) {
    (void)ws;
    Json j;
    if (!json_parse(&j, text, length)) { status("Hub response is too complex to read"); return; }
    int id = json_get(&j, 0, "id"), method = json_get(&j, 0, "method");
    if (method >= 0 && id < 0) notification(&j, method, json_get(&j, 0, "params"));
    else if (id >= 0 && pending != REQUEST_NONE && (unsigned)json_int(&j, id) == request_id) {
        enum Request completed = pending; pending = REQUEST_NONE; hub_loading = false;
        app_log("Hub request %u completed; kind %d; %lu bytes; %u ms", request_id, completed,
            (unsigned long)length, (unsigned)((app_time() - request_time) * 1000));
        int result = json_get(&j, 0, "result"), error = json_get(&j, 0, "error");
        if (completed == REQUEST_SEND) submission_result(&j, result, error);
        else if (error >= 0) {
            char reason[224]; json_field(&j, error, "message", reason, sizeof(reason));
            app_log("Hub request %u kind %d failed: %s", request_id, completed, reason);
            status(reason[0] ? reason : "Hub request failed");
            want_send = false;
        } else if (completed == REQUEST_INIT) {
            if (json_is(&j, json_get(&j, result, "protocolVersion"), "evener-appwire-v7")) initialized = true;
            else status("Hub protocol version is not supported");
        } else if (completed == REQUEST_NAV) navigation_result(&j, result);
        else if (completed == REQUEST_PREPARE_SEND) {
            if (same(request_ref, selected_ref)) {
                thread_metadata(&j, json_get(&j, result, "thread"));
                send_prepared = true;
                connected_status();
            }
        } else if (completed == REQUEST_READ || completed == REQUEST_OLDER) read_result(&j, result, completed == REQUEST_OLDER);
        else if (completed == REQUEST_RESUME) { want_read = true; hub_hydrated = send_prepared = false; }
    }
    json_free(&j);
}
static bool request(enum Request kind, const char *method, const char *params) {
    if (pending || !ws_writable(&socket)) return false;
    unsigned id = ++serial;
    int n = snprintf(outgoing, sizeof(outgoing), "{\"id\":%u,\"method\":\"%s\",\"params\":%s}", id, method, params);
    if (n < 0 || n >= sizeof(outgoing) || !ws_send(&socket, outgoing)) return false;
    pending = kind; request_id = id; request_time = app_time(); hub_loading = true;
    app_log("Hub request %u: %s", id, method);
    snprintf(request_ref, sizeof(request_ref), "%s", selected_ref);
    return true;
}
void hub_close(void) {
    save_chat();
    suspended = true;
    ws_close(&socket);
    hub_ready = hub_hydrated = initialized = send_prepared = false;
    pending = REQUEST_NONE;
    if (draft.state == DRAFT_SENDING) { draft.state = DRAFT_UNCERTAIN; draft_save(); }
}
void hub_connect(void) {
    suspended = false;
    ws_connect(&socket, config.hub_host, config.hub_port, config.hub_tls, config.hub_path,
        config.hub_token, 2097152, message);
    hub_ready = hub_hydrated = initialized = replay_allowed = send_prepared = false;
    pending = REQUEST_NONE;
    want_browse = true; want_read = selected_ref[0] != 0;
    if (draft.state == DRAFT_UNCERTAIN) want_send = true;
    if (!navigation_count) cache_load_navigation();
    status(config.use_tailnet ? tailnet_device_status() :
        navigation_count || chat_count ? "Connecting... Saved content available." : "Connecting to Evener...");
}
void hub_reset_profile(void) {
    clear_chat(); memset(&draft, 0, sizeof(draft)); reading_anchor[0] = 0;
    selected_ref[0] = selected_title[0] = session_status[0] = 0;
    incarnation[0] = instance[0] = boot_generation[0] = older_cursor[0] = 0;
    navigation_count = navigation_offset = navigation_remaining = browse_tier = 0;
    browse_kind = BROWSE_LIVE; browse_project[0] = browse_title[0] = 0;
    navigation_revision++; browse_generation++;
    want_read = want_older = want_send = replay_allowed = send_prepared = false;
    can_send = can_queue = resume_required = resume_foldable = replace_chat = rebase_older = false;
    hub_ready = hub_loading = hub_has_older = hub_hydrated = initialized = false;
    pending = REQUEST_NONE; saved_revision = -1; retry_delay = 1; reconnect_at = browse_at = cache_at = 0;
}
void hub_browse(enum BrowseKind kind, const char *project, const char *title, int tier, int offset) {
    browse_kind = kind; browse_tier = tier; navigation_offset = offset;
    if (project) snprintf(browse_project, sizeof(browse_project), "%s", project);
    if (title) snprintf(browse_title, sizeof(browse_title), "%s", title);
    browse_generation++; want_browse = true; browse_at = 0;
    navigation_count = navigation_remaining = 0; navigation_revision++;
    if (cache_load_navigation()) status("Saved session list; updating...");
}
bool hub_select(const NavigationRow *row) {
    if (draft.ref[0] && !draft_save()) { status("Could not save your draft"); return false; }
    if (!draft_load(row->ref)) { status("Could not read this session's saved draft"); return false; }
    save_chat();
    snprintf(selected_ref, sizeof(selected_ref), "%s", row->ref);
    snprintf(selected_title, sizeof(selected_title), "%s", row->title);
    clear_chat(); incarnation[0] = older_cursor[0] = session_status[0] = reading_anchor[0] = 0;
    hub_hydrated = hub_has_older = replay_allowed = send_prepared = false;
    replace_chat = false;
    if (cache_load_chat()) { replace_chat = true; status("Saved conversation; updating..."); }
    want_read = true; want_older = want_send = false;
    return true;
}
void hub_set_reading_anchor(const char *key) { snprintf(reading_anchor, sizeof(reading_anchor), "%s", key); }
bool hub_save_view(char *snapshot, size_t capacity) {
    if (strlen(incarnation) >= capacity) return false;
    if (selected_ref[0] && incarnation[0] && saved_revision != chat_revision) {
        if (!cache_save_chat()) return false;
        saved_revision = chat_revision;
    }
    snprintf(snapshot, capacity, "%s", incarnation);
    return true;
}
bool hub_restore_view(const NavigationRow *row, const char *snapshot) {
    if (!hub_select(row)) return false;
    if (snapshot[0] && chat_count) {
        // Only the checkpoint's successfully saved window has this identity.
        // A fresh read still replaces it if the server incarnation changed.
        snprintf(incarnation, sizeof(incarnation), "%s", snapshot);
        replace_chat = false;
    }
    return true;
}
void hub_older(void) { if (hub_has_older) want_older = true; }
void hub_latest(void) { save_chat(); replace_chat = true; want_read = true; want_older = false; }
void hub_recheck(void) { want_read = selected_ref[0] != 0; if (!hub_ready) reconnect_at = 0; }
bool hub_send_pending(void) { return want_send; }
void hub_send(void) {
    if (draft.state == DRAFT_READY && draft.text[0] && !want_send) {
        want_send = true; send_prepared = false; status("Preparing to send...");
    }
}
static void send_draft(void) {
    static char ref[REF_CAPACITY * 6 + 3], expected[128 * 6 + 3], params[55000];
    if (!send_prepared || !same(draft.ref, selected_ref)) return;
    if (draft.state == DRAFT_UNCERTAIN && !replay_allowed) { want_send = false; return; }
    if (draft.state == DRAFT_READY) {
        if (resume_required && !resume_foldable) {
            json_quote(selected_ref, ref, sizeof(ref));
            snprintf(params, sizeof(params), "{\"ref\":%s}", ref);
            if (request(REQUEST_RESUME, "thread/resume", params)) status("Resuming session...");
            return;
        }
        bool active = same(session_status, "active");
        if ((active && !can_queue) || (!active && !can_send && !resume_foldable)) {
            status("This session cannot accept a message yet. Draft saved."); want_send = false; return;
        }
        uint8_t random[16]; ws_random(random, sizeof(random));
        for (int i = 0; i < 16; i++) snprintf(draft.mutation + i * 2, 3, "%02x", random[i]);
        snprintf(draft.instance, sizeof(draft.instance), "%s", instance);
        snprintf(draft.method, sizeof(draft.method), "%s", active ? "turn/queue" : "turn/start");
    } else if (draft.state != DRAFT_UNCERTAIN) { want_send = false; return; }
    json_quote(draft.ref, ref, sizeof(ref)); json_quote(draft.instance, expected, sizeof(expected));
    if (!json_quote(draft.text, quoted_text, sizeof(quoted_text))) { status("Draft could not be encoded"); want_send = false; return; }
    snprintf(params, sizeof(params), "{\"ref\":%s,\"expectedInstanceId\":%s,\"clientMutationId\":\"%s\",\"input\":[{\"type\":\"text\",\"text\":%s}]}", ref, expected, draft.mutation, quoted_text);
    enum DraftState previous = draft.state;
    draft.state = DRAFT_SENDING;
    if (!draft_save()) { draft.state = previous; status("Could not save message before sending"); want_send = false; return; }
    if (request(REQUEST_SEND, draft.method, params)) {
        snprintf(request_mutation, sizeof(request_mutation), "%s", draft.mutation);
        want_send = false; status(same(draft.method, "turn/queue") ? "Queueing message..." : "Sending message...");
    } else { draft.state = DRAFT_UNCERTAIN; draft_save(); }
}
void hub_update(void) {
    if (suspended) return;
    if (app_time() >= cache_at && voice_state != VOICE_RECORDING && voice_state != VOICE_FINISHING) save_chat();
    ws_update(&socket);
    if (socket.state == WS_FAILED || (pending && app_time() - request_time > 35)) {
        if (socket.state == WS_FAILED) app_log("Hub disconnected: %s", socket.error);
        if (pending) app_log("Hub request %u interrupted; kind %d; %lu buffered bytes; %u ms", request_id,
            pending, (unsigned long)socket.message_length, (unsigned)((app_time() - request_time) * 1000));
        hub_close(); suspended = false;
        reconnect_at = app_time() + retry_delay;
        retry_delay = retry_delay < 16 ? retry_delay * 2 : 16;
        status(socket.error[0] ? socket.error : "Reconnecting... Your draft is saved.");
    }
    if (socket.state == WS_CLOSED && app_time() >= reconnect_at) hub_connect();
    if (!ws_writable(&socket) || pending) return;
    if (!initialized) {
        request(REQUEST_INIT, "initialize", "{\"protocolVersion\":\"evener-appwire-v7\",\"clientInfo\":{\"name\":\"playdate\",\"version\":\"0.1.0\"},\"capabilities\":{\"experimentalApi\":false}}");
        return;
    }
    if (!hub_ready) {
        if (ws_send(&socket, "{\"method\":\"initialized\",\"params\":{}}")) { hub_ready = true; retry_delay = 1; connected_status(); }
        return;
    }
    static char params[16384], ref[REF_CAPACITY * 6 + 3]; json_quote(selected_ref, ref, sizeof(ref));
    if (want_send) {
        // Session metadata carries capabilities and mutation authority without
        // downloading conversation history again before dispatch or recovery.
        if (send_prepared) send_draft();
        else {
            snprintf(params, sizeof(params), "{\"ref\":%s,\"includeTurns\":false,%s}", ref, history_content);
            request(REQUEST_PREPARE_SEND, "thread/read", params);
        }
    } else if (want_read) {
        snprintf(params, sizeof(params), "{\"ref\":%s,\"includeTurns\":true,\"subscribe\":true,\"replaceSubscription\":true,\"itemLimit\":%d,%s}", ref, HISTORY_PAGE_ITEMS, history_content);
        if (request(REQUEST_READ, "thread/read", params)) want_read = false;
    } else if (want_older && hub_has_older) {
        static char cursor[12291]; json_quote(older_cursor, cursor, sizeof(cursor));
        if (rebase_older && chat_count && !chat[0].overlay)
            snprintf(params, sizeof(params), "{\"ref\":%s,\"cursor\":%s,\"before\":{\"entry\":%d,\"item\":%d,\"sub\":%d},\"itemLimit\":%d,%s}", ref, cursor, chat[0].entry, chat[0].item, chat[0].sub, HISTORY_PAGE_ITEMS, history_content);
        else snprintf(params, sizeof(params), "{\"ref\":%s,\"cursor\":%s,\"itemLimit\":%d,%s}", ref, cursor, HISTORY_PAGE_ITEMS, history_content);
        if (request(REQUEST_OLDER, "thread/turns/list", params)) want_older = false;
    } else if (want_browse && app_time() >= browse_at) {
        const char *tiers[] = {"current", "recent", "archived"};
        if (browse_kind == BROWSE_LIVE) snprintf(params, sizeof(params), "{\"representationVersion\":3,\"resource\":\"section\",\"section\":\"live\",\"offset\":%d,\"limit\":%d}", navigation_offset, NAV_PAGE_SIZE);
        else if (browse_kind == BROWSE_PROJECTS) snprintf(params, sizeof(params), "{\"representationVersion\":3,\"resource\":\"catalog\",\"catalog\":\"projects\",\"offset\":%d,\"limit\":%d}", navigation_offset, NAV_PAGE_SIZE);
        else {
            json_quote(browse_project, ref, sizeof(ref));
            snprintf(params, sizeof(params), "{\"representationVersion\":3,\"resource\":\"project_page\",\"catalog\":\"projects\",\"projectKey\":%s,\"tier\":\"%s\",\"offset\":%d,\"limit\":%d}", ref, tiers[browse_tier], navigation_offset, NAV_PAGE_SIZE);
        }
        if (request(REQUEST_NAV, "evener/navigation/read", params)) {
            requested_browse = browse_generation; requested_offset = navigation_offset;
            want_browse = false; browse_at = app_time() + 5;
        }
    }
}

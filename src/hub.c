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
bool hub_ready, hub_loading, hub_has_older, hub_has_newer, hub_hydrated;

enum Request { REQUEST_NONE, REQUEST_INIT, REQUEST_NAV, REQUEST_READ, REQUEST_OLDER, REQUEST_SEND, REQUEST_RESUME, REQUEST_PREPARE_SEND };
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
static bool reading, prefetch_paused, background_request, archive_valid, view_valid, archive_failed;
static HistoryCache archive, view_archive;
static HistoryCache repair_archive;
static int repair_page = INT32_MIN, repair_entry, repair_item, repair_sub;
static bool repair_older, reset_history;
static ChatMessage page_rows[HISTORY_PAGE_ITEMS], incoming_rows[HISTORY_PAGE_ITEMS];
static int page_count, incoming_count;
static bool collecting, incoming_failed;
static char outgoing[57344], quoted_text[DRAFT_CAPACITY * 6 + 3];
static char text_buffer[32768], reading_anchor[192];

static void history_edges(void);
static bool checkpoint_history(void);
static void yield_request(void);

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
        if (cache_save_chat() && checkpoint_history()) saved_revision = chat_revision;
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
static void free_rows(ChatMessage *rows, int count) {
    for (int i = 0; i < count; i++) { free(rows[i].text); rows[i].text = NULL; }
}
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
static bool same_position(const ChatMessage *a, const ChatMessage *b) { return compare_chat(a, b) == 0; }
static bool copy_row(ChatMessage *to, const ChatMessage *from) {
    const char *text = from->text ? from->text : "";
    char *copy = malloc(strlen(text) + 1);
    if (!copy) return false;
    strcpy(copy, text);
    free(to->text); *to = *from; to->text = copy;
    return true;
}
static bool set_text(ChatMessage *row, const char *value, bool clipped) {
    size_t length = strlen(value);
    char *copy = realloc(row->text, length + 1);
    if (!copy) { status("Not enough memory for this message"); return false; }
    row->text = copy;
    memcpy(copy, value, length + 1);
    row->clipped = clipped;
    if (!collecting) chat_revision++;
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
    ChatMessage *rows = collecting ? incoming_rows : chat;
    int *count = collecting ? &incoming_count : &chat_count;
    int capacity = collecting ? HISTORY_PAGE_ITEMS : CHAT_CAPACITY;
    int index = -1;
    for (int i = 0; i < *count; i++) if (same(rows[i].key, key)) { index = i; break; }
    int version = json_int(j, json_get(j, item, "version"));
    if (index >= 0 && version < rows[index].version) return;
    if (index < 0) {
        if (*count == capacity) {
            if (collecting) { incoming_failed = true; return; }
            int remove = backfill ? chat_count - 1 : 0;
            if (same(chat[remove].key, reading_anchor)) return;
            remove_chat(remove);
            if (!backfill) hub_has_older = rebase_older = true;
        }
        index = (*count)++;
        memset(&rows[index], 0, sizeof(rows[index]));
        snprintf(rows[index].key, sizeof(rows[index].key), "%s", key);
    }
    ChatMessage *row = &rows[index];
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
    if (!set_text(row, text_buffer, !complete) && collecting) incoming_failed = true;
    if (!collecting && !row->overlay && !user && row->round[0]) {
        char round[128]; snprintf(round, sizeof(round), "%s", row->round);
        clear_overlays(round, NULL);
    }
    qsort(rows, *count, sizeof(*rows), compare_chat);
}
static void chat_array(const Json *j, int array, bool backfill) {
    for (int i = json_first(j, array); i >= 0 && i < json_next(j, array); i = json_next(j, i))
        chat_item(j, i, -1, backfill);
}
static void turns(const Json *j, int array, bool backfill) {
    for (int i = json_first(j, array); i >= 0 && i < json_next(j, array); i = json_next(j, i))
        chat_array(j, json_get(j, i, "items"), backfill);
}
static bool load_page(const HistoryCache *history, int page) {
    free_rows(page_rows, page_count); page_count = 0;
    return cache_load_history_page(history, page, page_rows, &page_count);
}
static bool merge_chat(const ChatMessage *row, bool older, bool existing_only) {
    int index = -1;
    for (int i = 0; i < chat_count; i++) if (same(chat[i].key, row->key)) { index = i; break; }
    if (index >= 0 && row->version < chat[index].version) return true;
    if (index < 0) {
        if (existing_only) return true;
        if (chat_count == CHAT_CAPACITY) {
            int edge = older ? chat_count - 1 : 0;
            if (same(chat[edge].key, reading_anchor)) return false;
            remove_chat(edge);
        }
        index = chat_count;
        memset(&chat[index], 0, sizeof(chat[index]));
    }
    if (!copy_row(&chat[index], row)) { status("Not enough memory for this message"); return false; }
    if (index == chat_count) chat_count++;
    chat_revision++;
    if (!row->overlay && !row->user && row->round[0]) clear_overlays(row->round, NULL);
    qsort(chat, chat_count, sizeof(*chat), compare_chat);
    return true;
}
static int last_recorded(void) {
    int index = chat_count - 1;
    while (index >= 0 && chat[index].overlay) index--;
    return index;
}
static void history_edges(void) {
    int last = last_recorded();
    if (archive_valid && chat_count && last >= 0) {
        if (view_valid && view_archive.generation == archive.generation) view_archive = archive;
        else if (cache_history_position(&archive, &chat[0]) != INT32_MIN &&
            cache_history_position(&archive, &chat[last]) != INT32_MIN) {
            view_archive = archive; view_valid = true;
        }
    }
    hub_has_older = archive_valid ? !archive.complete : older_cursor[0] != 0;
    hub_has_newer = false;
    if (!view_valid || last < 0) return;
    int first_page = cache_history_position(&view_archive, &chat[0]);
    int last_page = cache_history_position(&view_archive, &chat[last]);
    ChatMessage boundary;
    if (first_page != INT32_MIN) {
        hub_has_older = first_page > view_archive.oldest || !view_archive.complete;
        if (!hub_has_older && cache_history_edge(&view_archive, first_page, false, &boundary))
            hub_has_older = !same_position(&chat[0], &boundary);
    }
    if (last_page != INT32_MIN) {
        hub_has_newer = last_page < view_archive.newest || (archive_valid && view_archive.generation != archive.generation);
        if (!hub_has_newer && cache_history_edge(&view_archive, last_page, true, &boundary))
            hub_has_newer = compare_chat(&chat[last], &boundary) < 0;
    }
    free_rows(page_rows, page_count); page_count = 0;
}
static bool checkpoint_history(void) {
    if (!archive_valid) return true;
    snprintf(archive.first_key, sizeof(archive.first_key), "%s", chat_count ? chat[0].key : "");
    snprintf(archive.last_key, sizeof(archive.last_key), "%s", chat_count ? chat[chat_count - 1].key : "");
    archive.view_generation = view_valid ? view_archive.generation : 0;
    archive.view_oldest = view_archive.oldest; archive.view_newest = view_archive.newest;
    return cache_save_history(&archive);
}
static bool begin_history(const char *snapshot, const char *cursor) {
    static HistoryCache next;
    unsigned generation = archive.generation + 1;
    memset(&next, 0, sizeof(next)); next.generation = generation ? generation : 1;
    next.previous_generation = archive.generation; next.cleanup_generation = archive.cleanup_generation;
    if (same(archive.incarnation, snapshot)) {
        snprintf(next.first_key, sizeof(next.first_key), "%s", archive.first_key);
        snprintf(next.last_key, sizeof(next.last_key), "%s", archive.last_key);
        next.view_generation = archive.view_generation;
        next.view_oldest = archive.view_oldest; next.view_newest = archive.view_newest;
    }
    snprintf(next.incarnation, sizeof(next.incarnation), "%s", snapshot);
    snprintf(next.cursor, sizeof(next.cursor), "%s", cursor);
    next.complete = !cursor[0];
    if (!cache_save_history_page(&next, 0, incoming_rows, incoming_count) || !cache_save_history(&next)) return false;
    archive = next; archive_valid = true; rebase_older = false;
    return true;
}
static bool append_history(const ChatMessage *rows, int count) {
    static HistoryCache next;
    next = archive;
    int loaded = INT32_MIN;
    bool dirty = false, ok = true;
    for (int i = 0; ok && i < count; i++) {
        int page = cache_history_position(&next, &rows[i]);
        if (page == INT32_MIN) page = next.newest;
        if (loaded != page) {
            if (dirty) ok = cache_save_history_page(&next, loaded, page_rows, page_count);
            if (!ok || !load_page(&next, page)) { ok = false; break; }
            loaded = page; dirty = false;
        }
        int index = -1;
        for (int j = 0; j < page_count; j++) if (same(page_rows[j].key, rows[i].key)) { index = j; break; }
        if (index >= 0 && rows[i].version < page_rows[index].version) continue;
        if (index < 0) {
            // A missing interior item means the cached sequence cannot prove continuity.
            if (page_count && compare_chat(&rows[i], &page_rows[page_count - 1]) <= 0) { ok = false; break; }
            if (page_count == HISTORY_PAGE_ITEMS) {
                if (dirty && !cache_save_history_page(&next, loaded, page_rows, page_count)) { ok = false; break; }
                free_rows(page_rows, page_count); page_count = 0;
                loaded = ++next.newest; dirty = false;
            }
            index = page_count;
            memset(&page_rows[index], 0, sizeof(page_rows[index]));
        }
        if (!copy_row(&page_rows[index], &rows[i])) { ok = false; break; }
        if (index == page_count) page_count++;
        dirty = true;
    }
    if (ok && dirty) ok = cache_save_history_page(&next, loaded, page_rows, page_count);
    free_rows(page_rows, page_count); page_count = 0;
    if (ok) ok = cache_save_history(&next);
    if (ok) archive = next;
    return ok;
}
static bool refresh_history(const char *snapshot, const char *cursor) {
    bool continuous = !reset_history && archive_valid && same(archive.incarnation, snapshot) && load_page(&archive, archive.newest);
    if (continuous && page_count) {
        continuous = false;
        for (int i = 0; i < incoming_count; i++)
            if (same(incoming_rows[i].key, page_rows[page_count - 1].key)) continuous = true;
    }
    free_rows(page_rows, page_count); page_count = 0;
    if (!continuous) {
        bool saved = begin_history(snapshot, cursor);
        if (saved) reset_history = false;
        return saved;
    }
    if (!append_history(incoming_rows, incoming_count)) return false;
    if (!archive.complete) {
        snprintf(archive.cursor, sizeof(archive.cursor), "%s", cursor);
        rebase_older = true;
    }
    return cache_save_history(&archive);
}
static bool backfill_history(const char *cursor) {
    static HistoryCache next;
    next = archive;
    if (incoming_count) {
        bool empty = next.oldest == next.newest && load_page(&next, next.oldest) && !page_count;
        free_rows(page_rows, page_count); page_count = 0;
        if (!empty) next.oldest--;
        if (!cache_save_history_page(&next, next.oldest, incoming_rows, incoming_count)) return false;
    }
    snprintf(next.cursor, sizeof(next.cursor), "%s", cursor); next.complete = !cursor[0];
    if (!cache_save_history(&next)) return false;
    archive = next; rebase_older = false;
    return true;
}
static void repair_history(int page, bool older) {
    ChatMessage boundary;
    if (page != INT32_MIN && page < view_archive.newest && cache_history_edge(&view_archive, page + 1, false, &boundary)) {
        repair_archive = view_archive; repair_page = page; repair_older = older;
        repair_entry = boundary.entry; repair_item = boundary.item; repair_sub = boundary.sub;
        want_older = true;
    } else { reset_history = true; want_read = true; }
    free_rows(page_rows, page_count); page_count = 0;
}
static bool move_history(bool older) {
    int last = last_recorded();
    if (!view_valid || last < 0) return false;
    int edge = older ? 0 : last;
    int page = cache_history_position(&view_archive, &chat[edge]);
    ChatMessage boundary;
    if (page == INT32_MIN || !cache_history_edge(&view_archive, page, !older, &boundary)) {
        repair_history(page, older); return false;
    }
    if (same_position(&chat[edge], &boundary)) page += older ? -1 : 1;
    if (page < view_archive.oldest || page > view_archive.newest) {
        free_rows(page_rows, page_count); page_count = 0; return false;
    }
    if (!load_page(&view_archive, page)) { repair_history(page, older); return false; }
    // The page overlaps the retained window; only scrolling may evict its far edge.
    clear_overlays(NULL, NULL);
    for (int i = 0; i < page_count; i++) merge_chat(&page_rows[i], older, false);
    free_rows(page_rows, page_count); page_count = 0;
    history_edges();
    return true;
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
    if (backfill && (!same(incoming, incarnation) ||
        (json_get(j, result, "bootGeneration") >= 0 && !json_is(j, json_get(j, result, "bootGeneration"), boot_generation)) ||
        (json_get(j, result, "epoch") >= 0 && json_int(j, json_get(j, result, "epoch")) != epoch))) {
        repair_page = INT32_MIN; want_read = true; return;
    }
    free_rows(incoming_rows, incoming_count); incoming_count = 0; incoming_failed = false;
    collecting = true;
    if (backfill) turns(j, json_get(j, result, "data"), true);
    else {
        turns(j, json_get(j, json_get(j, result, "thread"), "turns"), false);
        chat_array(j, json_get(j, json_get(j, result, "changes"), "items"), false);
    }
    collecting = false;
    if (incoming_failed) {
        free_rows(incoming_rows, incoming_count); incoming_count = 0;
        archive_failed = true; status("Could not load this history page"); return;
    }
    static char cursor[2048];
    bool cursor_complete = json_field(j, result, backfill ? "nextCursor" : "olderCursor", cursor, sizeof(cursor)) ||
        json_get(j, result, backfill ? "nextCursor" : "olderCursor") < 0;
    if (!backfill) {
        bool replace = replace_chat || !view_valid || (incarnation[0] && !same(incoming, incarnation));
        bool tail = !hub_has_newer;
        if (replace) clear_chat();
        replace_chat = false;
        snprintf(incarnation, sizeof(incarnation), "%s", incoming);
        json_field(j, result, "bootGeneration", boot_generation, sizeof(boot_generation));
        epoch = json_int(j, json_get(j, result, "epoch"));
        int thread = json_get(j, result, "thread");
        thread_metadata(j, thread);
        archive_failed = !cursor_complete || !incoming[0] || !refresh_history(incoming, cursor);
        if (archive_failed) {
            app_log("Could not save fresh history pages");
            if (replace || !same(archive.incarnation, incoming)) archive_valid = view_valid = false;
        }
        if (!archive_failed && replace) { view_archive = archive; view_valid = true; }
        for (int i = 0; i < incoming_count; i++) merge_chat(&incoming_rows[i], false, !replace && !tail);
        if (replace || tail) {
            clear_overlays(NULL, NULL);
            int overlay = json_get(j, result, "overlay");
            for (int i = json_first(j, overlay); i >= 0 && i < json_next(j, overlay); i = json_next(j, i))
                chat_item(j, json_get(j, i, "item"), i, false);
        }
        snprintf(older_cursor, sizeof(older_cursor), "%s", cursor);
        hub_hydrated = true;
        app_log("Hub read: %d chat messages; session %s", chat_count, session_status);
    } else {
        bool show = !background_request || want_older;
        if (repair_page != INT32_MIN) {
            archive_failed = !cache_save_history_page(&repair_archive, repair_page, incoming_rows, incoming_count);
            for (int i = 0; i < incoming_count; i++) merge_chat(&incoming_rows[i], repair_older, false);
            repair_page = INT32_MIN;
        } else {
            archive_failed = !cursor_complete || (cursor[0] && archive_valid && same(cursor, archive.cursor)) ||
                !archive_valid || !backfill_history(cursor);
            if (archive_failed) {
                app_log("History cache paused after an incomplete page commit");
                if (show) {
                    for (int i = 0; i < incoming_count; i++) merge_chat(&incoming_rows[i], true, false);
                    snprintf(older_cursor, sizeof(older_cursor), "%s", cursor);
                    archive_valid = view_valid = false;
                }
            } else {
                snprintf(older_cursor, sizeof(older_cursor), "%s", archive.cursor);
                history_edges();
                if (show) move_history(true);
            }
        }
        if (show) want_older = false;
    }
    free_rows(incoming_rows, incoming_count); incoming_count = 0;
    history_edges();
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
        bool tail = !hub_has_newer;
        int array = json_get(j, params, "items");
        for (int i = json_first(j, array); i >= 0 && i < json_next(j, array); i = json_next(j, i)) {
            incoming_count = 0; incoming_failed = false; collecting = true;
            chat_item(j, i, -1, false); collecting = false;
            if (incoming_count && !incoming_failed) {
                if (archive_valid && !archive_failed && !append_history(incoming_rows, incoming_count)) {
                    archive_failed = reset_history = true; want_read = true;
                }
                merge_chat(&incoming_rows[0], false, !tail);
            }
            free_rows(incoming_rows, incoming_count); incoming_count = 0;
        }
        history_edges();
    } else if (json_is(j, method, "overlay/upserted")) {
        if (hub_has_newer) return;
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
            if (completed == REQUEST_OLDER) { repair_page = INT32_MIN; want_read = true; archive_failed = true; }
            else want_send = false;
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
        background_request = false;
    }
    json_free(&j);
}
static bool request(enum Request kind, const char *method, const char *params) {
    if (pending || !ws_writable(&socket)) return false;
    unsigned id = ++serial;
    int n = snprintf(outgoing, sizeof(outgoing), "{\"id\":%u,\"method\":\"%s\",\"params\":%s}", id, method, params);
    if (n < 0 || n >= sizeof(outgoing) || !ws_send(&socket, outgoing)) return false;
    pending = kind; request_id = id; request_time = app_time(); hub_loading = !background_request;
    if (kind == REQUEST_READ) status("Loading conversation...");
    else if (kind == REQUEST_OLDER && !background_request) status("Loading older messages...");
    else if (kind == REQUEST_NAV) status("Loading sessions...");
    app_log("Hub request %u: %s", id, method);
    snprintf(request_ref, sizeof(request_ref), "%s", selected_ref);
    return true;
}
void hub_close(void) {
    save_chat();
    suspended = true;
    ws_close(&socket);
    hub_ready = hub_hydrated = initialized = send_prepared = false;
    pending = REQUEST_NONE; hub_loading = background_request = false; repair_page = INT32_MIN;
    if (draft.state == DRAFT_SENDING) { draft.state = DRAFT_UNCERTAIN; draft_save(); }
}
void hub_connect(void) {
    suspended = false;
    ws_connect(&socket, config.hub_host, config.hub_port, config.hub_tls, config.hub_path,
        config.hub_token, 2097152, message);
    hub_ready = hub_hydrated = initialized = replay_allowed = send_prepared = false;
    pending = REQUEST_NONE; background_request = false;
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
    hub_ready = hub_loading = hub_has_older = hub_has_newer = hub_hydrated = initialized = false;
    archive_valid = view_valid = archive_failed = reading = prefetch_paused = background_request = reset_history = false;
    repair_page = INT32_MIN;
    memset(&archive, 0, sizeof(archive)); memset(&view_archive, 0, sizeof(view_archive));
    free_rows(page_rows, page_count); page_count = 0;
    free_rows(incoming_rows, incoming_count); incoming_count = 0;
    pending = REQUEST_NONE; saved_revision = -1; retry_delay = 1; reconnect_at = browse_at = cache_at = 0;
}
void hub_browse(enum BrowseKind kind, const char *project, const char *title, int tier, int offset) {
    reading = false;
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
    hub_hydrated = hub_has_older = hub_has_newer = replay_allowed = send_prepared = false;
    archive_valid = view_valid = archive_failed = reset_history = false; reading = true;
    repair_page = INT32_MIN;
    memset(&archive, 0, sizeof(archive)); memset(&view_archive, 0, sizeof(view_archive));
    replace_chat = false;
    if (cache_load_chat()) { replace_chat = true; status("Saved conversation; updating..."); }
    archive_valid = cache_load_history(&archive);
    if (archive_valid && chat_count && archive.view_generation && same(archive.first_key, chat[0].key) &&
        same(archive.last_key, chat[chat_count - 1].key)) {
        view_archive = archive; view_archive.generation = archive.view_generation;
        if (archive.view_generation != archive.generation) {
            view_archive.oldest = archive.view_oldest; view_archive.newest = archive.view_newest;
        }
        view_valid = true; replace_chat = false;
        snprintf(incarnation, sizeof(incarnation), "%s", archive.incarnation);
        snprintf(older_cursor, sizeof(older_cursor), "%s", archive.cursor);
        history_edges();
    }
    want_read = true; want_older = want_send = false;
    yield_request();
    return true;
}
void hub_set_reading_anchor(const char *key) { snprintf(reading_anchor, sizeof(reading_anchor), "%s", key); }
static void yield_request(void) {
    if (pending == REQUEST_NAV || (pending == REQUEST_OLDER && (background_request || want_read || want_send)) ||
        ((pending == REQUEST_READ || pending == REQUEST_OLDER) && !same(request_ref, selected_ref))) {
        // IDs remain unique, so a late response cannot replace the selected read.
        if (pending == REQUEST_OLDER) repair_page = INT32_MIN;
        pending = REQUEST_NONE; hub_loading = background_request = false;
    }
}
void hub_set_reading(bool value) {
    reading = value;
    if ((reading && pending == REQUEST_NAV) || (!reading && background_request)) yield_request();
}
void hub_set_prefetch_paused(bool value) {
    prefetch_paused = value;
    if (value && background_request) yield_request();
}
bool hub_foreground_busy(void) {
    return hub_ready && socket.state == WS_OPEN && selected_ref[0] && (want_read || want_older || want_send || pending == REQUEST_READ ||
        pending == REQUEST_PREPARE_SEND || pending == REQUEST_SEND || pending == REQUEST_RESUME ||
        (pending == REQUEST_OLDER && !background_request));
}
bool hub_save_view(char *snapshot, size_t capacity) {
    if (strlen(incarnation) >= capacity) return false;
    if (selected_ref[0] && incarnation[0] && saved_revision != chat_revision) {
        if (!cache_save_chat() || !checkpoint_history()) return false;
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
void hub_older(void) {
    if (!hub_has_older) return;
    if (!move_history(true)) { want_older = true; archive_failed = false; }
}
void hub_newer(void) {
    if (hub_has_newer && !move_history(false)) status("Loading newer messages...");
}
void hub_latest(void) {
    save_chat();
    if (archive_valid && load_page(&archive, archive.newest) && page_count) {
        clear_chat();
        for (int i = 0; i < page_count; i++) merge_chat(&page_rows[i], false, false);
        for (int page = archive.newest - 1; chat_count < HISTORY_PAGE_ITEMS && page >= archive.oldest; page--) {
            if (!load_page(&archive, page)) break;
            for (int i = 0; i < page_count; i++) merge_chat(&page_rows[i], true, false);
        }
        while (chat_count > HISTORY_PAGE_ITEMS) remove_chat(0);
        free_rows(page_rows, page_count); page_count = 0;
        view_archive = archive; view_valid = true; history_edges();
    }
    replace_chat = true; want_read = true; want_older = false;
    yield_request();
}
void hub_recheck(void) { want_read = selected_ref[0] != 0; if (!hub_ready) reconnect_at = 0; }
bool hub_send_pending(void) { return want_send; }
void hub_send(void) {
    if (draft.state == DRAFT_READY && draft.text[0] && !want_send) {
        want_send = true; send_prepared = false; status("Preparing to send...");
        yield_request();
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
    if (want_send || want_read) yield_request();
    if (archive_valid && !prefetch_paused && !hub_foreground_busy() && voice_state == VOICE_OFF)
        cache_reclaim_history(&archive, view_valid ? view_archive.generation : 0);
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
    } else if ((want_older && hub_has_older) || (reading && !prefetch_paused && hub_hydrated &&
        archive_valid && !archive_failed && !archive.complete && voice_state != VOICE_RECORDING &&
        voice_state != VOICE_FINISHING && voice_state != VOICE_CONNECTING && voice_state != VOICE_CONFIGURING)) {
        static char cursor[12291];
        const char *history_cursor = older_cursor;
        if (repair_page == INT32_MIN && archive_valid) history_cursor = archive.cursor;
        json_quote(history_cursor, cursor, sizeof(cursor));
        ChatMessage boundary = {0};
        bool before = false;
        if (repair_page != INT32_MIN) {
            boundary.entry = repair_entry; boundary.item = repair_item; boundary.sub = repair_sub; before = true;
        } else if (rebase_older && archive_valid && cache_history_edge(&archive, archive.oldest, false, &boundary)) {
            before = true;
        } else if (rebase_older && !archive_valid && chat_count && !chat[0].overlay) {
            boundary = chat[0]; boundary.text = NULL; before = true;
        }
        free_rows(page_rows, page_count); page_count = 0;
        if (before)
            snprintf(params, sizeof(params), "{\"ref\":%s,\"cursor\":%s,\"before\":{\"entry\":%d,\"item\":%d,\"sub\":%d},\"itemLimit\":%d,%s}", ref, cursor, boundary.entry, boundary.item, boundary.sub, HISTORY_PAGE_ITEMS, history_content);
        else snprintf(params, sizeof(params), "{\"ref\":%s,\"cursor\":%s,\"itemLimit\":%d,%s}", ref, cursor, HISTORY_PAGE_ITEMS, history_content);
        background_request = !want_older;
        if (request(REQUEST_OLDER, "thread/turns/list", params)) want_older = false;
        else background_request = false;
    } else if (!reading && !prefetch_paused && want_browse && app_time() >= browse_at) {
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

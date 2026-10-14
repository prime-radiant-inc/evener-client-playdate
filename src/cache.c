#include "cache.h"
#include "app.h"
#include "hub.h"
#include "settings.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint32_t version, row_size, count, remaining;
    char key[1280], status[32];
} Header;
typedef struct { SDFile *file; uint32_t checksum; bool writing, ok; } Stream;

static uint32_t checksum(uint32_t sum, const void *bytes, size_t length) {
    const unsigned char *p = bytes;
    for (size_t i = 0; i < length; i++) sum = (sum ^ p[i]) * 16777619u;
    return sum;
}
static void key(Header *header, bool navigation_page) {
    memset(header, 0, sizeof(*header));
    header->version = 1;
    if (navigation_page)
        snprintf(header->key, sizeof(header->key), "%s:%d%s/nav/%d/%s/%d/%d", config.hub_host, config.hub_port,
            config.hub_path, browse_kind, browse_kind == BROWSE_PROJECT ? browse_project : "", browse_tier, navigation_offset);
    else snprintf(header->key, sizeof(header->key), "%s:%d%s/chat/%s", config.hub_host, config.hub_port, config.hub_path, selected_ref);
}
static void path(const Header *header, char *value, size_t capacity, bool temporary) {
    char leaf[40];
    snprintf(leaf, sizeof(leaf), "cache-%08lx.bin%s", (unsigned long)checksum(2166136261u, header->key, strlen(header->key)), temporary ? ".tmp" : "");
    settings_path(value, capacity, leaf);
}
static Stream open_cache(const Header *header, bool writing) {
    char filename[64]; path(header, filename, sizeof(filename), writing);
    SDFile *file = pd->file->open(filename, writing ? kFileWrite : kFileReadData);
    if (!file && !writing) {
        path(header, filename, sizeof(filename), true);
        file = pd->file->open(filename, kFileReadData);
    }
    return (Stream){ .file = file, .checksum = 2166136261u, .writing = writing, .ok = file != NULL };
}
static void transfer(Stream *stream, void *bytes, size_t length) {
    if (!stream->ok) return;
    int n = stream->writing ? pd->file->write(stream->file, bytes, length) : pd->file->read(stream->file, bytes, length);
    stream->ok = n == length;
    if (stream->ok) stream->checksum = checksum(stream->checksum, bytes, length);
}
static bool finish(Stream *stream, const Header *header) {
    if (!stream->file) return false;
    uint32_t saved = stream->checksum;
    if (stream->writing) {
        if (stream->ok) stream->ok = pd->file->write(stream->file, &saved, sizeof(saved)) == sizeof(saved) && pd->file->flush(stream->file) >= 0;
    } else if (stream->ok) {
        stream->ok = pd->file->read(stream->file, &saved, sizeof(saved)) == sizeof(saved) && saved == stream->checksum;
    }
    if (pd->file->close(stream->file) != 0) stream->ok = false;
    if (stream->writing && stream->ok) {
        char from[64], to[64]; path(header, from, sizeof(from), true); path(header, to, sizeof(to), false);
        stream->ok = storage_replace(from, to);
    }
    return stream->ok;
}
static bool read_header(Stream *stream, Header *header, const Header *expected, unsigned row_size, unsigned limit) {
    transfer(stream, header, sizeof(*header));
    stream->ok = stream->ok && header->version == 1 && header->row_size == row_size && header->count <= limit &&
        memchr(header->key, 0, sizeof(header->key)) && !strcmp(header->key, expected->key) && memchr(header->status, 0, sizeof(header->status));
    return stream->ok;
}
bool cache_save_navigation(void) {
    static Header header; key(&header, true);
    header.row_size = sizeof(NavigationRow); header.count = navigation_count; header.remaining = navigation_remaining;
    Stream stream = open_cache(&header, true);
    transfer(&stream, &header, sizeof(header));
    transfer(&stream, navigation, navigation_count * sizeof(*navigation));
    return finish(&stream, &header);
}
bool cache_load_navigation(void) {
    static Header expected, header; key(&expected, true);
    Stream stream = open_cache(&expected, false);
    static NavigationRow rows[NAV_PAGE_SIZE];
    if (read_header(&stream, &header, &expected, sizeof(NavigationRow), NAV_PAGE_SIZE)) {
        transfer(&stream, rows, header.count * sizeof(*rows));
        for (unsigned i = 0; stream.ok && i < header.count; i++)
            stream.ok = memchr(rows[i].ref, 0, sizeof(rows[i].ref)) && memchr(rows[i].title, 0, sizeof(rows[i].title)) && memchr(rows[i].state, 0, sizeof(rows[i].state));
        if (header.remaining > INT32_MAX) stream.ok = false;
    }
    if (!finish(&stream, &expected)) return false;
    memcpy(navigation, rows, header.count * sizeof(*rows));
    navigation_count = header.count; navigation_remaining = header.remaining; navigation_revision++;
    return true;
}
bool cache_save_chat(void) {
    static Header header; key(&header, false);
    header.row_size = sizeof(ChatMessage); header.count = chat_count;
    snprintf(header.status, sizeof(header.status), "%s", session_status);
    Stream stream = open_cache(&header, true);
    transfer(&stream, &header, sizeof(header));
    for (int i = 0; stream.ok && i < chat_count; i++) {
        ChatMessage row = chat[i]; row.text = NULL;
        uint32_t length = chat[i].text ? strlen(chat[i].text) : 0;
        transfer(&stream, &row, sizeof(row)); transfer(&stream, &length, sizeof(length));
        if (length) transfer(&stream, chat[i].text, length);
    }
    return finish(&stream, &header);
}
bool cache_load_chat(void) {
    static Header expected, header; key(&expected, false);
    Stream stream = open_cache(&expected, false);
    static ChatMessage rows[CHAT_CAPACITY];
    memset(rows, 0, sizeof(rows));
    if (read_header(&stream, &header, &expected, sizeof(ChatMessage), CHAT_CAPACITY)) {
        for (unsigned i = 0; stream.ok && i < header.count; i++) {
            uint32_t length = 0;
            transfer(&stream, &rows[i], sizeof(rows[i])); rows[i].text = NULL;
            transfer(&stream, &length, sizeof(length));
            stream.ok = stream.ok && length < 32768 && memchr(rows[i].key, 0, sizeof(rows[i].key)) &&
                memchr(rows[i].round, 0, sizeof(rows[i].round)) && memchr(rows[i].stream, 0, sizeof(rows[i].stream));
            if (!stream.ok) break;
            rows[i].text = malloc(length + 1);
            if (!rows[i].text) { stream.ok = false; break; }
            transfer(&stream, rows[i].text, length); rows[i].text[length] = 0;
        }
    }
    if (!finish(&stream, &expected)) {
        for (unsigned i = 0; i < CHAT_CAPACITY; i++) free(rows[i].text);
        return false;
    }
    for (int i = 0; i < chat_count; i++) free(chat[i].text);
    memcpy(chat, rows, header.count * sizeof(*rows)); chat_count = header.count; chat_revision++;
    snprintf(session_status, sizeof(session_status), "%s", header.status);
    return true;
}

typedef struct { int entry, item, sub; } Position;
typedef struct { Position first, last; uint32_t checksum; } PageBounds;

static Position position(const ChatMessage *row) { return (Position){row->entry, row->item, row->sub}; }
static int compare_position(Position a, Position b) {
    if (a.entry != b.entry) return a.entry < b.entry ? -1 : 1;
    if (a.item != b.item) return a.item < b.item ? -1 : 1;
    return (a.sub > b.sub) - (a.sub < b.sub);
}
static void history_key(Header *header, const HistoryCache *history, int page) {
    key(header, false);
    size_t used = strlen(header->key);
    if (history && page == INT32_MIN)
        snprintf(header->key + used, sizeof(header->key) - used, "/history/%u/index", history->generation);
    else if (history)
        snprintf(header->key + used, sizeof(header->key) - used, "/history/%u/%d", history->generation, page);
    else snprintf(header->key + used, sizeof(header->key) - used, "/history");
}
static Stream open_history(const Header *header, bool temporary) {
    char filename[64]; path(header, filename, sizeof(filename), temporary);
    SDFile *file = pd->file->open(filename, kFileReadData);
    return (Stream){.file = file, .checksum = 2166136261u, .ok = file != NULL};
}
static bool save_history(const HistoryCache *history, bool descriptor) {
    static Header header; history_key(&header, descriptor ? history : NULL, INT32_MIN);
    header.row_size = sizeof(*history); header.count = 1;
    Stream stream = open_cache(&header, true);
    transfer(&stream, &header, sizeof(header));
    transfer(&stream, (void *)history, sizeof(*history));
    return finish(&stream, &header);
}
bool cache_save_history(const HistoryCache *history) {
    return save_history(history, true) && save_history(history, false);
}
static bool load_history(HistoryCache *history, unsigned generation) {
    static Header expected, header;
    static HistoryCache loaded;
    loaded.generation = generation;
    history_key(&expected, generation ? &loaded : NULL, INT32_MIN);
    for (int temporary = 1; temporary >= 0; temporary--) {
        Stream stream = open_history(&expected, temporary);
        if (read_header(&stream, &header, &expected, sizeof(loaded), 1)) transfer(&stream, &loaded, sizeof(loaded));
        stream.ok = stream.ok && header.count == 1 && loaded.generation && (!generation || loaded.generation == generation) &&
            loaded.previous_generation < loaded.generation && loaded.oldest <= loaded.newest &&
            loaded.oldest > INT32_MIN / 2 && loaded.newest < INT32_MAX / 2 &&
            memchr(loaded.incarnation, 0, sizeof(loaded.incarnation)) && loaded.incarnation[0] &&
            memchr(loaded.cursor, 0, sizeof(loaded.cursor)) && memchr(loaded.first_key, 0, sizeof(loaded.first_key)) &&
            memchr(loaded.last_key, 0, sizeof(loaded.last_key));
        if (finish(&stream, &expected)) { *history = loaded; return true; }
    }
    return false;
}
bool cache_load_history(HistoryCache *history) { return load_history(history, 0); }
static bool page_bounds(Stream *stream, const Header *expected, Header *header, PageBounds *bounds) {
    if (!read_header(stream, header, expected, sizeof(ChatMessage), HISTORY_PAGE_ITEMS)) return false;
    uint32_t sum = stream->checksum;
    transfer(stream, bounds, sizeof(*bounds));
    stream->ok = stream->ok && bounds->checksum == checksum(sum, bounds, offsetof(PageBounds, checksum)) &&
        compare_position(bounds->first, bounds->last) <= 0;
    return stream->ok;
}
bool cache_save_history_page(const HistoryCache *history, int page, const ChatMessage *rows, int count) {
    if (count < 0 || count > HISTORY_PAGE_ITEMS) return false;
    static Header header; history_key(&header, history, page);
    header.row_size = sizeof(ChatMessage); header.count = count;
    Stream stream = open_cache(&header, true);
    transfer(&stream, &header, sizeof(header));
    PageBounds bounds = {0};
    if (count) { bounds.first = position(&rows[0]); bounds.last = position(&rows[count - 1]); }
    bounds.checksum = checksum(stream.checksum, &bounds, offsetof(PageBounds, checksum));
    transfer(&stream, &bounds, sizeof(bounds));
    for (int i = 0; stream.ok && i < count; i++) {
        ChatMessage row = rows[i]; row.text = NULL;
        uint32_t length = rows[i].text ? strlen(rows[i].text) : 0;
        transfer(&stream, &row, sizeof(row)); transfer(&stream, &length, sizeof(length));
        if (length) transfer(&stream, rows[i].text, length);
    }
    return finish(&stream, &header);
}
bool cache_load_history_page(const HistoryCache *history, int page, ChatMessage *rows, int *count) {
    static Header expected, header; history_key(&expected, history, page);
    for (int temporary = 1; temporary >= 0; temporary--) {
        memset(rows, 0, HISTORY_PAGE_ITEMS * sizeof(*rows));
        Stream stream = open_history(&expected, temporary);
        PageBounds bounds;
        if (page_bounds(&stream, &expected, &header, &bounds)) {
            for (unsigned i = 0; stream.ok && i < header.count; i++) {
                uint32_t length = 0;
                transfer(&stream, &rows[i], sizeof(rows[i])); rows[i].text = NULL;
                transfer(&stream, &length, sizeof(length));
                stream.ok = stream.ok && length < 32768 && memchr(rows[i].key, 0, sizeof(rows[i].key)) &&
                    memchr(rows[i].round, 0, sizeof(rows[i].round)) && memchr(rows[i].stream, 0, sizeof(rows[i].stream)) &&
                    !rows[i].overlay && (!i || compare_position(position(&rows[i - 1]), position(&rows[i])) <= 0);
                if (!stream.ok) break;
                rows[i].text = malloc(length + 1);
                if (!rows[i].text) { stream.ok = false; break; }
                transfer(&stream, rows[i].text, length); rows[i].text[length] = 0;
            }
            if (stream.ok && header.count)
                stream.ok = !compare_position(bounds.first, position(&rows[0])) &&
                    !compare_position(bounds.last, position(&rows[header.count - 1]));
        }
        if (finish(&stream, &expected)) { *count = header.count; return true; }
        for (unsigned i = 0; i < HISTORY_PAGE_ITEMS; i++) { free(rows[i].text); rows[i].text = NULL; }
    }
    *count = 0;
    return false;
}
static bool read_page_bounds(const HistoryCache *history, int page, PageBounds *bounds) {
    static Header expected, header;
    history_key(&expected, history, page);
    for (int temporary = 1; temporary >= 0; temporary--) {
        Stream stream = open_history(&expected, temporary);
        bool valid = page_bounds(&stream, &expected, &header, bounds) && header.count;
        if (stream.file && pd->file->close(stream.file) != 0) valid = false;
        if (valid) return true;
    }
    return false;
}
bool cache_history_edge(const HistoryCache *history, int page, bool newest, ChatMessage *row) {
    PageBounds bounds;
    if (!read_page_bounds(history, page, &bounds)) return false;
    Position edge = newest ? bounds.last : bounds.first;
    memset(row, 0, sizeof(*row)); row->entry = edge.entry; row->item = edge.item; row->sub = edge.sub;
    return true;
}
int cache_history_position(const HistoryCache *history, const ChatMessage *row) {
    int low = history->oldest, high = history->newest;
    Position target = position(row);
    while (low <= high) {
        int page = low + (high - low) / 2;
        PageBounds bounds;
        if (!read_page_bounds(history, page, &bounds)) return INT32_MIN;
        if (compare_position(target, bounds.first) < 0) high = page - 1;
        else if (compare_position(target, bounds.last) > 0) low = page + 1;
        else return page;
    }
    return INT32_MIN;
}
/* A filename hash is only a locator. Verify ownership before removing either copy. */
static int remove_history_file(const Header *expected, unsigned row_size, unsigned limit, bool temporary) {
    char filename[64]; path(expected, filename, sizeof(filename), temporary);
    FileStat info;
    if (pd->file->stat(filename, &info) != 0) return 0;
    Stream stream = open_history(expected, temporary);
    static Header header;
    bool ours = read_header(&stream, &header, expected, row_size, limit);
    if (stream.file && pd->file->close(stream.file) != 0) ours = false;
    if (!ours || pd->file->unlink(filename, 0) != 0) return -1;
    return 1;
}
void cache_reclaim_history(HistoryCache *history, unsigned visible_generation) {
    static Header expected;
    static HistoryCache retired, next;
    if (history->cleanup_generation) {
        retired.generation = history->cleanup_generation;
        if (retired.generation == history->generation || retired.generation == visible_generation ||
            retired.generation == history->view_generation) return;
        history_key(&expected, &retired, INT32_MIN);
        int removed = remove_history_file(&expected, sizeof(retired), 1, true);
        if (!removed) removed = remove_history_file(&expected, sizeof(retired), 1, false);
        if (removed) return;
        next = *history; next.cleanup_generation = 0;
    } else {
        unsigned generation = history->previous_generation;
        if (!generation || generation == visible_generation || generation == history->view_generation ||
            !load_history(&retired, generation)) return;
        history_key(&expected, &retired, retired.oldest);
        int removed = remove_history_file(&expected, sizeof(ChatMessage), HISTORY_PAGE_ITEMS, true);
        if (!removed) removed = remove_history_file(&expected, sizeof(ChatMessage), HISTORY_PAGE_ITEMS, false);
        if (removed) return;
        if (retired.oldest < retired.newest) { retired.oldest++; save_history(&retired, true); return; }
        next = *history; next.previous_generation = retired.previous_generation;
        next.cleanup_generation = retired.generation;
    }
    if (cache_save_history(&next)) *history = next;
}

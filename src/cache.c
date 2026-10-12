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

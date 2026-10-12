#include "app.h"
#include "recording.h"
#include "storage.h"
#include "settings.h"
#include <stdio.h>
#include <string.h>

typedef struct {
    uint32_t version;
    char ref[REF_CAPACITY];
    uint32_t checksum;
} RecordingHeader;
static RecordingHeader header;
static char path[64];
static SDFile *writer, *reader;
static size_t length, header_written, reader_length;
static bool dirty, creating;

static uint32_t checksum(const void *bytes, size_t count) {
    const uint8_t *p = bytes;
    uint32_t sum = 2166136261u;
    for (size_t i = 0; i < count; i++) sum = (sum ^ p[i]) * 16777619u;
    return sum;
}
static void filename(char *out, size_t capacity, const char *ref) {
    char leaf[32]; snprintf(leaf, sizeof(leaf), "voice-%08lx.pcmu", (unsigned long)checksum(ref, strlen(ref)));
    settings_path(out, capacity, leaf);
}
bool recording_exists(const char *ref) {
    if (!ref[0]) return false;
    char target[64]; filename(target, sizeof(target), ref);
    FileStat metadata;
    // Damaged recordings still own saved input until explicitly discarded.
    return pd->file->stat(target, &metadata) == 0;
}
void recording_close(void) {
    if (writer) pd->file->close(writer);
    if (reader) pd->file->close(reader);
    writer = reader = NULL; reader_length = 0;
}
bool recording_begin(const char *ref) {
    if (!ref[0] || strlen(ref) >= sizeof(header.ref)) return false;
    char target[64]; filename(target, sizeof(target), ref);
    FileStat metadata;
    // A saved recording owns this session's unfinished dictation until review
    // succeeds or the person explicitly discards it.
    if (pd->file->stat(target, &metadata) == 0) return false;
    recording_close();
    snprintf(path, sizeof(path), "%s", target);
    memset(&header, 0, sizeof(header));
    header.version = 1;
    snprintf(header.ref, sizeof(header.ref), "%s", ref);
    header.checksum = checksum(&header, offsetof(RecordingHeader, checksum));
    length = header_written = 0;
    dirty = false; creating = true;
    return true;
}
enum RecordingLoad recording_load(const char *ref) {
    recording_close();
    filename(path, sizeof(path), ref);
    FileStat metadata;
    length = 0; dirty = creating = false;
    if (pd->file->stat(path, &metadata) != 0) return RECORDING_MISSING;
    reader = pd->file->open(path, kFileReadData);
    if (!reader) return RECORDING_DAMAGED;
    int n = pd->file->read(reader, &header, sizeof(header));
    if (n != sizeof(header) || header.version != 1 ||
        !memchr(header.ref, 0, sizeof(header.ref)) || strcmp(header.ref, ref) ||
        header.checksum != checksum(&header, offsetof(RecordingHeader, checksum)) ||
        metadata.size < sizeof(header) || metadata.size - sizeof(header) > RECORDING_LIMIT) {
        recording_close(); return RECORDING_DAMAGED;
    }
    length = metadata.size - sizeof(header); reader_length = length;
    header_written = sizeof(header);
    return RECORDING_PRESENT;
}
bool recording_flush(void) {
    if (creating && !writer) {
        writer = pd->file->open(path, kFileWrite);
        if (!writer) return false;
    }
    if (creating && header_written < sizeof(header)) {
        int n = pd->file->write(writer, (const uint8_t *)&header + header_written, sizeof(header) - header_written);
        if (n <= 0) return false;
        header_written += (size_t)n; dirty = true;
        if (header_written < sizeof(header)) return false;
    }
    if (dirty && pd->file->flush(writer) < 0) return false;
    dirty = false; creating = false;
    return true;
}
bool recording_append(const uint8_t *bytes, size_t count, size_t *accepted) {
    *accepted = 0;
    if (count > RECORDING_CHUNK || count > RECORDING_LIMIT - length || !recording_flush() || !writer) return false;
    int n = pd->file->write(writer, bytes, count);
    if (n <= 0) return false;
    *accepted = (size_t)n; length += (size_t)n; dirty = true;
    return recording_flush() && (size_t)n == count;
}
size_t recording_length(void) { return length; }
int recording_read(size_t offset, uint8_t *bytes, size_t capacity) {
    if (dirty || creating || offset > length) return -1;
    if (capacity > length - offset) capacity = length - offset;
    if (!capacity) return 0;
    // The SDK reader caches file size at open. Keep that snapshot while it has
    // bytes left, then reopen to include audio flushed by the growing writer.
    if (reader && offset >= reader_length && reader_length < length) {
        pd->file->close(reader); reader = NULL;
    }
    if (!reader) {
        reader = pd->file->open(path, kFileReadData);
        if (!reader) return -1;
        reader_length = length;
    }
    if (capacity > reader_length - offset) capacity = reader_length - offset;
    if (pd->file->seek(reader, (int)(sizeof(header) + offset), SEEK_SET) != 0) return -1;
    int n = pd->file->read(reader, bytes, capacity);
    if (n == 0) {
        pd->file->close(reader); reader = NULL; reader_length = 0;
        return -1;
    }
    return n;
}
bool recording_discard(const char *ref) {
    char target[64]; filename(target, sizeof(target), ref);
    if (!strcmp(target, path)) recording_close();
    FileStat metadata;
    if (pd->file->stat(target, &metadata) != 0) return true;
    return pd->file->unlink(target, 0) == 0;
}

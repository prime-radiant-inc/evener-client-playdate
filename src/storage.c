#include "app.h"
#include "storage.h"
#include "json.h"
#include "settings.h"
#include <stdio.h>
#include <string.h>

Draft draft;
static uint32_t checksum(const void *bytes, size_t length) {
    const unsigned char *p = bytes;
    uint32_t sum = 2166136261u;
    for (size_t i = 0; i < length; i++) sum = (sum ^ p[i]) * 16777619u;
    return sum;
}
static void filename(char *out, size_t capacity, const char *ref) {
    char leaf[32]; snprintf(leaf, sizeof(leaf), "draft-%08lx.bin", (unsigned long)checksum(ref, strlen(ref)));
    settings_path(out, capacity, leaf);
}
static int read_draft(const char *path, const char *ref, Draft *loaded) {
    FileStat metadata;
    if (pd->file->stat(path, &metadata) != 0) return 0;
    SDFile *file = pd->file->open(path, kFileReadData);
    if (file) {
        uint32_t saved = 0;
        int n = pd->file->read(file, loaded, sizeof(*loaded));
        int m = pd->file->read(file, &saved, sizeof(saved));
        pd->file->close(file);
        if (n == sizeof(*loaded) && m == sizeof(saved) && loaded->version == 1 &&
            saved == checksum(loaded, sizeof(*loaded)) &&
            memchr(loaded->ref, 0, sizeof(loaded->ref)) && !strcmp(loaded->ref, ref) &&
            memchr(loaded->text, 0, sizeof(loaded->text)) &&
            memchr(loaded->mutation, 0, sizeof(loaded->mutation)) &&
            memchr(loaded->instance, 0, sizeof(loaded->instance)) &&
            memchr(loaded->method, 0, sizeof(loaded->method)) &&
            loaded->state >= DRAFT_EMPTY && loaded->state <= DRAFT_UNCERTAIN) return 1;
    }
    return -1;
}
bool storage_replace(const char *from, const char *to) {
    FileStat metadata;
    // Device rename can reject an existing destination. The complete temporary
    // record remains recoverable across the interval without a primary file.
    if (pd->file->stat(to, &metadata) == 0 && pd->file->unlink(to, 0) != 0) return false;
    return pd->file->rename(from, to) == 0;
}
bool draft_load(const char *ref) {
    static Draft loaded;
    char path[64], temporary[72]; filename(path, sizeof(path), ref);
    snprintf(temporary, sizeof(temporary), "%s.tmp", path);
    int staged = read_draft(temporary, ref, &loaded);
    if (staged == 1) {
        if (!storage_replace(temporary, path)) return false;
    } else {
        int saved = read_draft(path, ref, &loaded);
        if (saved < 0 || (!saved && staged < 0)) return false;
        if (!saved) memset(&loaded, 0, sizeof(loaded));
    }
    loaded.version = 1;
    snprintf(loaded.ref, sizeof(loaded.ref), "%s", ref);
    if (loaded.state == DRAFT_SENDING) loaded.state = DRAFT_UNCERTAIN;
    draft = loaded;
    return true;
}
bool draft_save(void) {
    if (!draft.ref[0]) return false;
    char path[64], temporary[72]; filename(path, sizeof(path), draft.ref);
    snprintf(temporary, sizeof(temporary), "%s.tmp", path);
    static Draft staged;
    if (read_draft(temporary, draft.ref, &staged) == 1 && !storage_replace(temporary, path)) return false;
    SDFile *file = pd->file->open(temporary, kFileWrite);
    if (!file) return false;
    uint32_t sum = checksum(&draft, sizeof(draft));
    bool ok = pd->file->write(file, &draft, sizeof(draft)) == sizeof(draft) &&
        pd->file->write(file, &sum, sizeof(sum)) == sizeof(sum) && pd->file->flush(file) >= 0;
    int closed = pd->file->close(file);
    if (closed != 0) ok = false;
    if (!ok) return false;
    if (storage_replace(temporary, path)) return true;
    app_log("Draft commit failed: %s", pd->file->geterr());
    return false;
}
bool draft_set(const char *text, bool complete) {
    if (draft.state == DRAFT_SENDING || draft.state == DRAFT_UNCERTAIN) return false;
    utf8_copy(draft.text, sizeof(draft.text), text, strlen(text));
    draft.state = draft.text[0] ? (complete ? DRAFT_READY : DRAFT_PARTIAL) : DRAFT_EMPTY;
    return draft_save();
}
bool draft_clear(void) {
    static Draft previous;
    previous = draft;
    memset(draft.text, 0, sizeof(draft.text));
    draft.state = DRAFT_EMPTY;
    draft.mutation[0] = draft.instance[0] = draft.method[0] = 0;
    if (draft_save()) return true;
    draft = previous;
    return false;
}

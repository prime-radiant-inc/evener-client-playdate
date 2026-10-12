#include "json.h"
#define JSMN_STRICT
#include "jsmn.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

bool json_parse(Json *j, const char *text, size_t length) {
    memset(j, 0, sizeof(*j));
    j->text = text;
    for (unsigned count = 512; count <= 32768; count *= 2) {
        void *tokens = realloc(j->tokens, count * sizeof(jsmntok_t));
        if (!tokens) break;
        j->tokens = tokens;
        jsmn_parser parser;
        jsmn_init(&parser);
        j->count = jsmn_parse(&parser, text, length, j->tokens, count);
        if (j->count > 0 && json_next(j, 0) == j->count) return true;
        if (j->count != JSMN_ERROR_NOMEM) break;
    }
    json_free(j);
    return false;
}

void json_free(Json *j) { free(j->tokens); memset(j, 0, sizeof(*j)); }
static jsmntok_t *token(const Json *j, int i) {
    return i >= 0 && i < j->count ? &((jsmntok_t *)j->tokens)[i] : NULL;
}
int json_next(const Json *j, int i) {
    jsmntok_t *t = token(j, i);
    if (!t) return j->count;
    int end = t->end;
    while (++i < j->count && token(j, i)->start < end) {}
    return i;
}
bool json_is(const Json *j, int i, const char *value) {
    jsmntok_t *t = token(j, i);
    return t && (t->type == JSMN_STRING || t->type == JSMN_PRIMITIVE) &&
        strlen(value) == (size_t)(t->end - t->start) &&
        memcmp(j->text + t->start, value, t->end - t->start) == 0;
}
int json_get(const Json *j, int object, const char *key) {
    jsmntok_t *t = token(j, object);
    if (!t || t->type != JSMN_OBJECT) return -1;
    for (int i = object + 1; i < j->count && token(j, i)->start < t->end;) {
        if (json_is(j, i, key)) return i + 1;
        i = json_next(j, i + 1);
    }
    return -1;
}
int json_first(const Json *j, int i) {
    jsmntok_t *t = token(j, i);
    return t && t->type == JSMN_ARRAY && t->size ? i + 1 : -1;
}
int json_int(const Json *j, int i) {
    jsmntok_t *t = token(j, i);
    return t && t->type == JSMN_PRIMITIVE ? atoi(j->text + t->start) : 0;
}
size_t utf8_copy(char *out, size_t capacity, const char *text, size_t length) {
    if (!capacity) return 0;
    size_t n = length < capacity - 1 ? length : capacity - 1;
    if (n < length) while (n && ((uint8_t)text[n] & 0xc0) == 0x80) n--;
    memcpy(out, text, n);
    out[n] = 0;
    return n;
}
static unsigned hex4(const char *p) {
    unsigned n = 0;
    for (int i = 0; i < 4; i++) {
        char c = p[i];
        n = n * 16 + (c >= '0' && c <= '9' ? c - '0' : (c | 32) - 'a' + 10);
    }
    return n;
}
bool json_text(const Json *j, int i, char *out, size_t capacity) {
    if (!capacity) return false;
    out[0] = 0;
    jsmntok_t *t = token(j, i);
    if (!t || t->type != JSMN_STRING) return false;
    size_t used = 0;
    for (int p = t->start; p < t->end;) {
        char bytes[4];
        size_t n = 1;
        unsigned char c = j->text[p++];
        bytes[0] = (char)c;
        if (c == '\\') {
            c = j->text[p++];
            if (c == 'u') {
                unsigned cp = hex4(j->text + p); p += 4;
                if (cp >= 0xd800 && cp <= 0xdbff && p + 6 <= t->end &&
                    j->text[p] == '\\' && j->text[p + 1] == 'u') {
                    unsigned low = hex4(j->text + p + 2);
                    if (low >= 0xdc00 && low <= 0xdfff) {
                        cp = 0x10000 + ((cp - 0xd800) << 10) + low - 0xdc00;
                        p += 6;
                    }
                }
                if (cp >= 0xd800 && cp <= 0xdfff) cp = 0xfffd;
                if (!cp) cp = 0xfffd;
                if (cp < 0x80) bytes[0] = (char)cp;
                else if (cp < 0x800) { n = 2; bytes[0] = 0xc0 | (cp >> 6); bytes[1] = 0x80 | (cp & 63); }
                else if (cp < 0x10000) { n = 3; bytes[0] = 0xe0 | (cp >> 12); bytes[1] = 0x80 | ((cp >> 6) & 63); bytes[2] = 0x80 | (cp & 63); }
                else { n = 4; bytes[0] = 0xf0 | (cp >> 18); bytes[1] = 0x80 | ((cp >> 12) & 63); bytes[2] = 0x80 | ((cp >> 6) & 63); bytes[3] = 0x80 | (cp & 63); }
            } else bytes[0] = c == 'n' ? '\n' : c == 'r' ? '\r' : c == 't' ? '\t' : c == 'b' ? '\b' : c == 'f' ? '\f' : (char)c;
        } else if (c >= 0xc0) {
            n = c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4;
            if (p + (int)n - 1 > t->end) return false;
            for (size_t k = 1; k < n; k++) bytes[k] = j->text[p++];
        }
        if (used + n >= capacity) { out[used] = 0; return false; }
        memcpy(out + used, bytes, n); used += n;
    }
    out[used] = 0;
    return true;
}
bool json_field(const Json *j, int object, const char *key, char *out, size_t capacity) {
    return json_text(j, json_get(j, object, key), out, capacity);
}
bool json_quote(const char *text, char *out, size_t capacity) {
    size_t n = 0;
    if (capacity < 3) return false;
    out[n++] = '"';
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        if (n + 8 > capacity) return false;
        if (*p == '"' || *p == '\\') { out[n++] = '\\'; out[n++] = *p; }
        else if (*p < 32) { snprintf(out + n, capacity - n, "\\u%04x", *p); n += 6; }
        else out[n++] = *p;
    }
    out[n++] = '"'; out[n] = 0;
    return true;
}

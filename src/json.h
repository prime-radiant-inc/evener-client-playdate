#ifndef EVENER_JSON_H
#define EVENER_JSON_H
#include <stdbool.h>
#include <stddef.h>
typedef struct { const char *text; void *tokens; int count; } Json;
bool json_parse(Json *json, const char *text, size_t length);
void json_free(Json *json);
int json_get(const Json *json, int object, const char *key);
int json_first(const Json *json, int array);
int json_next(const Json *json, int token);
bool json_is(const Json *json, int token, const char *value);
int json_int(const Json *json, int token);
bool json_text(const Json *json, int token, char *out, size_t capacity);
bool json_field(const Json *json, int object, const char *key, char *out, size_t capacity);
bool json_quote(const char *text, char *out, size_t capacity);
size_t utf8_copy(char *out, size_t capacity, const char *text, size_t length);
#endif

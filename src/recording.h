#ifndef EVENER_RECORDING_H
#define EVENER_RECORDING_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Mu-law stores one byte per 8 kHz sample. Writes stay below one tenth second.
#define RECORDING_CHUNK 800u
#define RECORDING_LIMIT (8000u * 120u)
enum RecordingLoad { RECORDING_MISSING, RECORDING_PRESENT, RECORDING_DAMAGED };
bool recording_exists(const char *ref);
bool recording_begin(const char *ref);
enum RecordingLoad recording_load(const char *ref);
bool recording_append(const uint8_t *bytes, size_t count, size_t *accepted);
bool recording_flush(void);
size_t recording_length(void);
int recording_read(size_t offset, uint8_t *bytes, size_t capacity);
bool recording_discard(const char *ref);
void recording_close(void);
#endif

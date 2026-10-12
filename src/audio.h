#ifndef EVENER_AUDIO_H
#define EVENER_AUDIO_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>

#define AUDIO_CAPACITY 32768u

typedef struct {
    int16_t samples[AUDIO_CAPACITY];
    atomic_uint written, read;
    atomic_bool overflow;
} AudioBuffer;
void audio_reset(AudioBuffer *buffer);
bool audio_push(AudioBuffer *buffer, const int16_t *samples, size_t count);
size_t audio_pop(AudioBuffer *buffer, int16_t *samples, size_t count);

typedef struct {
    int16_t history[64];
    unsigned position, phase;
} SpeechEncoder;
void speech_reset(SpeechEncoder *encoder);
bool speech_capture(SpeechEncoder *encoder, AudioBuffer *buffer, const int16_t *input, int count);
#endif

#include "audio.h"
#include <string.h>

void audio_reset(AudioBuffer *buffer) {
    atomic_store(&buffer->written, 0);
    atomic_store(&buffer->read, 0);
    atomic_store(&buffer->overflow, false);
}

bool audio_push(AudioBuffer *buffer, const int16_t *samples, size_t count) {
    unsigned written = atomic_load_explicit(&buffer->written, memory_order_relaxed);
    unsigned read = atomic_load_explicit(&buffer->read, memory_order_acquire);
    if (count > AUDIO_CAPACITY - (written - read)) {
        atomic_store(&buffer->overflow, true);
        return false;
    }
    for (size_t i = 0; i < count; i++) buffer->samples[(written + i) % AUDIO_CAPACITY] = samples[i];
    atomic_store_explicit(&buffer->written, written + (unsigned)count, memory_order_release);
    return true;
}

size_t audio_pop(AudioBuffer *buffer, int16_t *samples, size_t count) {
    unsigned read = atomic_load_explicit(&buffer->read, memory_order_relaxed);
    unsigned written = atomic_load_explicit(&buffer->written, memory_order_acquire);
    if (count > written - read) count = written - read;
    for (size_t i = 0; i < count; i++) samples[i] = buffer->samples[(read + i) % AUDIO_CAPACITY];
    atomic_store_explicit(&buffer->read, read + (unsigned)count, memory_order_release);
    return count;
}

// A 3.2 kHz low-pass FIR protects the 8 kHz speech stream from aliasing.
static const int16_t filter[] = {
    27, 26, 20, 7, -11, -36, -62, -83, -88, -70, -22, 53, 144, 231, 287, 285,
    206, 45, -183, -441, -672, -807, -780, -541, -66, 630, 1491, 2429, 3331,
    4081, 4578, 4752, 4578, 4081, 3331, 2429, 1491, 630, -66, -541, -780,
    -807, -672, -441, -183, 45, 206, 285, 287, 231, 144, 53, -22, -70, -88,
    -83, -62, -36, -11, 7, 20, 26, 27,
};

void speech_reset(SpeechEncoder *encoder) {
    memset(encoder->history, 0, sizeof(encoder->history));
    encoder->position = encoder->phase = 0;
}
static uint8_t mulaw(int32_t value) {
    unsigned sign = value < 0 ? 0x80 : 0;
    if (value < 0) value = -value;
    if (value > 32635) value = 32635;
    value += 132;
    unsigned exponent = 7;
    for (unsigned mask = 0x4000; exponent && !(value & mask); mask >>= 1) exponent--;
    unsigned mantissa = (value >> (exponent + 3)) & 15;
    return (uint8_t)~(sign | (exponent << 4) | mantissa);
}
bool speech_capture(SpeechEncoder *encoder, AudioBuffer *buffer, const int16_t *input, int count) {
    int16_t output[256];
    size_t used = 0;
    for (int i = 0; i < count; i++) {
        encoder->history[encoder->position] = input[i]; encoder->position = (encoder->position + 1) & 63;
        encoder->phase += 8000;
        if (encoder->phase < 44100) continue;
        encoder->phase -= 44100;
        int32_t current = 0, previous = 0;
        for (unsigned tap = 0; tap < sizeof(filter) / sizeof(filter[0]); tap++) {
            current += filter[tap] * encoder->history[(encoder->position - 1 - tap) & 63];
            previous += filter[tap] * encoder->history[(encoder->position - 2 - tap) & 63];
        }
        // Interpolate the filtered signal at the exact 8 kHz sample time.
        current /= 32768; previous /= 32768;
        int32_t sample = (current * (int32_t)(8000 - encoder->phase) + previous * (int32_t)encoder->phase) / 8000;
        output[used++] = mulaw(sample);
        if (used == sizeof(output) / sizeof(output[0])) {
            if (!audio_push(buffer, output, used)) return false;
            used = 0;
        }
    }
    return audio_push(buffer, output, used);
}

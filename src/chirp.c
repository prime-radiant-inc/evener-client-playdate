#include "chirp.h"
#include <math.h>
#include <string.h>

static const unsigned frequencies[4] = {1500, 2100, 2700, 3300};
static const int sync_tones[8] = {0, 1, 3, 2, 1, 0, 2, 3};
void chirp_reset(ChirpDecoder *d) {
    memset(d, 0, sizeof(*d));
    for (unsigned i = 0; i < CHIRP_HISTORY; i++) d->history[i] = -1;
    for (unsigned i = 0; i < 4; i++) d->coefficient[i] = 2 * cosf(6.28318530718f * frequencies[i] / 44100);
}
static int tone(const ChirpDecoder *d) {
    float power[4] = {0}, energy = 0;
    // Short windows keep reflections at a tone change out of the next symbol.
    for (unsigned i = 0; i < CHIRP_WINDOW_SAMPLES; i++) energy += d->samples[i] * d->samples[i];
    if (energy < 0.00005f) return -1;
    for (unsigned t = 0; t < 4; t++) {
        float previous = 0, older = 0;
        for (unsigned i = 0; i < CHIRP_WINDOW_SAMPLES; i++) {
            float value = d->samples[(d->sample_at + i) % CHIRP_WINDOW_SAMPLES] + d->coefficient[t] * previous - older;
            older = previous; previous = value;
        }
        power[t] = previous * previous + older * older - d->coefficient[t] * previous * older;
    }
    unsigned best = 0;
    for (unsigned i = 1; i < 4; i++) if (power[i] > power[best]) best = i;
    if (power[best] < energy * CHIRP_WINDOW_SAMPLES * .12f) return -1;
    return (int)best;
}
static uint32_t little32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static void receive_frame(ChirpDecoder *d) {
    if (d->damaged || pairing_crc(d->frame, 55) != little32(d->frame + 55)) { d->rejected_frames++; return; }
    unsigned length = (unsigned)d->frame[1] | (unsigned)d->frame[2] << 8;
    unsigned index = d->frame[0], fragments = (length + CHIRP_FRAGMENT - 1) / CHIRP_FRAGMENT;
    uint32_t id = little32(d->frame + 3);
    if (!length || length > PAIRING_MAX_BYTES || index >= fragments) { d->rejected_frames++; return; }
    if (d->length != length || d->message_id != id) {
        d->length = length; d->message_id = id; d->received = 0; d->fragments = fragments;
    }
    unsigned count = length - index * CHIRP_FRAGMENT;
    if (count > CHIRP_FRAGMENT) count = CHIRP_FRAGMENT;
    memcpy(d->bytes + index * CHIRP_FRAGMENT, d->frame + 7, count);
    d->received |= 1u << index; d->valid_frames++;
    if (d->received == (1u << fragments) - 1 && pairing_crc(d->bytes, length) != id) {
        d->received = 0; d->rejected_frames++;
    }
}
static uint8_t decode_byte(unsigned code) {
    unsigned syndrome = 0, value = 0, bit = 0;
    for (unsigned i = 1; i <= 12; i++) if (code & (1u << (i - 1))) syndrome ^= i;
    if (syndrome && syndrome <= 12) code ^= 1u << (syndrome - 1);
    for (unsigned i = 1; i <= 12; i++) if (i != 1 && i != 2 && i != 4 && i != 8)
        value |= ((code >> (i - 1)) & 1) << bit++;
    return (uint8_t)value;
}
static void hop(ChirpDecoder *d) {
    int value = tone(d);
    d->history[d->hops % CHIRP_HISTORY] = value;
    if (d->receiving) {
        if (d->hops == d->next_symbol) {
            d->next_symbol += CHIRP_SYMBOL_HOPS;
            if (value < 0) { value = 0; d->damaged = true; }
            d->codeword |= (unsigned)value << (2 * d->symbols++);
            if (d->symbols == 6) {
                d->frame[d->frame_at++] = decode_byte(d->codeword);
                d->symbols = d->codeword = 0;
                if (d->frame_at == CHIRP_FRAME_BYTES) { d->receiving = false; receive_frame(d); }
            }
        }
    } else if (d->hops >= CHIRP_HISTORY) {
        bool sync = true;
        for (unsigned i = 0; i < 8; i++)
            if (d->history[(d->hops - (7 - i) * CHIRP_SYMBOL_HOPS) % CHIRP_HISTORY] != sync_tones[i]) sync = false;
        if (sync) {
            d->receiving = true; d->damaged = false;
            d->symbols = d->codeword = d->frame_at = 0;
            // Detect the leading part of the sync tone, then sample farther
            // inside each data symbol to allow clock drift and reflections.
            d->next_symbol = d->hops + CHIRP_SYMBOL_HOPS + 2;
        }
    }
    d->hops++;
}
void chirp_feed(ChirpDecoder *d, const int16_t *samples, size_t count) {
    for (size_t i = 0; i < count; i++) {
        d->samples[d->sample_at] = samples[i] / 32768.f;
        d->sample_at = (d->sample_at + 1) % CHIRP_WINDOW_SAMPLES;
        if (++d->hop_samples == CHIRP_HOP_SAMPLES) { d->hop_samples = 0; hop(d); }
    }
}
bool chirp_complete(const ChirpDecoder *d) { return d->fragments && d->received == (1u << d->fragments) - 1; }
unsigned chirp_progress(const ChirpDecoder *d) {
    unsigned count = 0;
    for (uint32_t bits = d->received; bits; bits >>= 1) count += bits & 1;
    return d->fragments ? count * 100 / d->fragments : 0;
}

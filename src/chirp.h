#ifndef EVENER_CHIRP_H
#define EVENER_CHIRP_H
#include "pairing_protocol.h"
#define CHIRP_FRAGMENT 48
#define CHIRP_FRAME_BYTES 59
#define CHIRP_SYMBOL_SAMPLES 441
#define CHIRP_WINDOW_SAMPLES 147
#define CHIRP_HOP_SAMPLES 49
#define CHIRP_SYMBOL_HOPS (CHIRP_SYMBOL_SAMPLES / CHIRP_HOP_SAMPLES)
#define CHIRP_HISTORY (8 * CHIRP_SYMBOL_HOPS)
typedef struct {
    uint8_t frame[CHIRP_FRAME_BYTES];
    unsigned symbols, codeword, frame_at;
    bool damaged;
} ChirpPhase;
typedef struct {
    float samples[CHIRP_WINDOW_SAMPLES], coefficient[4];
    unsigned sample_at, hop_samples, hops, first_symbol;
    int history[CHIRP_HISTORY];
    bool receiving;
    ChirpPhase phases[CHIRP_SYMBOL_HOPS];
    uint8_t bytes[PAIRING_MAX_BYTES];
    uint32_t message_id, received;
    unsigned length, fragments, valid_frames, rejected_frames;
} ChirpDecoder;
void chirp_reset(ChirpDecoder *decoder);
void chirp_feed(ChirpDecoder *decoder, const int16_t *samples, size_t count);
bool chirp_complete(const ChirpDecoder *decoder);
unsigned chirp_progress(const ChirpDecoder *decoder);
#endif

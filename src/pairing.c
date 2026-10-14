#include "pairing.h"
#include "chirp.h"
#include "audio.h"
#include "random.h"
#include "settings.h"
#include "qrcodegen.h"
#include "pairing_http.h"
#include "tailnet_device.h"
#include <stdio.h>
#include <string.h>

static bool active, listening, review, asking, allowed, paused, received_invitation, terminal;
static uint8_t secret[32], envelope[PAIRING_MAX_BYTES];
static char public_key[65], status[160];
static PairingInvitation invitation;
static PairingSettings candidate;
static char comparison[PAIRING_COMPARISON_SIZE];
static float started;
static AudioBuffer audio;
static ChirpDecoder decoder;
static atomic_uint microphone_peak, microphone_samples, microphone_clipped;
static unsigned input_peak, interval_peak, interruptions;
static float input_log_at;
static uint8_t qr_work[qrcodegen_BUFFER_LEN_FOR_VERSION(12)], qr_code[qrcodegen_BUFFER_LEN_FOR_VERSION(12)];
static char qr_cached[512];
static bool qr_valid;
static void listen(void);

bool pairing_draw_qr(const char *value, int x, int y, int maximum) {
    if (strlen(value) >= sizeof(qr_cached)) return false;
    if (strcmp(qr_cached, value)) {
        snprintf(qr_cached, sizeof(qr_cached), "%s", value);
        qr_valid = qrcodegen_encodeText(value, qr_work, qr_code, qrcodegen_Ecc_MEDIUM, 1, 12, qrcodegen_Mask_AUTO, true);
    }
    if (!qr_valid) return false;
    int size = qrcodegen_getSize(qr_code), scale = maximum / (size + 8);
    if (!scale) return false;
    int extent = (size + 8) * scale;
    pd->graphics->fillRect(x, y, extent, extent, kColorWhite);
    for (int row = 0; row < size; row++) for (int col = 0; col < size; col++)
        if (qrcodegen_getModule(qr_code, col, row))
            pd->graphics->fillRect(x + (col + 4) * scale, y + (row + 4) * scale, scale, scale, kColorBlack);
    return true;
}
static void stop_mic(void) {
    if (listening) pd->sound->setMicCallback(NULL, NULL, kMicInputInternal);
    listening = false; input_peak = 0;
}
static int microphone(void *context, int16_t *samples, int count) {
    (void)context;
    if (count <= 0) return 1;
    unsigned peak = 0, clipped = 0;
    for (int i = 0; i < count; i++) {
        int value = samples[i];
        if (value < 0) value = -value;
        if ((unsigned)value > peak) peak = (unsigned)value;
        if (value >= 32760) clipped++;
    }
    // Keep brief peaks between game updates without doing decoding or drawing here.
    unsigned previous = atomic_load_explicit(&microphone_peak, memory_order_relaxed);
    while (peak > previous && !atomic_compare_exchange_weak_explicit(&microphone_peak, &previous, peak,
        memory_order_relaxed, memory_order_relaxed)) {}
    atomic_fetch_add_explicit(&microphone_samples, (unsigned)count, memory_order_relaxed);
    atomic_fetch_add_explicit(&microphone_clipped, clipped, memory_order_relaxed);
    audio_push(&audio, samples, (size_t)count);
    return 1;
}
static void update_input(void) {
    unsigned peak = atomic_exchange_explicit(&microphone_peak, 0, memory_order_relaxed);
    input_peak = input_peak * 3 / 4;
    if (peak > input_peak) input_peak = peak;
    if (peak > interval_peak) interval_peak = peak;
    if (!listening || app_time() < input_log_at) return;
    unsigned captured = atomic_exchange_explicit(&microphone_samples, 0, memory_order_relaxed);
    unsigned clipped = atomic_exchange_explicit(&microphone_clipped, 0, memory_order_relaxed);
    unsigned queued = atomic_load(&audio.written) - atomic_load(&audio.read);
    app_log("Pairing input: samples=%u peak=%u clipped=%u queued=%u interruptions=%u valid=%u rejected=%u",
        captured, interval_peak, clipped, queued, interruptions, decoder.valid_frames, decoder.rejected_frames);
    interval_peak = interruptions = 0; input_log_at = app_time() + 1;
}
static void permission(bool granted, void *context) {
    (void)context; asking = false; allowed = granted;
    if (!active) return;
    if (!granted) snprintf(status, sizeof(status), "Allow microphone access in Settings, then try again.");
    else if (!paused) listen();
}
static void listen(void) {
    if (!active || review || received_invitation || terminal || listening || paused || asking) return;
    if (!allowed) {
        asking = true;
        enum accessReply reply = pd->sound->requestMicAccess("Receive your Evener setup invitation.", permission, NULL);
        if (reply != kAccessAsk) permission(reply == kAccessAllow, NULL);
        if (!allowed || listening || !active || paused) return;
    }
    audio_reset(&audio);
    atomic_store(&microphone_peak, 0); atomic_store(&microphone_samples, 0); atomic_store(&microphone_clipped, 0);
    input_peak = interval_peak = interruptions = 0; input_log_at = app_time() + 1;
    listening = pd->sound->setMicCallback(microphone, NULL, kMicInputInternal) != 0;
    snprintf(status, sizeof(status), "%s", listening ? "Listening..." : "Microphone unavailable.");
}
bool pairing_begin(void) {
    if (asking || !settings_cancel_pending()) return false;
    stop_mic(); pairing_http_stop(); tailnet_device_stop(); settings_bootstrap_end();
    random_fill(secret, sizeof(secret)); chirp_reset(&decoder);
    memset(&candidate, 0, sizeof(candidate)); memset(&invitation, 0, sizeof(invitation));
    active = true; review = paused = received_invitation = terminal = false;
    started = app_time(); status[0] = 0;
    listen(); return true;
}
bool pairing_active(void) { return active; }
bool pairing_ready_to_save(void) { return active && review; }
void pairing_pause(void) {
    paused = true; stop_mic(); pairing_http_stop(); tailnet_device_stop();
}
void pairing_resume(void) {
    paused = false;
    if (received_invitation && !review && !terminal) {
        pairing_http_start(&invitation, public_key, comparison);
        if (config.use_tailnet) tailnet_device_start();
    } else listen();
}
static void finish(void) {
    stop_mic(); pairing_http_stop(); tailnet_device_stop(); settings_bootstrap_end(); active = false;
    memset(secret, 0, sizeof(secret)); memset(&candidate, 0, sizeof(candidate)); memset(&invitation, 0, sizeof(invitation));
    memset(envelope, 0, sizeof(envelope)); memset(&decoder, 0, sizeof(decoder)); memset(&audio, 0, sizeof(audio));
}
static void end_attempt(const char *message) {
    stop_mic(); pairing_http_stop(); tailnet_device_stop(); terminal = true;
    memset(secret, 0, sizeof(secret)); snprintf(status, sizeof(status), "%s", message);
}
enum PairingResult pairing_update(PDButtons pressed) {
    if (pressed & kButtonB) {
        if (!settings_cancel_pending()) {
            snprintf(status, sizeof(status), "Could not cancel the pending save. Press B to retry.");
            return PAIRING_RUNNING;
        }
        finish(); return PAIRING_CANCELLED;
    }
    if (paused) return PAIRING_RUNNING;
    if (!review && !terminal && app_time() - started > 600)
        end_attempt("Invitation expired. Create a new one, then press A to listen.");
    if (terminal) { if (pressed & kButtonA) pairing_begin(); return PAIRING_RUNNING; }
    if (review) {
        if (pressed & kButtonA) {
            if (settings_save(&candidate)) { finish(); return PAIRING_APPLIED; }
            snprintf(status, sizeof(status), "Could not save this Hub. Try saving again.");
        }
        return PAIRING_RUNNING;
    }
    if (received_invitation) {
        if (config.use_tailnet) tailnet_device_update();
        size_t length;
        enum PairingHTTPResult result = pairing_http_update(envelope, sizeof(envelope), &length);
        snprintf(status, sizeof(status), "%s", pairing_http_status());
        if (result == PAIRING_HTTP_APPROVED) {
            if (pairing_approved(envelope, length, &invitation, secret, &candidate)) {
                review = true; pairing_http_stop(); tailnet_device_stop();
                status[0] = 0;
            } else end_attempt("Could not verify approval. Create a new invitation and retry.");
            memset(envelope, 0, sizeof(envelope));
        } else if (result == PAIRING_HTTP_ENDED) {
            // Copy before stopping the transport so its diagnostic remains available.
            char message[160]; snprintf(message, sizeof(message), "%s", status); end_attempt(message);
        }
        return PAIRING_RUNNING;
    }
    if (pressed & kButtonA) listen();
    if (atomic_exchange(&audio.overflow, false)) {
        interruptions++;
        decoder.receiving = false; decoder.hops = decoder.hop_samples = 0;
        for (unsigned i = 0; i < CHIRP_HISTORY; i++) decoder.history[i] = -1;
        snprintf(status, sizeof(status), "Sound interrupted. Play it again to finish.");
    }
    int16_t samples[512];
    for (unsigned budget = 0; budget < 12; budget++) {
        size_t count = audio_pop(&audio, samples, 512);
        if (!count) break;
        chirp_feed(&decoder, samples, count);
    }
    update_input();
    if (chirp_complete(&decoder)) {
        if (pairing_invitation(decoder.bytes, decoder.length, &invitation)) {
            stop_mic(); received_invitation = true; started = app_time();
            pairing_claim_key(&invitation, secret, public_key, comparison);
            settings_bootstrap_begin(&invitation.destination);
            if (config.use_tailnet && !tailnet_device_initialize()) {
                end_attempt("Could not save Tailscale identity. Press A to retry setup."); return PAIRING_RUNNING;
            }
            pairing_http_start(&invitation, public_key, comparison);
            if (config.use_tailnet) tailnet_device_start();
        } else {
            chirp_reset(&decoder);
            snprintf(status, sizeof(status), "Unsupported invitation. Use a DNS or IPv4 Hub; native Tailscale needs HTTP.");
        }
    }
    return PAIRING_RUNNING;
}
static void text(const char *value, int x, int y, int width, int height) {
    pd->graphics->drawTextInRect(value, strlen(value), kUTF8Encoding, x, y, width, height, kWrapWord, kAlignTextLeft);
}
void pairing_draw(LCDFont *font) {
    pd->graphics->clear(kColorWhite); pd->graphics->setFont(font);
    const char *auth = received_invitation && config.use_tailnet && !review && !terminal ? tailnet_device_auth_url() : "";
    const char *title = "Receive setup";
    if (review) title = "Hub approved";
    else if (auth[0]) title = "Join your Hub's tailnet";
    else if (received_invitation && !terminal) title = "Pair with Hub";
    pd->graphics->fillRect(0, 0, 400, 21, kColorBlack);
    pd->graphics->setDrawMode(kDrawModeFillWhite);
    text(title, 8, 4, 384, 16);
    pd->graphics->setDrawMode(kDrawModeCopy);
    char summary[384];
    if (received_invitation && !terminal) {
        if (auth[0] && pairing_draw_qr(auth, 5, 31, 184)) {
            text("Scan with your phone.\n\nSign in to Tailscale and choose the tailnet with your Hub.\n\nThis screen updates automatically.", 199, 36, 193, 170);
            text("B Cancel", 8, 223, 384, 16);
            return;
        }
        snprintf(summary, sizeof(summary), "%s:%u\n%s", invitation.destination.host, invitation.destination.port,
            invitation.destination.tailnet ? "Tailscale" : invitation.destination.tls ? "Wi-Fi with TLS" : "Wi-Fi");
        text(summary, 12, 34, 376, review ? 110 : 38);
        if (!review) {
            text("Compare all four lines in your browser:", 12, 78, 376, 20);
            for (int row = 0; row < 4; row++) {
                snprintf(summary, sizeof(summary), "%.4s  %.4s", comparison + row * 10, comparison + row * 10 + 5);
                text(summary, 24, 103 + row * 19, 352, 19);
            }
        }
        text(status, 12, review ? 153 : 187, 376, review ? 62 : 31);
        text(review ? "A Save and connect    B Cancel" : "B Cancel", 8, 223, 384, 16);
    } else {
        if (!terminal) {
            text("In Evener: Settings > Mobile app > Pair using sound.\n\nPlay the setup sound near this Playdate.", 12, 36, 376, 84);
            snprintf(summary, sizeof(summary), "Received: %u%%", chirp_progress(&decoder));
            text(summary, 12, 128, 376, 20);
            text("Mic", 12, 152, 44, 16);
            // Six-decibel steps make quiet input visible independently of valid fragments.
            const unsigned thresholds[] = {16, 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384, 32767};
            for (unsigned i = 0; i < sizeof(thresholds) / sizeof(thresholds[0]); i++) {
                int x = 60 + (int)i * 27;
                pd->graphics->fillRect(x, 154, 24, 12, kColorBlack);
                if (input_peak < thresholds[i]) pd->graphics->fillRect(x + 2, 156, 20, 8, kColorWhite);
            }
        }
        text(status, 12, terminal ? 36 : 172, 376, terminal ? 175 : 44);
        const char *actions = "B Cancel";
        if (terminal) actions = "A Listen again    B Cancel";
        else if (!listening) actions = "A Retry microphone    B Cancel";
        text(actions, 8, 223, 384, 16);
    }
}

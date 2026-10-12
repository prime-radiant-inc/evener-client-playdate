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
static unsigned comparison;
static float started;
static AudioBuffer audio;
static ChirpDecoder decoder;
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
    listening = false;
}
static int microphone(void *context, int16_t *samples, int count) {
    (void)context;
    if (count > 0) audio_push(&audio, samples, (size_t)count);
    return 1;
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
    listening = pd->sound->setMicCallback(microphone, NULL, kMicInputInternal) != 0;
    snprintf(status, sizeof(status), "%s", listening ? "Listening for setup sound..." : "Microphone unavailable. Press A to retry.");
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
            snprintf(status, sizeof(status), "Could not save setup. Press A to retry, or B to cancel.");
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
                snprintf(status, sizeof(status), "Approved by your Hub. Press A to save this destination.");
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
    if (chirp_complete(&decoder)) {
        if (pairing_invitation(decoder.bytes, decoder.length, &invitation)) {
            stop_mic(); received_invitation = true; started = app_time();
            pairing_claim_key(&invitation, secret, public_key, &comparison);
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
    pd->graphics->fillRect(0, 0, 400, 21, kColorBlack);
    pd->graphics->setDrawMode(kDrawModeFillWhite);
    text(review ? "Save Evener setup" : "Receive setup", 8, 4, 384, 16);
    pd->graphics->setDrawMode(kDrawModeCopy);
    char summary[384];
    if (received_invitation && !terminal) {
        const char *auth = config.use_tailnet && !review ? tailnet_device_auth_url() : "";
        if (auth[0] && pairing_draw_qr(auth, 5, 31, 184)) {
            snprintf(summary, sizeof(summary), "Sign in to Tailscale\n\n%s\n\nThen compare: %06u", auth, comparison);
            text(summary, 199, 36, 193, 157);
            text("B Cancel", 8, 223, 384, 16);
            return;
        }
        snprintf(summary, sizeof(summary), "%s:%u\n%s\n\nCompare code: %06u", invitation.destination.host, invitation.destination.port,
            invitation.destination.tailnet ? "Tailscale" : invitation.destination.tls ? "Wi-Fi with TLS" : "Wi-Fi", comparison);
        text(summary, 12, 38, 376, 110);
        text(status, 12, 159, 376, 55);
        text(review ? "A Save setup    B Cancel" : "B Cancel    Approve in browser", 8, 223, 384, 16);
    } else {
        snprintf(summary, sizeof(summary), "In Evener Settings, choose Pair a device.\n\nCreate an invitation and play its setup sound near this Playdate.\n\nReceived: %u%%", chirp_progress(&decoder));
        text(summary, 12, 36, 376, 130);
        text(status, 12, 172, 376, 44);
        text(terminal ? "A Listen again    B Cancel" : "B Cancel    A Retry microphone", 8, 223, 384, 16);
    }
}

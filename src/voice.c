#include "app.h"
#include "voice.h"
#include "websocket.h"
#include "json.h"
#include "audio.h"
#include "recording.h"
#include "storage.h"
#include <stdio.h>
#include <string.h>

enum VoiceState voice_state;
char voice_text[DRAFT_CAPACITY], voice_status[256];
static WebSocket socket;
static AudioBuffer audio;
static atomic_bool capturing;
static SpeechEncoder encoder;
static size_t sent_samples;
static float began, finished, connected_at, committed_at, retry_at;
static unsigned retry_delay = 1;
static bool configured, ready, committed, first_caption, replay_caption, active, storage_failed, padded, retry_pending;
static uint8_t pending[RECORDING_CHUNK];
static size_t pending_count, pending_offset;
static char recording_ref[REF_CAPACITY], capture_notice[96];
static char item_id[128];

static void stop_capture(void) {
    atomic_store(&capturing, false);
    pd->sound->setMicCallback(NULL, NULL, kMicInputInternal);
}
static void retry(const char *reason) {
    retry_pending = true;
    configured = ready = committed = replay_caption = false;
    sent_samples = 0; padded = false; item_id[0] = 0;
    retry_at = app_time() + retry_delay;
    if (retry_delay < 16) retry_delay *= 2;
    snprintf(voice_status, sizeof(voice_status), "%s Audio kept; retrying.", reason);
    app_log("Voice retry: %s; captured %u; queued %u", reason,
        (unsigned)recording_length(), atomic_load(&audio.written) - atomic_load(&audio.read));
}
static void capture_failed(const char *reason) {
    stop_capture();
    if (voice_state == VOICE_RECORDING) finished = app_time();
    if (voice_state != VOICE_OFF) voice_state = VOICE_FINISHING;
    snprintf(capture_notice, sizeof(capture_notice), "%s", reason);
    snprintf(voice_status, sizeof(voice_status), "%s Audio kept.", reason);
    app_log("Voice capture stopped: %s", reason);
}
static int microphone(void *context, int16_t *input, int count) {
    (void)context;
    if (!atomic_load(&capturing)) return 0;
    return speech_capture(&encoder, &audio, input, count) ? 1 : 0;
}
static void message(WebSocket *ws, const char *text, size_t length) {
    (void)ws;
    if (retry_pending) return;
    Json j;
    if (!json_parse(&j, text, length)) { retry("Invalid transcription response."); return; }
    int type = json_get(&j, 0, "type");
    if (json_is(&j, type, "error")) {
        char reason[160];
        json_field(&j, json_get(&j, 0, "error"), "message", reason, sizeof(reason));
        retry(reason[0] ? reason : "Transcription service error.");
    } else if (json_is(&j, type, "input_audio_buffer.committed")) {
        app_log("Direct commit acknowledged after %d ms", (int)((app_time() - finished) * 1000));
    } else if (json_is(&j, type, "session.updated") && configured) {
        ready = true; retry_delay = 1;
        app_log("Direct transcription ready (TLS, 8 kHz mu-law)");
    } else if (json_is(&j, type, "conversation.item.input_audio_transcription.failed")) {
        retry("Could not transcribe this recording.");
    } else if ((voice_state == VOICE_RECORDING || voice_state == VOICE_FINISHING) &&
        (json_is(&j, type, "conversation.item.input_audio_transcription.delta") ||
         json_is(&j, type, "conversation.item.input_audio_transcription.completed"))) {
        char id[128];
        json_field(&j, 0, "item_id", id, sizeof(id));
        if (item_id[0] && strcmp(item_id, id)) { json_free(&j); retry("Transcription item changed."); return; }
        snprintf(item_id, sizeof(item_id), "%s", id);
        bool final = json_is(&j, type, "conversation.item.input_audio_transcription.completed");
        if (final && !committed) { json_free(&j); return; }
        size_t offset = final || !replay_caption ? 0 : strlen(voice_text);
        if (!json_field(&j, 0, final ? "transcript" : "delta", voice_text + offset, sizeof(voice_text) - offset)) {
            json_free(&j); retry("Voice draft is full."); return;
        }
        replay_caption = true;
        if (!first_caption) {
            first_caption = true;
            app_log("Direct first caption: %d ms", (int)((app_time() - began) * 1000));
        }
        if (final) {
            stop_capture();
            voice_state = VOICE_REVIEW;
            snprintf(voice_status, sizeof(voice_status), "%s", capture_notice[0] ? capture_notice : "Review your message");
            app_log("Direct final caption: %d ms; audio bytes: %u",
                (int)((app_time() - began) * 1000), (unsigned)sent_samples);
        }
    }
    json_free(&j);
}
static void connect_transcription(void) {
    configured = ready = committed = replay_caption = false;
    sent_samples = 0; padded = false; item_id[0] = 0;
    retry_pending = false;
    connected_at = app_time();
    app_log("Voice connecting: %d ms after microphone start; captured %u",
        (int)((connected_at - began) * 1000), (unsigned)recording_length());
    ws_connect(&socket, "api.openai.com", 443, true, "/v1/realtime?intent=transcription",
        config.openai_key, 65536, message);
}
static void spool(void) {
    if (!active || voice_state == VOICE_REVIEW) return;
    if (!recording_flush()) {
        storage_failed = true;
        capture_failed("Storage unavailable; keep Evener open."); return;
    }
    if (!pending_count) {
        static int16_t samples[RECORDING_CHUNK];
        pending_count = audio_pop(&audio, samples, RECORDING_CHUNK);
        pending_offset = 0;
        for (size_t i = 0; i < pending_count; i++) pending[i] = (uint8_t)samples[i];
    }
    if (pending_count) {
        size_t remaining = RECORDING_LIMIT - recording_length();
        size_t count = pending_count - pending_offset;
        if (count > remaining) count = remaining;
        if (!count) {
            pending_count = pending_offset = 0;
            audio_reset(&audio);
            capture_failed("Two-minute recording limit reached."); return;
        }
        size_t accepted = 0;
        bool saved = recording_append(pending + pending_offset, count, &accepted);
        pending_offset += accepted;
        if (pending_offset == pending_count) pending_count = pending_offset = 0;
        if (!saved) {
            storage_failed = true;
            capture_failed("Storage unavailable; keep Evener open."); return;
        }
    }
    storage_failed = false;
    if (recording_length() == RECORDING_LIMIT && !capture_notice[0]) {
        stop_capture();
        pending_count = pending_offset = 0; audio_reset(&audio);
        capture_failed("Two-minute recording limit reached.");
    }
}
void voice_close(void) {
    if (voice_state == VOICE_RECORDING) voice_finish();
    stop_capture();
    // One bounded drain also covers suspend between the callback and next frame.
    spool();
    ws_close(&socket); retry_pending = false;
    voice_state = VOICE_OFF;
}
bool voice_terminate(void) {
    if (voice_state == VOICE_RECORDING) voice_finish();
    stop_capture();
    bool saved = true;
    do {
        spool();
        if (active && storage_failed) { saved = false; break; }
    } while (active && (pending_count || atomic_load(&audio.written) != atomic_load(&audio.read)));
    ws_close(&socket); retry_pending = false;
    if (saved) recording_close();
    voice_state = VOICE_OFF;
    app_log("Voice termination: %u saved bytes; pending %u; complete %d", (unsigned)recording_length(),
        (unsigned)(pending_count - pending_offset) + atomic_load(&audio.written) - atomic_load(&audio.read), saved);
    return saved;
}
void voice_connect(void) {
    if (!active || voice_state == VOICE_REVIEW) return;
    ws_close(&socket); configured = ready = committed = false;
    retry_at = app_time();
    if (voice_state == VOICE_OFF || voice_state == VOICE_ERROR) voice_state = VOICE_FINISHING;
}
bool voice_start(void) {
    if (active || draft.state != DRAFT_EMPTY || !recording_begin(draft.ref)) {
        snprintf(voice_status, sizeof(voice_status), "Saved input needs review. Menu Discard to replace it.");
        voice_state = VOICE_ERROR; return false;
    }
    audio_reset(&audio);
    speech_reset(&encoder);
    pending_count = pending_offset = sent_samples = 0;
    configured = ready = first_caption = committed = replay_caption = storage_failed = padded = retry_pending = false;
    item_id[0] = voice_text[0] = capture_notice[0] = 0;
    snprintf(recording_ref, sizeof(recording_ref), "%s", draft.ref);
    began = app_time(); retry_at = began; retry_delay = 1;
    voice_state = VOICE_RECORDING; active = true;
    atomic_store(&capturing, true);
    if (!pd->sound->setMicCallback(microphone, NULL, kMicInputInternal)) {
        capture_failed("Microphone could not start.");
        voice_state = VOICE_ERROR;
        if (atomic_load(&audio.written) == atomic_load(&audio.read)) {
            active = false; recording_close();
            snprintf(voice_status, sizeof(voice_status), "Microphone could not start. Press A to retry.");
        }
        return false;
    }
    snprintf(voice_status, sizeof(voice_status), "Recording; connecting to transcription...");
    app_log("Voice microphone started synchronously; session %s", recording_ref);
    return true;
}
void voice_finish(void) {
    if (voice_state != VOICE_RECORDING) return;
    stop_capture(); voice_state = VOICE_FINISHING;
    finished = app_time();
    snprintf(voice_status, sizeof(voice_status), "Audio captured; finishing transcription...");
    app_log("Direct recording released: %d ms; saved %u; queued %u", (int)((finished - began) * 1000),
        (unsigned)recording_length(), atomic_load(&audio.written) - atomic_load(&audio.read));
}
bool voice_recover(void) {
    if (draft.state == DRAFT_READY || draft.state == DRAFT_SENDING || draft.state == DRAFT_UNCERTAIN) {
        voice_close(); voice_status[0] = 0;
        if (active && !strcmp(recording_ref, draft.ref)) { recording_close(); active = false; }
        if (!active) recording_discard(draft.ref);
        return false;
    }
    if (active) {
        if (strcmp(recording_ref, draft.ref)) {
            stop_capture();
            voice_state = VOICE_ERROR;
            snprintf(voice_status, sizeof(voice_status), "Previous audio still saving. Keep Evener open.");
            return true;
        }
        voice_connect(); return true;
    }
    voice_state = VOICE_OFF; voice_status[0] = 0;
    snprintf(voice_text, sizeof(voice_text), "%s", draft.text);
    enum RecordingLoad loaded = recording_load(draft.ref);
    if (loaded == RECORDING_MISSING) return false;
    if (loaded == RECORDING_DAMAGED) {
        voice_state = VOICE_ERROR;
        snprintf(voice_status, sizeof(voice_status), "Saved audio could not be read. Menu Discard to replace it.");
        return true;
    }
    snprintf(recording_ref, sizeof(recording_ref), "%s", draft.ref);
    snprintf(voice_text, sizeof(voice_text), "%s", draft.text);
    audio_reset(&audio); pending_count = pending_offset = 0;
    active = true; storage_failed = false; first_caption = false; capture_notice[0] = 0;
    began = finished = app_time(); retry_delay = 1;
    voice_state = VOICE_FINISHING;
    voice_connect();
    snprintf(voice_status, sizeof(voice_status), "Recovered recording; transcribing...");
    app_log("Voice recovered %u bytes for %s", (unsigned)recording_length(), recording_ref);
    return true;
}
void voice_saved(void) {
    if (voice_state != VOICE_REVIEW || strcmp(recording_ref, draft.ref)) return;
    if (!recording_discard(recording_ref)) {
        snprintf(voice_status, sizeof(voice_status), "Text saved; audio cleanup will retry."); return;
    }
    active = false; recording_ref[0] = 0;
    ws_close(&socket);
}
void voice_discard(void) {
    if (active && strcmp(recording_ref, draft.ref)) return;
    stop_capture(); ws_close(&socket);
    if (!recording_discard(draft.ref)) {
        voice_state = VOICE_ERROR;
        snprintf(voice_status, sizeof(voice_status), "Could not discard audio. Your recording is kept."); return;
    }
    active = false; pending_count = pending_offset = 0;
    recording_ref[0] = voice_text[0] = 0;
    audio_reset(&audio); voice_state = VOICE_OFF;
}
void voice_update(void) {
    spool();
    if (retry_pending) { ws_close(&socket); retry_pending = false; }
    if ((voice_state == VOICE_OFF || voice_state == VOICE_ERROR) && active && !storage_failed && !pending_count &&
        atomic_load(&audio.written) == atomic_load(&audio.read)) {
        recording_close(); active = false;
    }
    if (voice_state == VOICE_REVIEW) {
        ws_close(&socket);
        return;
    }
    if (voice_state == VOICE_OFF || voice_state == VOICE_ERROR) return;
    if (atomic_load(&audio.overflow) && voice_state == VOICE_RECORDING)
        capture_failed("Capture buffer full; review captured speech.");
    if (storage_failed) return;
    if (socket.state == WS_CLOSED && app_time() >= retry_at) connect_transcription();
    ws_update(&socket);
    // The completed response releases its scarce SDK connection even when
    // saving the draft or removing the retained recording needs another try.
    if (voice_state == VOICE_REVIEW) { ws_close(&socket); return; }
    if (net_waiting(socket.connection)) connected_at = app_time();
    if (retry_pending) { ws_close(&socket); retry_pending = false; return; }
    if (socket.state == WS_FAILED) { retry(socket.error); return; }
    if (socket.state == WS_OPEN && !configured && ws_send(&socket,
        "{\"type\":\"session.update\",\"session\":{\"type\":\"transcription\",\"audio\":{\"input\":{"
        "\"format\":{\"type\":\"audio/pcmu\"},\"transcription\":{\"model\":\"gpt-live-transcribe\","
        "\"delay\":\"low\"},\"turn_detection\":null}}}}")) configured = true;
    if (!ready && socket.state != WS_CLOSED && app_time() - connected_at > 45) {
        retry("Transcription connection timed out."); return;
    }
    if (committed && app_time() - committed_at > 40) { retry("Transcription timed out."); return; }
    if (!ready || !ws_writable(&socket) || committed) return;
    static uint8_t bytes[RECORDING_CHUNK];
    static char encoded[1069], request[1200];
    int count = recording_read(sent_samples, bytes, sizeof(bytes));
    if (count < 0 || (!count && sent_samples != recording_length())) {
        app_log("Voice saved audio incomplete: sent %u of %u bytes", (unsigned)sent_samples, (unsigned)recording_length());
        retry("Could not read complete saved audio."); return;
    }
    if (count) {
        base64_encode(bytes, (size_t)count, encoded);
        snprintf(request, sizeof(request), "{\"type\":\"input_audio_buffer.append\",\"audio\":\"%s\"}", encoded);
        if (!ws_send(&socket, request)) { retry("Could not queue microphone audio."); return; }
        if (!sent_samples) app_log("Voice first audio queued: %d ms after microphone start", (int)((app_time() - began) * 1000));
        sent_samples += (size_t)count;
    } else if (voice_state == VOICE_FINISHING && !pending_count &&
        atomic_load(&audio.written) == atomic_load(&audio.read)) {
        if (!sent_samples) {
            voice_state = VOICE_REVIEW;
            snprintf(voice_status, sizeof(voice_status), "No speech captured. Menu Discard to retry.");
            return;
        } else if (sent_samples < 800 && !padded) {
            // Realtime needs at least 100 ms. Silence completes a short tap without
            // dropping the speech captured before its connection was ready.
            memset(bytes, 0xff, 800 - sent_samples);
            base64_encode(bytes, 800 - sent_samples, encoded);
            snprintf(request, sizeof(request), "{\"type\":\"input_audio_buffer.append\",\"audio\":\"%s\"}", encoded);
            if (ws_send(&socket, request)) padded = true;
        } else if (ws_send(&socket, "{\"type\":\"input_audio_buffer.commit\"}")) {
            committed = true; committed_at = app_time();
            app_log("Direct commit queued after %d ms; %u audio bytes; pending %u", (int)((app_time() - finished) * 1000),
                (unsigned)sent_samples, (unsigned)ws_pending(&socket));
        }
    }
    if (!capture_notice[0]) snprintf(voice_status, sizeof(voice_status), "%s", voice_state == VOICE_RECORDING ?
        "Recording; live transcription" : "Finishing transcription...");
}

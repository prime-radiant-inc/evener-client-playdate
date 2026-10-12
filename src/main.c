#include "app.h"
#include "json.h"
#include "websocket.h"
#include "voice.h"
#include "recording.h"
#include "hub.h"
#include "tailnet_device.h"
#include "settings.h"
#include "pairing.h"
#include "net_socket.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

PlaydateAPI *pd;
Configuration config;
static LCDFont *font, *small_font;
enum { MARGIN = 8, CONTENT_TOP = 24, CONTENT_HEIGHT = 196, FOOTER_TOP = 224, SESSION_ROWS = 6, BUBBLE_WIDTH = 374 };
enum Screen { SCREEN_SESSIONS, SCREEN_TRANSCRIPT, SCREEN_COMPOSER, SCREEN_AUTH };
static enum Screen screen;
static int permission_step, scroll, selection, content_height;
static int row_y[CHAT_CAPACITY], row_height[CHAT_CAPACITY], row_width[CHAT_CAPACITY], measured_revision = -1;
static char measured_keys[CHAT_CAPACITY][192];
static int measured_count;
static float crank_remainder;
static bool permission_pending, permissions_ready, follow = true;
static bool recording_started, final_saved, saved_audio;
static float saved_at, save_retry_at, repeat_at;
static char error[256];
static PDMenuItem *network_menu;
static enum Screen before_pairing;
static bool seed_ready, setup_requested, change_tailnet;

static bool read_configuration(void) {
    static char bytes[4096];
    SDFile *file = pd->file->open("config.json", kFileRead | kFileReadData);
    if (!file) return false;
    int n = pd->file->read(file, bytes, sizeof(bytes) - 1);
    pd->file->close(file);
    if (n <= 0) return false;
    bytes[n] = 0;
    Json j;
    if (!json_parse(&j, bytes, n)) return false;
    json_field(&j, 0, "hubHost", config.hub_host, sizeof(config.hub_host));
    json_field(&j, 0, "hubPath", config.hub_path, sizeof(config.hub_path));
    json_field(&j, 0, "hubToken", config.hub_token, sizeof(config.hub_token));
    bool ok = json_field(&j, 0, "openaiKey", config.openai_key, sizeof(config.openai_key)) &&
        json_field(&j, 0, "transportSeed", config.seed, sizeof(config.seed));
    config.hub_port = json_int(&j, json_get(&j, 0, "hubPort"));
    config.hub_tls = json_is(&j, json_get(&j, 0, "hubTLS"), "true");
    config.use_tailnet = json_is(&j, json_get(&j, 0, "useTailnet"), "true");
    json_free(&j);
    memset(bytes, 0, sizeof(bytes));
    seed_ready = ok && ws_initialize(config.seed);
    return seed_ready && settings_initialize() &&
        (!settings_ready() || !config.use_tailnet || tailnet_device_initialize());
}
static void permission(bool allowed, void *context) {
    (void)context;
    permission_pending = false;
    if (allowed) permission_step++;
    else snprintf(error, sizeof(error), "Access denied. Allow permissions in Settings, then reopen Evener.");
}
static void text(const char *value, int x, int y, int width, int height) {
    pd->graphics->drawTextInRect(value, strlen(value), kUTF8Encoding, x, y, width, height, kWrapWord, kAlignTextLeft);
}
static void reset_scroll(void) { follow = true; scroll = 0; crank_remainder = 0; }
static void reset_profile(void) {
    hub_reset_profile(); selection = 0; reset_scroll();
    saved_audio = recording_started = final_saved = false;
}
static void refresh_saved_audio(void) { saved_audio = recording_exists(draft.ref); }
static void discard(void *context) {
    (void)context;
    if (pairing_active()) return;
    if (draft.state == DRAFT_SENDING || draft.state == DRAFT_UNCERTAIN) {
        snprintf(hub_status, sizeof(hub_status), "Delivery is being checked. Draft kept.");
        return;
    }
    if (selected_ref[0] && draft_clear()) {
        voice_discard();
        refresh_saved_audio();
        if (voice_state == VOICE_ERROR) return;
        recording_started = final_saved = false;
        screen = SCREEN_TRANSCRIPT; reset_scroll();
    }
}
static bool save_voice(void);
static void latest(void *context) {
    (void)context;
    if (pairing_active()) return;
    if (selected_ref[0]) {
        if (recording_started) {
            voice_finish();
            if (!save_voice()) return;
            voice_close(); recording_started = false;
        }
        refresh_saved_audio();
        screen = SCREEN_TRANSCRIPT; hub_latest(); reset_scroll();
    }
}
static void reconnect(void *context) {
    (void)context;
    if (config.use_tailnet) { hub_close(); tailnet_device_stop(); hub_connect(); }
    else hub_recheck();
    if (screen == SCREEN_COMPOSER && recording_started) voice_connect();
}
static void receive_setup(void) {
    if (!seed_ready || pairing_active() || permission_pending) return;
    if (recording_started && !save_voice()) return;
    if (!voice_terminate() || (draft.ref[0] && !draft_save())) {
        snprintf(hub_status, sizeof(hub_status), "Saving your draft. Try setup again in a moment."); return;
    }
    recording_started = false; hub_close();
    if (config.use_tailnet) tailnet_device_stop();
    before_pairing = screen;
    if (!pairing_begin()) { snprintf(hub_status, sizeof(hub_status), "Finish the microphone prompt, then retry setup."); hub_connect(); }
}
static void network_action(void *context) {
    (void)context;
    int choice = pd->system->getMenuItemValue(network_menu);
    pd->system->setMenuItemValue(network_menu, 0);
    if (choice == 1) receive_setup();
    else if ((choice == 2 || choice == 3) && !pairing_active()) {
        if (recording_started && !save_voice()) return;
        if (!voice_terminate()) return;
        if (choice == 3 && draft.ref[0] && !draft_save()) {
            snprintf(hub_status, sizeof(hub_status), "Saving your draft. Try changing tailnets again in a moment."); return;
        }
        recording_started = false;
        screen = SCREEN_AUTH; change_tailnet = choice == 3 && config.use_tailnet;
    }
    else if (!pairing_active() && settings_ready()) reconnect(NULL);
}
static void draw_authorization(void) {
    pd->graphics->clear(kColorWhite); pd->graphics->setFont(small_font);
    pd->graphics->fillRect(0, 0, 400, 21, kColorBlack);
    pd->graphics->setDrawMode(kDrawModeFillWhite); text("Tailscale sign-in", 8, 4, 384, 16);
    pd->graphics->setDrawMode(kDrawModeCopy);
    if (change_tailnet) {
        text(error[0] ? error : "Join a different tailnet?\n\nThis creates a new Playdate identity. Saved messages and drafts stay on the device.\n\nThe old identity remains in Tailscale until you remove it there.", 12, 35, 376, 165);
        text("A New identity    B Cancel", 8, 223, 384, 16); return;
    }
    const char *url = tailnet_device_auth_url();
    if (url[0] && pairing_draw_qr(url, 5, 31, 184)) {
        text("Scan with your phone.\n\nSign in and choose the tailnet with your Hub.\n\nThis screen updates when approval finishes.", 199, 35, 193, 151);
    } else text(config.use_tailnet ? tailnet_device_status() : "This connection uses Wi-Fi directly. Receive setup to choose Tailscale.", 12, 40, 376, 140);
    text(tailnet_device_status(), 12, 193, 376, 27);
    text("B Sessions    A Check again", 8, 223, 384, 16);
}
static bool save_voice(void) {
    bool complete = voice_state == VOICE_REVIEW;
    if (!draft_set(voice_text, complete)) {
        // draft_set retains the newest text in RAM even when its disk commit
        // fails. It must remain partial until the final commit succeeds.
        draft.state = draft.text[0] ? DRAFT_PARTIAL : DRAFT_EMPTY;
        snprintf(hub_status, sizeof(hub_status), "Could not save your draft. Keep Evener open.");
        refresh_saved_audio();
        return false;
    }
    if (complete) voice_saved();
    refresh_saved_audio();
    return true;
}
static void compose(void) {
    screen = SCREEN_COMPOSER; reset_scroll(); recording_started = final_saved = false;
    bool recovered = voice_recover();
    if (recovered) recording_started = voice_state != VOICE_ERROR;
    else if (draft.state == DRAFT_EMPTY) recording_started = voice_start();
    refresh_saved_audio();
    saved_at = app_time(); save_retry_at = 0;
}
static void line(LCDFont *face, const char *value, int x, int y, int width) {
    char short_text[512];
    size_t n = utf8_copy(short_text, sizeof(short_text), value, strlen(value));
    bool shortened = pd->graphics->getTextWidth(face, short_text, n, kUTF8Encoding, 0) > width;
    int ellipsis = pd->graphics->getTextWidth(face, "...", 3, kASCIIEncoding, 0);
    while (shortened && n && pd->graphics->getTextWidth(face, short_text, n, kUTF8Encoding, 0) > width - ellipsis) {
        do { n--; } while (n && ((unsigned char)short_text[n] & 0xc0) == 0x80);
        short_text[n] = 0; shortened = true;
    }
    if (shortened && n + 4 < sizeof(short_text)) strcat(short_text, "...");
    pd->graphics->setFont(face);
    pd->graphics->drawText(short_text, strlen(short_text), kUTF8Encoding, x, y);
}
static void footer(const char *primary, const char *secondary) {
    const char *notice = config.use_tailnet ? tailnet_device_notice() : "";
    if (notice[0]) secondary = notice;
    pd->graphics->clearClipRect();
    pd->graphics->fillRect(0, FOOTER_TOP, 400, 240 - FOOTER_TOP, kColorWhite);
    pd->graphics->drawLine(MARGIN, FOOTER_TOP, 400 - MARGIN, FOOTER_TOP, 1, kColorBlack);
    int detail_width = pd->graphics->getTextWidth(small_font, secondary, strlen(secondary), kUTF8Encoding, 0);
    int hint_width = pd->graphics->getTextWidth(small_font, primary, strlen(primary), kUTF8Encoding, 0);
    if (detail_width > 100 && hint_width + detail_width + 12 > 384) {
        // A recovery instruction gets the whole line instead of being clipped
        // behind the controls that remain available throughout recovery.
        line(small_font, secondary, MARGIN, 227, 384);
    } else {
        line(small_font, primary, MARGIN, 227, 384 - detail_width - 12);
        line(small_font, secondary, 392 - detail_width, 227, detail_width);
    }
    pd->graphics->setFont(font);
}
static void bubble(const char *value, int x, int y, int width, int height, bool user, bool clipped) {
    pd->graphics->fillRoundRect(x, y, width, height, 5, user ? kColorBlack : kColorWhite);
    if (!user) pd->graphics->drawRoundRect(x, y, width, height, 5, 1, kColorBlack);
    pd->graphics->setDrawMode(user ? kDrawModeFillWhite : kDrawModeCopy);
    pd->graphics->setFont(font);
    text(value, x + 8, y + 6, width - 16, height - 12);
    if (clipped) {
        pd->graphics->setFont(small_font);
        text("Message exceeds the device limit.\nOpen Evener for the complete text.", x + 8, y + height - 32, width - 16, 26);
    }
    pd->graphics->setFont(font);
    pd->graphics->setDrawMode(kDrawModeCopy);
}
static void measure_chat(void) {
    if (measured_revision == chat_revision) return;
    char anchor[192] = ""; int anchor_offset = 0;
    if (!follow && measured_revision >= 0) {
        for (int i = 0; i < measured_count; i++) if (row_y[i] + row_height[i] > scroll) {
            // The UI retains the same text position when older rows are prepended.
            memcpy(anchor, measured_keys[i], sizeof(anchor)); anchor_offset = scroll - row_y[i]; break;
        }
    }
    content_height = 0;
    for (int i = 0; i < chat_count; i++) {
        row_y[i] = content_height;
        memcpy(measured_keys[i], chat[i].key, sizeof(measured_keys[i]));
        const char *value = chat[i].text ? chat[i].text : "";
        int width = pd->graphics->getTextWidth(font, value, strlen(value), kUTF8Encoding, 0) + 16;
        row_width[i] = width > BUBBLE_WIDTH || chat[i].clipped ? BUBBLE_WIDTH : width < 64 ? 64 : width;
        row_height[i] = 12 + pd->graphics->getTextHeightForMaxWidth(font, value, strlen(value), row_width[i] - 16, kUTF8Encoding, kWrapWord, 0, 0) + (chat[i].clipped ? 26 : 0);
        content_height += row_height[i] + 8;
        if (anchor[0] && !strcmp(anchor, chat[i].key)) scroll = row_y[i] + anchor_offset;
    }
    measured_revision = chat_revision;
    measured_count = chat_count;
}
static void draw(void) {
    pd->graphics->clear(kColorWhite);
    pd->graphics->setFont(font);
    const char *heading = screen == SCREEN_SESSIONS ?
        (browse_kind == BROWSE_LIVE ? "Evener / Live" : browse_kind == BROWSE_PROJECTS ? "Evener / Projects" : browse_title) :
        screen == SCREEN_COMPOSER ?
            (voice_state == VOICE_RECORDING ? "Recording" : voice_state == VOICE_FINISHING ? "Transcribing audio" :
                voice_state == VOICE_ERROR ? "Dictation paused" : draft.state == DRAFT_EMPTY ? "Dictate message" : "Review message") : selected_title;
    pd->graphics->fillRect(0, 0, 400, 20, kColorBlack);
    pd->graphics->setDrawMode(kDrawModeFillWhite);
    line(font, heading, MARGIN, 2, 384);
    pd->graphics->setDrawMode(kDrawModeCopy);
    if (error[0] || !permissions_ready) {
        text(error[0] ? error : "Preparing microphone and network access...", MARGIN, CONTENT_TOP, 384, CONTENT_HEIGHT);
        return;
    }
    if (screen == SCREEN_SESSIONS) {
        int total = navigation_count + (navigation_remaining > 0);
        if (selection >= total) selection = total ? total - 1 : 0;
        int start = selection / SESSION_ROWS * SESSION_ROWS;
        for (int i = start; i < total && i < start + SESSION_ROWS; i++) {
            int y = CONTENT_TOP + (i - start) * 30;
            bool picked = i == selection;
            if (picked) { pd->graphics->fillRect(4, y - 1, 392, 29, kColorBlack); pd->graphics->setDrawMode(kDrawModeFillWhite); }
            line(font, i < navigation_count ? navigation[i].title : "More sessions...", MARGIN, y, 384);
            line(small_font, i < navigation_count ? navigation[i].state : "Next page", MARGIN, y + 16, 384);
            pd->graphics->setFont(font); pd->graphics->setDrawMode(kDrawModeCopy);
        }
        if (!total) text(hub_loading ? "Loading..." : "No sessions in this view.", MARGIN, 40, 384, 80);
        const char *tabs[] = {"Current", "Recent", "Archived"};
        char hint[128];
        snprintf(hint, sizeof(hint), "A Open  B Back  L/R %s", browse_kind == BROWSE_PROJECT ? tabs[browse_tier] : "Live / Projects");
        footer(hint, hub_status);
    } else if (screen == SCREEN_TRANSCRIPT) {
        measure_chat();
        int bottom = content_height > CONTENT_HEIGHT ? content_height - CONTENT_HEIGHT : 0;
        if (follow) scroll = bottom;
        if (scroll < 0) scroll = 0;
        if (scroll > bottom) scroll = bottom;
        pd->graphics->setClipRect(MARGIN, CONTENT_TOP, 384, CONTENT_HEIGHT);
        if (!chat_count) text(hub_loading ? "Loading conversation..." :
            draft.state == DRAFT_EMPTY && saved_audio ? "No chat messages yet.\n\nPress A to transcribe your saved audio." :
            draft.state != DRAFT_EMPTY ? "No chat messages yet.\n\nPress A to review your saved draft." :
            "No chat messages yet.\n\nHold A to dictate a message.", MARGIN, CONTENT_TOP, 378, CONTENT_HEIGHT);
        for (int i = 0; i < chat_count; i++) {
            int y = CONTENT_TOP + row_y[i] - scroll;
            if (y + row_height[i] < CONTENT_TOP || y >= CONTENT_TOP + CONTENT_HEIGHT) continue;
            const char *value = chat[i].text ? chat[i].text : "";
            int x = chat[i].user ? 392 - row_width[i] : MARGIN;
            bubble(value, x, y, row_width[i], row_height[i], chat[i].user, chat[i].clipped);
        }
        footer(draft.state == DRAFT_EMPTY ? (saved_audio ? "A Transcribe saved audio  B Sessions" : "Hold A Dictate  B Sessions  Crank Scroll") :
            "A Review draft  B Sessions", hub_hydrated ? session_status : hub_status);
    } else {
        const char *body = voice_state == VOICE_ERROR ? voice_status : recording_started ? voice_text : draft.text;
        if (!body[0]) body = voice_state == VOICE_RECORDING ? "Listening now...\n\nRelease A to review before sending." :
            voice_state == VOICE_FINISHING ? "Audio captured.\n\nWaiting for transcription.\n\nKeep Evener open while audio saves.\nMenu > Discard draft removes it." : "Hold A and speak.\nRelease A to review before sending.";
        int height = pd->graphics->getTextHeightForMaxWidth(font, body, strlen(body), 384, kUTF8Encoding, kWrapWord, 0, 0);
        int bottom = height > CONTENT_HEIGHT ? height - CONTENT_HEIGHT : 0;
        if (follow) scroll = bottom;
        if (scroll < 0) scroll = 0;
        if (scroll > bottom) scroll = bottom;
        pd->graphics->setClipRect(MARGIN, CONTENT_TOP, 384, CONTENT_HEIGHT);
        text(body, MARGIN, CONTENT_TOP - scroll, 384, height + 16);
        const char *hint = voice_state == VOICE_RECORDING ? "Listening... Release A to review" :
            voice_state == VOICE_FINISHING ? "Transcribing...  B Keep  Menu Discard" :
            recording_started && voice_state == VOICE_REVIEW && !final_saved ? "Saving review..." :
            draft.state == DRAFT_READY ? "A Send  B Keep draft  Menu Discard" :
            draft.state == DRAFT_UNCERTAIN || draft.state == DRAFT_SENDING ? "Checking delivery...  B Conversation" :
            draft.state == DRAFT_PARTIAL ? "Partial saved  Menu Discard to retry" : "Hold A Dictate  B Conversation";
        footer(hint, recording_started || voice_state == VOICE_ERROR ? voice_status : hub_status);
    }
}
static int update(void *context) {
    (void)context;
    PDButtons held, pressed, released;
    pd->system->getButtonState(&held, &pressed, &released);
    if (pairing_active()) {
        net_tick();
        enum PairingResult result = pairing_update(pressed);
        if (result == PAIRING_RUNNING) { pairing_draw(small_font); return 1; }
        if (result == PAIRING_APPLIED) {
            reset_profile(); screen = config.use_tailnet ? SCREEN_AUTH : SCREEN_SESSIONS;
            error[0] = 0;
            if (config.use_tailnet && !tailnet_device_initialize())
                snprintf(error, sizeof(error), "Could not save Tailscale identity. Reopen Evener to retry.");
        } else {
            screen = before_pairing;
            if (config.use_tailnet && !tailnet_device_initialize())
                snprintf(error, sizeof(error), "Could not restore Tailscale connection. Reopen Evener to retry.");
        }
        permissions_ready = permission_pending = false; permission_step = 0;
        pressed = released = 0;
    }
    if (!settings_ready()) {
        if (setup_requested || (pressed & kButtonA)) { setup_requested = false; receive_setup(); }
        if (pairing_active()) pairing_draw(small_font);
        else {
            pd->graphics->clear(kColorWhite); pd->graphics->setFont(small_font);
            text(error[0] ? error : "Set up Evener\n\nOpen Evener Settings on your phone or computer, then press A here to receive setup.", 12, 38, 376, 170);
        }
        return 1;
    }
    if (!error[0] && !permissions_ready && !permission_pending) {
        enum accessReply reply = kAccessAsk;
        permission_pending = true;
        if (permission_step == 0) reply = pd->sound->requestMicAccess("Dictate messages for Evener.", permission, NULL);
        else if (permission_step == 1) reply = pd->network->tcp->requestAccess("api.openai.com", 443, true, "Transcribe your voice directly.", permission, NULL);
        else if (permission_step == 2 && !config.use_tailnet) reply = pd->network->tcp->requestAccess(config.hub_host, config.hub_port, config.hub_tls, "Read and send Evener messages.", permission, NULL);
        else { permissions_ready = true; permission_pending = false; hub_connect(); }
        if (reply != kAccessAsk) permission(reply == kAccessAllow, NULL);
    }
    if (permissions_ready) {
        if (screen == SCREEN_AUTH) {
            if (change_tailnet) {
                if (pressed & kButtonB) {
                    if (!error[0] || tailnet_device_initialize()) {
                        if (error[0]) { error[0] = 0; reset_profile(); hub_connect(); }
                        change_tailnet = false; screen = SCREEN_SESSIONS;
                    }
                }
                else if ((pressed & kButtonA) && config.use_tailnet) {
                    hub_close();
                    if (tailnet_device_new_identity()) {
                        error[0] = 0; change_tailnet = false; reset_profile(); hub_connect();
                    }
                    else snprintf(error, sizeof(error), "Could not finish saving the new identity. Press A to retry. Your drafts are kept.");
                }
                draw_authorization(); return 1;
            }
            if (config.use_tailnet) tailnet_device_update();
            hub_update();
            if ((pressed & kButtonB) || hub_ready) screen = SCREEN_SESSIONS;
            else {
                if (pressed & kButtonA) reconnect(NULL);
                draw_authorization(); return 1;
            }
        }
        if ((pressed & kButtonA) && (screen == SCREEN_TRANSCRIPT ||
            (screen == SCREEN_COMPOSER && draft.state == DRAFT_EMPTY && voice_state != VOICE_FINISHING))) {
            compose();
            pressed &= ~kButtonA;
        }
        bool send_review = screen == SCREEN_COMPOSER && (pressed & kButtonA) &&
            draft.state == DRAFT_READY && (!recording_started || final_saved) &&
            voice_state != VOICE_RECORDING && voice_state != VOICE_FINISHING;
        if (!(held & kButtonA) && voice_state == VOICE_RECORDING) voice_finish();
        char focused[REF_CAPACITY] = "";
        if (screen == SCREEN_SESSIONS && selection < navigation_count)
            memcpy(focused, navigation[selection].ref, sizeof(focused));
        int nav_revision = navigation_revision;
        enum DraftState before = draft.state;
        if (config.use_tailnet) {
            tailnet_device_set_voice_active(voice_state == VOICE_RECORDING || voice_state == VOICE_FINISHING);
            tailnet_device_update();
        }
        hub_update();
        if (before != DRAFT_EMPTY && draft.state == DRAFT_EMPTY) refresh_saved_audio();
        if (screen == SCREEN_COMPOSER && (before == DRAFT_SENDING || before == DRAFT_UNCERTAIN) && draft.state == DRAFT_EMPTY) {
            screen = SCREEN_TRANSCRIPT; reset_scroll();
        }
        if (nav_revision != navigation_revision && focused[0])
            for (int i = 0; i < navigation_count; i++) if (!strcmp(focused, navigation[i].ref)) { selection = i; break; }
        voice_update();
        if (screen == SCREEN_COMPOSER && recording_started && app_time() >= save_retry_at &&
            (voice_state == VOICE_REVIEW || voice_state == VOICE_ERROR || (!draft.text[0] && voice_text[0]) || app_time() - saved_at > 1) && !final_saved) {
            bool complete = voice_state == VOICE_REVIEW;
            if (!save_voice()) {
                save_retry_at = app_time() + 1;
            }
            else {
                final_saved = complete;
            }
            saved_at = app_time();
        }
        if (final_saved && voice_state == VOICE_REVIEW) voice_saved();
        if (pressed & kButtonA) {
            if (screen == SCREEN_SESSIONS) {
                if (selection == navigation_count && navigation_remaining > 0) {
                    hub_browse(browse_kind, NULL, NULL, browse_tier, navigation_offset + NAV_PAGE_SIZE); selection = 0;
                } else if (selection < navigation_count) {
                    if (browse_kind == BROWSE_PROJECTS) {
                        NavigationRow row = navigation[selection];
                        hub_browse(BROWSE_PROJECT, row.ref, row.title, 0, 0); selection = 0;
                    } else if (hub_select(&navigation[selection])) {
                        refresh_saved_audio();
                        screen = SCREEN_TRANSCRIPT; measured_revision = -1; reset_scroll();
                    }
                }
            } else if (send_review) {
                recording_started = false; voice_close(); hub_send();
            }
        }
        if (pressed & kButtonB) {
            if (screen == SCREEN_COMPOSER) {
                if (voice_state == VOICE_RECORDING) voice_finish();
                else {
                    if (recording_started && !save_voice()) {
                        draw(); return 1;
                    }
                    voice_close(); refresh_saved_audio(); screen = SCREEN_TRANSCRIPT; recording_started = false; reset_scroll();
                }
            } else if (screen == SCREEN_TRANSCRIPT) { screen = SCREEN_SESSIONS; hub_browse(browse_kind, NULL, NULL, browse_tier, navigation_offset); }
            else if (navigation_offset > 0) { hub_browse(browse_kind, NULL, NULL, browse_tier, navigation_offset - NAV_PAGE_SIZE); selection = 0; }
            else if (browse_kind == BROWSE_PROJECT) { hub_browse(BROWSE_PROJECTS, NULL, NULL, 0, 0); selection = 0; }
        }
        if (screen == SCREEN_SESSIONS && (pressed & (kButtonLeft | kButtonRight))) {
            int direction = pressed & kButtonLeft ? -1 : 1;
            if (browse_kind == BROWSE_PROJECT) hub_browse(BROWSE_PROJECT, NULL, NULL, (browse_tier + direction + 3) % 3, 0);
            else hub_browse(browse_kind == BROWSE_LIVE ? BROWSE_PROJECTS : BROWSE_LIVE, NULL, NULL, 0, 0);
            selection = 0;
        }
    }
    float crank = pd->system->getCrankChange();
    PDButtons direction = pressed & (kButtonUp | kButtonDown);
    if (direction) repeat_at = app_time() + .35f;
    else if ((held & (kButtonUp | kButtonDown)) && app_time() > repeat_at) { direction = held & (kButtonUp | kButtonDown); repeat_at = app_time() + .08f; }
    if (crank != 0 || direction) {
        crank_remainder += crank;
        if (screen == SCREEN_SESSIONS) {
            int move = (int)(crank_remainder / 18); crank_remainder -= move * 18;
            selection += move + ((direction & kButtonDown) ? 1 : (direction & kButtonUp) ? -1 : 0);
            if (selection < 0) selection = 0;
        } else {
            follow = false;
            int move = (int)crank_remainder; crank_remainder -= move;
            scroll += move + ((direction & kButtonDown) ? 30 : (direction & kButtonUp) ? -30 : 0);
            if (screen == SCREEN_TRANSCRIPT && scroll <= 0) hub_older();
            if (screen == SCREEN_TRANSCRIPT && scroll >= content_height - CONTENT_HEIGHT && (move > 0 || (direction & kButtonDown))) follow = true;
        }
    }
    draw();
    return 1;
}
#ifdef _WINDLL
__declspec(dllexport)
#endif
int eventHandler(PlaydateAPI *playdate, PDSystemEvent event, uint32_t arg) {
    (void)arg;
    if (pd && (event == kEventLock || event == kEventPause || event == kEventTerminate ||
        event == kEventResume || event == kEventUnlock))
        app_log("Application lifecycle event %u at %.2f seconds", (unsigned)event, (double)app_time());
    if (event == kEventInit) {
        pd = playdate;
        const char *reason = NULL;
        font = pd->graphics->loadFont("/System/Fonts/Roobert-10-Bold.pft", &reason);
        small_font = pd->graphics->loadFont("fonts/Nontendo-Light", &reason);
        pd->display->setRefreshRate(30);
        pd->system->setUpdateCallback(update, NULL);
        pd->system->addMenuItem("Discard draft", discard, NULL);
        pd->system->addMenuItem("Latest messages", latest, NULL);
        static const char *network_options[] = {"Reconnect", "Receive setup", "Tailscale sign-in", "Change tailnet"};
        network_menu = pd->system->addOptionsMenuItem("Network", network_options, 4, network_action, NULL);
        if (!read_configuration()) snprintf(error, sizeof(error), "Device configuration is missing or could not be read.");
        else setup_requested = !settings_ready();
    } else if (event == kEventLock || event == kEventPause || event == kEventTerminate) {
        if (pairing_active()) pairing_pause();
        if (recording_started) save_voice();
        if (event == kEventTerminate) voice_terminate();
        else voice_close();
        refresh_saved_audio();
        recording_started = false;
        hub_close();
        if (config.use_tailnet) tailnet_device_stop();
    } else if (event == kEventResume || event == kEventUnlock) {
        if (pairing_active()) { pairing_resume(); return 0; }
        if (!permissions_ready) return 0;
        if (screen == SCREEN_COMPOSER) recording_started = voice_recover() && voice_state != VOICE_ERROR;
        refresh_saved_audio();
        hub_connect();
    }
    return 0;
}

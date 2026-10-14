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
static LCDFont *font, *small_font, *body_fonts[3], *measured_font;
static LCDBitmap *canvas;
enum { MARGIN = 8, CONTENT_TOP = 24, CONTENT_HEIGHT = 196, FOOTER_TOP = 224, BUBBLE_WIDTH = 374 };
enum Screen { SCREEN_SESSIONS, SCREEN_TRANSCRIPT, SCREEN_COMPOSER, SCREEN_AUTH };
static enum Screen screen;
static int permission_step, scroll, selection, content_height;
static int row_y[CHAT_CAPACITY], row_height[CHAT_CAPACITY], row_width[CHAT_CAPACITY], measured_revision = -1;
static char measured_keys[CHAT_CAPACITY][192];
static int measured_count;
static LCDFont *composer_font;
static int composer_height;
static size_t row_text_length[CHAT_CAPACITY];
static float crank_remainder;
static bool permission_pending, permissions_ready, follow = true;
static bool recording_started, final_saved, saved_audio;
static float saved_at, save_retry_at, repeat_at;
static char error[256];
static PDMenuItem *font_menu;
static bool actions_open;
static int action_selection, font_size;
static enum Screen before_pairing;
static bool seed_ready, setup_requested, change_tailnet;
static float network_restart_at;
static bool network_restarting;

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
    if (network_restarting) return;
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
    if (network_restarting || pairing_active()) return;
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
    if (network_restarting || pairing_active()) return;
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
static void network_action(int choice) {
    if (network_restarting) return;
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
static void show_actions(void *context) {
    (void)context;
    if (network_restarting || pairing_active()) return;
    actions_open = true; action_selection = 0; crank_remainder = 0;
}
static int read_font_size(const char *path) {
    unsigned char record[9];
    SDFile *file = pd->file->open(path, kFileReadData);
    if (!file) return -1;
    int count = pd->file->read(file, record, sizeof(record));
    pd->file->close(file);
    return count == 8 && !memcmp(record, "EVFONT1", 7) && record[7] < 3 ? record[7] : -1;
}
static bool save_font_size(int choice) {
    // A complete temporary record remains readable if device rename must wait
    // until the next launch. Preferences apply to this device across Hubs.
    unsigned char record[8] = "EVFONT1";
    record[7] = (unsigned char)choice;
    SDFile *file = pd->file->open("reading-font.tmp", kFileWrite);
    if (!file) return false;
    bool ok = pd->file->write(file, record, sizeof(record)) == sizeof(record) && pd->file->flush(file) >= 0;
    if (pd->file->close(file)) ok = false;
    if (ok) storage_replace("reading-font.tmp", "reading-font.bin");
    return ok;
}
static bool select_font(int choice) {
    static const char *paths[] = {"/System/Fonts/Roobert-10-Bold.pft", "/System/Fonts/Roobert-11-Bold.pft", "/System/Fonts/Roobert-20-Medium-extended.pft"};
    if (!body_fonts[choice]) {
        const char *reason = NULL;
        body_fonts[choice] = pd->graphics->loadFont(paths[choice], &reason);
        if (!body_fonts[choice]) return false;
    }
    font_size = choice; font = body_fonts[choice];
    return true;
}
static void font_action(void *context) {
    (void)context;
    if (network_restarting) return;
    int choice = pd->system->getMenuItemValue(font_menu);
    int previous = font_size;
    if (!select_font(choice)) {
        snprintf(hub_status, sizeof(hub_status), "Could not load that font.");
        pd->system->setMenuItemValue(font_menu, previous);
    } else if (!save_font_size(choice)) {
        snprintf(hub_status, sizeof(hub_status), "Font changed. Could not save it for next time.");
    }
    crank_remainder = 0;
}
static void initialize_reader(void) {
    const char *reason = NULL;
    select_font(0);
    small_font = pd->graphics->loadFont("fonts/Nontendo-Light", &reason);
    int saved = read_font_size("reading-font.tmp");
    if (saved >= 0) storage_replace("reading-font.tmp", "reading-font.bin");
    else saved = read_font_size("reading-font.bin");
    if (saved >= 0) select_font(saved);
    static const char *font_options[] = {"Small", "Medium", "Large"};
    font_menu = pd->system->addOptionsMenuItem("Font", font_options, 3, font_action, NULL);
    pd->system->setMenuItemValue(font_menu, font_size);
    pd->system->addMenuItem("Latest", latest, NULL);
    pd->system->addMenuItem("Actions", show_actions, NULL);
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
static void restore_network_context(void) {
    const char *args = pd->system->getLaunchArgs(NULL);
    Json j;
    if (!args || strlen(args) > 10000 || !json_parse(&j, args, strlen(args))) return;
    static NavigationRow row;
    static char project[REF_CAPACITY], title[160], snapshot[128];
    int view = json_int(&j, json_get(&j, 0, "screen"));
    int kind = json_int(&j, json_get(&j, 0, "browse"));
    int tier = json_int(&j, json_get(&j, 0, "tier"));
    int offset = json_int(&j, json_get(&j, 0, "offset"));
    int picked = json_int(&j, json_get(&j, 0, "selection"));
    int position = json_int(&j, json_get(&j, 0, "scroll"));
    bool valid = json_is(&j, json_get(&j, 0, "networkRecovery"), "1") &&
        view >= SCREEN_SESSIONS && view <= SCREEN_AUTH && kind >= BROWSE_LIVE && kind <= BROWSE_PROJECT &&
        tier >= 0 && tier <= 2 && offset >= 0 && picked >= 0 && picked <= NAV_PAGE_SIZE && position >= 0 &&
        json_field(&j, 0, "ref", row.ref, sizeof(row.ref)) &&
        json_field(&j, 0, "title", row.title, sizeof(row.title)) &&
        json_field(&j, 0, "project", project, sizeof(project)) &&
        json_field(&j, 0, "projectTitle", title, sizeof(title)) &&
        json_field(&j, 0, "snapshot", snapshot, sizeof(snapshot));
    bool resume_send = json_is(&j, json_get(&j, 0, "send"), "true");
    bool resume_follow = json_is(&j, json_get(&j, 0, "follow"), "true");
    json_free(&j);
    if (!valid) return;
    hub_browse(kind, project, title, tier, offset);
    if (row.ref[0] && !hub_restore_view(&row, snapshot)) return;
    if ((view == SCREEN_TRANSCRIPT || view == SCREEN_COMPOSER) && !row.ref[0]) return;
    screen = view; selection = picked; scroll = position; follow = resume_follow;
    if (screen == SCREEN_COMPOSER) recording_started = voice_recover() && voice_state != VOICE_ERROR;
    refresh_saved_audio();
    // An already dispatched mutation keeps its persisted identity and reconciles
    // through the authoritative thread read. Only a pre-dispatch request needs
    // its in-memory send intent restored here.
    if (resume_send) hub_send();
    app_log("Network recovery restored view %d; selected %d; draft %d", screen, row.ref[0] != 0, draft.state);
}
static bool recover_network(void) {
    if (!net_recovery_needed() || app_time() < network_restart_at || pairing_active() || change_tailnet ||
        voice_state == VOICE_RECORDING || !voice_audio_saved()) return false;
    // Keep the SDK's existing two TLS slots: DERP and transcription. Relaunch
    // attempts recovery without recycling an object the worker still owns.
    static char ref[REF_CAPACITY * 6 + 3], title[160 * 6 + 3];
    static char project[REF_CAPACITY * 6 + 3], project_title[160 * 6 + 3], args[10000];
    static char snapshot[128], quoted_snapshot[128 * 6 + 3];
    network_restart_at = app_time() + 1;
    if (!hub_save_view(snapshot, sizeof(snapshot)) || !json_quote(snapshot, quoted_snapshot, sizeof(quoted_snapshot))) return false;
    if (!json_quote(selected_ref, ref, sizeof(ref)) || !json_quote(selected_title, title, sizeof(title)) ||
        !json_quote(browse_project, project, sizeof(project)) || !json_quote(browse_title, project_title, sizeof(project_title))) return false;
    int length = snprintf(args, sizeof(args),
        "{\"networkRecovery\":1,\"screen\":%d,\"ref\":%s,\"title\":%s,\"browse\":%d,\"project\":%s,"
        "\"projectTitle\":%s,\"snapshot\":%s,\"tier\":%d,\"offset\":%d,\"selection\":%d,\"scroll\":%d,\"follow\":%s,\"send\":%s}",
        screen, ref, title, browse_kind, project, project_title, quoted_snapshot, browse_tier, navigation_offset, selection,
        scroll > 0 ? scroll : 0, follow ? "true" : "false", hub_send_pending() ? "true" : "false");
    if (length < 0 || length >= sizeof(args)) return false;
    if (recording_started && !save_voice()) return false;
    if ((!recording_started && draft.ref[0] && !draft_save()) || !voice_prepare_restart()) {
        snprintf(hub_status, sizeof(hub_status), "Saving your work before reconnecting..."); return false;
    }
    recording_started = false;
    network_restarting = true;
    app_log("Network recovery restarting app after SDK cleanup stalled");
    pd->system->restartGame(args);
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
static const char *body_text(const char *value) {
    if (font_size != 2 || !strstr(value, "\xe2\x80")) return value;
    // The large SDK font omits typographic dashes and quotes. Use plain
    // equivalents for display while retaining the original transcript/draft.
    static char display[32769];
    size_t source = 0, target = 0;
    while (value[source] && target < sizeof(display) - 1) {
        unsigned char mark = (unsigned char)value[source];
        char replacement = 0;
        if (mark == 0xe2 && (unsigned char)value[source + 1] == 0x80) {
            unsigned char punctuation = (unsigned char)value[source + 2];
            if (punctuation == 0x93 || punctuation == 0x94) replacement = '-';
            else if (punctuation == 0x98 || punctuation == 0x99) replacement = '\'';
            else if (punctuation == 0x9c || punctuation == 0x9d) replacement = '"';
        }
        display[target++] = replacement ? replacement : value[source];
        source += replacement ? 3 : 1;
    }
    display[target] = 0;
    return display;
}
static size_t display_length(const char *value) {
    size_t end = strlen(value), characters = 0;
    while (end && (value[end - 1] == '\n' || value[end - 1] == '\r' || value[end - 1] == ' ' || value[end - 1] == '\t')) end--;
    // The SDK length is a count of Unicode codepoints, not UTF-8 bytes.
    for (size_t i = 0; i < end; i++) if (((unsigned char)value[i] & 0xc0) != 0x80) characters++;
    return characters;
}
static void spinner(int x, int y, int radius, LCDColor color) {
    static const int dx[] = {0, 7, 10, 7, 0, -7, -10, -7};
    static const int dy[] = {-10, -7, 0, 7, 10, 7, 0, -7};
    int phase = (int)(app_time() * 8) % 8;
    for (int i = 0; i < 8; i++) {
        int size = (i - phase + 8) % 8 < 3 ? 3 : 1;
        pd->graphics->fillEllipse(x + dx[i] * radius / 10 - size / 2,
            y + dy[i] * radius / 10 - size / 2, size, size, 0, 360, color);
    }
}
static void draw_actions(void) {
    static const char *labels[] = {"Discard draft", "Reconnect", "Receive setup", "Tailscale sign-in", "Change tailnet", "Back"};
    pd->graphics->clear(kColorWhite);
    pd->graphics->fillRect(0, 0, 400, 20, kColorBlack);
    pd->graphics->setDrawMode(kDrawModeFillWhite);
    line(body_fonts[0], "Actions", MARGIN, 2, 384);
    pd->graphics->setDrawMode(kDrawModeCopy);
    for (int i = 0; i < 6; i++) {
        int y = CONTENT_TOP + i * 30;
        if (i == action_selection) {
            pd->graphics->fillRect(4, y - 1, 392, 27, kColorBlack);
            pd->graphics->setDrawMode(kDrawModeFillWhite);
        }
        line(body_fonts[0], labels[i], MARGIN, y + 5, 384);
        pd->graphics->setDrawMode(kDrawModeCopy);
    }
    footer("A Select  B Back  Crank Choose", "");
}
static void bubble(const char *value, size_t length, int x, int y, int width, int height, bool user, bool clipped) {
    pd->graphics->fillRoundRect(x, y, width, height, 5, user ? kColorBlack : kColorWhite);
    if (!user) pd->graphics->drawRoundRect(x, y, width, height, 5, 1, kColorBlack);
    pd->graphics->setDrawMode(user ? kDrawModeFillWhite : kDrawModeCopy);
    pd->graphics->setFont(font);
    pd->graphics->drawTextInRect(value, length, kUTF8Encoding, x + 8, y + 6, width - 16, height - 12, kWrapWord, kAlignTextLeft);
    if (clipped) {
        pd->graphics->setFont(small_font);
        text("Message exceeds the device limit.\nOpen Evener for the complete text.", x + 8, y + height - 32, width - 16, 26);
    }
    pd->graphics->setFont(font);
    pd->graphics->setDrawMode(kDrawModeCopy);
}
static void measure_chat(void) {
    if (measured_revision == chat_revision && measured_font == font) return;
    char anchor[192] = ""; int anchor_offset = 0, anchor_height = 0;
    if (!follow && measured_revision >= 0) {
        for (int i = 0; i < measured_count; i++) if (row_y[i] + row_height[i] > scroll) {
            // The UI retains the same text position when older rows are prepended.
            memcpy(anchor, measured_keys[i], sizeof(anchor)); anchor_offset = scroll - row_y[i]; anchor_height = row_height[i]; break;
        }
    }
    content_height = 0;
    for (int i = 0; i < chat_count; i++) {
        row_y[i] = content_height;
        memcpy(measured_keys[i], chat[i].key, sizeof(measured_keys[i]));
        const char *value = body_text(chat[i].text ? chat[i].text : "");
        row_text_length[i] = display_length(value);
        int width = pd->graphics->getTextWidth(font, value, row_text_length[i], kUTF8Encoding, 0) + 16;
        row_width[i] = width > BUBBLE_WIDTH || chat[i].clipped ? BUBBLE_WIDTH : width < 64 ? 64 : width;
        row_height[i] = 12 + pd->graphics->getTextHeightForMaxWidth(font, value, row_text_length[i], row_width[i] - 16, kUTF8Encoding, kWrapWord, 0, 0) + (chat[i].clipped ? 26 : 0);
        content_height += row_height[i] + 8;
        if (anchor[0] && !strcmp(anchor, chat[i].key)) {
            if (measured_font != font && anchor_height > 0 && anchor_offset > 0)
                anchor_offset = (int)((int64_t)anchor_offset * row_height[i] / anchor_height);
            scroll = row_y[i] + anchor_offset;
        }
    }
    measured_revision = chat_revision;
    measured_font = font;
    measured_count = chat_count;
}
static void draw(void) {
    if (actions_open) { draw_actions(); return; }
    pd->graphics->clear(kColorWhite);
    pd->graphics->setFont(font);
    const char *heading = screen == SCREEN_SESSIONS ?
        (browse_kind == BROWSE_LIVE ? "Evener / Live" : browse_kind == BROWSE_PROJECTS ? "Evener / Projects" : browse_title) :
        screen == SCREEN_COMPOSER ?
            (voice_state == VOICE_RECORDING ? "Recording" : voice_state == VOICE_FINISHING ? "Transcribing audio" :
                voice_state == VOICE_ERROR ? "Dictation paused" : draft.state == DRAFT_EMPTY ? "Dictate message" : "Review message") : selected_title;
    bool loading = !error[0] && (!permissions_ready || hub_loading || !hub_ready);
    bool empty = screen == SCREEN_SESSIONS ? !navigation_count : screen == SCREEN_TRANSCRIPT ? !chat_count : false;
    pd->graphics->fillRect(0, 0, 400, 20, kColorBlack);
    pd->graphics->setDrawMode(kDrawModeFillWhite);
    line(body_fonts[0], heading, MARGIN, 2, loading && !empty ? 362 : 384);
    if (loading && !empty) spinner(383, 9, 6, kColorWhite);
    pd->graphics->setDrawMode(kDrawModeCopy);
    pd->graphics->setFont(font);
    if (error[0]) { text(error, MARGIN, CONTENT_TOP, 384, CONTENT_HEIGHT); return; }
    if (loading && (empty || !permissions_ready)) {
        spinner(200, 82, 14, kColorBlack);
        const char *phase = !permissions_ready ? "Preparing microphone and network access..." :
            hub_status[0] ? hub_status : "Connecting to Evener...";
        pd->graphics->drawTextInRect(phase, strlen(phase), kUTF8Encoding, MARGIN, 115, 384, 100, kWrapWord, kAlignTextCenter);
        footer(screen == SCREEN_TRANSCRIPT ? "B Sessions" : "A Open  B Back", "");
        return;
    }
    if (screen == SCREEN_SESSIONS) {
        int total = navigation_count + (navigation_remaining > 0);
        if (selection >= total) selection = total ? total - 1 : 0;
        int title_height = pd->graphics->getFontHeight(font);
        int row_size = title_height + pd->graphics->getFontHeight(small_font) + 3;
        int visible_rows = CONTENT_HEIGHT / row_size;
        if (visible_rows < 1) visible_rows = 1;
        int start = selection / visible_rows * visible_rows;
        for (int i = start; i < total && i < start + visible_rows; i++) {
            int y = CONTENT_TOP + (i - start) * row_size;
            bool picked = i == selection;
            if (picked) { pd->graphics->fillRect(4, y - 1, 392, row_size, kColorBlack); pd->graphics->setDrawMode(kDrawModeFillWhite); }
            line(font, body_text(i < navigation_count ? navigation[i].title : "More sessions..."), MARGIN, y, 384);
            line(small_font, i < navigation_count ? navigation[i].state : "Next page", MARGIN, y + title_height + 2, 384);
            pd->graphics->setFont(font); pd->graphics->setDrawMode(kDrawModeCopy);
        }
        if (!total) text("No sessions in this view.", MARGIN, 40, 384, 80);
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
        if (!chat_count) text(draft.state == DRAFT_EMPTY && saved_audio ? "No chat messages yet.\n\nPress A to transcribe your saved audio." :
            draft.state != DRAFT_EMPTY ? "No chat messages yet.\n\nPress A to review your saved draft." :
            "No chat messages yet.\n\nHold A to dictate a message.", MARGIN, CONTENT_TOP, 378, CONTENT_HEIGHT);
        for (int i = 0; i < chat_count; i++) {
            int y = CONTENT_TOP + row_y[i] - scroll;
            if (y + row_height[i] < CONTENT_TOP || y >= CONTENT_TOP + CONTENT_HEIGHT) continue;
            const char *value = body_text(chat[i].text ? chat[i].text : "");
            int x = chat[i].user ? 392 - row_width[i] : MARGIN;
            bubble(value, row_text_length[i], x, y, row_width[i], row_height[i], chat[i].user, chat[i].clipped);
        }
        footer(draft.state == DRAFT_EMPTY ? (saved_audio ? "A Transcribe saved audio  B Sessions" : "Hold A Dictate  B Sessions  Crank Scroll") :
            "A Review draft  B Sessions", loading || !hub_hydrated ? hub_status : session_status);
    } else {
        const char *body = voice_state == VOICE_ERROR ? voice_status : recording_started ? voice_text : draft.text;
        if (!body[0]) body = voice_state == VOICE_RECORDING ? "Listening now...\n\nRelease A to review before sending." :
            voice_state == VOICE_FINISHING ? "Audio captured.\n\nWaiting for transcription.\n\nKeep Evener open while audio saves.\nMenu > Actions > Discard draft removes it." : "Hold A and speak.\nRelease A to review before sending.";
        body = body_text(body);
        int height = pd->graphics->getTextHeightForMaxWidth(font, body, strlen(body), 384, kUTF8Encoding, kWrapWord, 0, 0);
        if (!follow && composer_font != font && composer_height > 0)
            scroll = (int)((int64_t)scroll * height / composer_height);
        composer_font = font; composer_height = height;
        int bottom = height > CONTENT_HEIGHT ? height - CONTENT_HEIGHT : 0;
        if (follow) scroll = bottom;
        if (scroll < 0) scroll = 0;
        if (scroll > bottom) scroll = bottom;
        pd->graphics->setClipRect(MARGIN, CONTENT_TOP, 384, CONTENT_HEIGHT);
        text(body, MARGIN, CONTENT_TOP - scroll, 384, height + 16);
        const char *hint = voice_state == VOICE_RECORDING ? "Listening... Release A to review" :
            voice_state == VOICE_FINISHING ? "Transcribing...  B Keep  Menu Actions" :
            recording_started && voice_state == VOICE_REVIEW && !final_saved ? "Saving review..." :
            hub_send_pending() ? "B Conversation" :
            draft.state == DRAFT_READY ? "A Send  B Keep draft  Menu Actions" :
            draft.state == DRAFT_UNCERTAIN || draft.state == DRAFT_SENDING ? "Checking delivery...  B Conversation" :
            draft.state == DRAFT_PARTIAL ? "Partial saved  Menu Actions to retry" : "Hold A Dictate  B Conversation";
        footer(hint, recording_started || voice_state == VOICE_ERROR ? voice_status : hub_status);
    }
}
static void draw_setup(void) {
    if (pairing_active()) pairing_draw(small_font);
    else {
        pd->graphics->clear(kColorWhite); pd->graphics->setFont(small_font);
        text(error[0] ? error : "Set up Evener\n\nOpen Evener Settings on your phone or computer, then press A here to receive setup.", 12, 38, 376, 170);
    }
}
static int render(void (*paint)(void)) {
    if (!canvas) { paint(); return 1; }
    // Keep clearing and text layout away from the displayed buffer. Only a
    // completed frame can replace its pixels, and idle rows need no refresh.
    pd->graphics->pushContext(canvas);
    paint();
    pd->graphics->popContext();
    int stride;
    uint8_t *pixels;
    pd->graphics->getBitmapData(canvas, NULL, NULL, &stride, NULL, &pixels);
    uint8_t *frame = pd->graphics->getFrame();
    int first = -1;
    bool changed = false;
    for (int y = 0; y < LCD_ROWS; y++) {
        uint8_t *target = frame + y * LCD_ROWSIZE;
        const uint8_t *source = pixels + y * stride;
        if (memcmp(target, source, LCD_COLUMNS / 8)) {
            memcpy(target, source, LCD_COLUMNS / 8);
            if (first < 0) first = y;
            changed = true;
        } else if (first >= 0) {
            pd->graphics->markUpdatedRows(first, y - 1);
            first = -1;
        }
    }
    if (first >= 0) pd->graphics->markUpdatedRows(first, LCD_ROWS - 1);
    return changed;
}
static int scroll_input(PDButtons held, PDButtons pressed, float crank) {
    int history = 0;
    PDButtons direction = pressed & (kButtonUp | kButtonDown);
    if (direction) repeat_at = app_time() + .35f;
    else if ((held & (kButtonUp | kButtonDown)) && app_time() > repeat_at) { direction = held & (kButtonUp | kButtonDown); repeat_at = app_time() + .08f; }
    if (crank != 0 || direction) {
        crank_remainder += crank;
        if (actions_open || screen == SCREEN_SESSIONS) {
            int move = (int)(crank_remainder / 18); crank_remainder -= move * 18;
            int *picked = actions_open ? &action_selection : &selection;
            *picked += move + ((direction & kButtonDown) ? 1 : (direction & kButtonUp) ? -1 : 0);
            if (*picked < 0) *picked = 0;
            if (actions_open && *picked > 5) *picked = 5;
        } else if (screen == SCREEN_TRANSCRIPT || screen == SCREEN_COMPOSER) {
            follow = false;
            int move = (int)crank_remainder; crank_remainder -= move;
            scroll += move + ((direction & kButtonDown) ? 30 : (direction & kButtonUp) ? -30 : 0);
            if (screen == SCREEN_TRANSCRIPT && scroll <= 0) history = -1;
            if (screen == SCREEN_TRANSCRIPT && scroll >= content_height - CONTENT_HEIGHT && (move > 0 || (direction & kButtonDown))) {
                if (hub_has_newer) history = 1;
                else follow = true;
            }
        }
    }
    return history;
}
static int update(void *context) {
    (void)context;
    if (network_restarting) return 0;
    PDButtons held, pressed, released;
    pd->system->getButtonState(&held, &pressed, &released);
    float crank = pd->system->getCrankChange();
    int history = 0;
    if (actions_open) {
        scroll_input(held, pressed, crank);
        if (pressed & (kButtonA | kButtonB)) {
            actions_open = false; crank_remainder = 0;
            if (pressed & kButtonA) {
                if (action_selection == 0) discard(NULL);
                else if (action_selection < 5) network_action(action_selection - 1);
            }
        }
        held = pressed = released = 0; crank = 0;
    } else if (permissions_ready && !pairing_active() && screen != SCREEN_AUTH) history = scroll_input(held, pressed, crank);
    else crank_remainder = 0;
    hub_set_reading(screen == SCREEN_TRANSCRIPT);
    hub_set_prefetch_paused(actions_open || pairing_active() || screen == SCREEN_AUTH || screen == SCREEN_COMPOSER);
    if (pairing_active()) {
        net_tick();
        enum PairingResult result = pairing_update(pressed);
        if (result == PAIRING_RUNNING) return render(draw_setup);
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
        return render(draw_setup);
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
                return render(draw_authorization);
            }
            if (config.use_tailnet) tailnet_device_update();
            hub_update();
            if ((pressed & kButtonB) || hub_ready) screen = SCREEN_SESSIONS;
            else {
                if (pressed & kButtonA) reconnect(NULL);
                if (recover_network()) return 0;
                return render(draw_authorization);
            }
        }
        if ((pressed & kButtonA) && (screen == SCREEN_TRANSCRIPT ||
            (screen == SCREEN_COMPOSER && draft.state == DRAFT_EMPTY && voice_state != VOICE_FINISHING))) {
            compose();
            pressed &= ~kButtonA;
        }
        bool send_review = screen == SCREEN_COMPOSER && (pressed & kButtonA) &&
            draft.state == DRAFT_READY && !hub_send_pending() && (!recording_started || final_saved) &&
            voice_state != VOICE_RECORDING && voice_state != VOICE_FINISHING;
        if (!(held & kButtonA) && voice_state == VOICE_RECORDING) voice_finish();
        char focused[REF_CAPACITY] = "";
        if (screen == SCREEN_SESSIONS && selection < navigation_count)
            memcpy(focused, navigation[selection].ref, sizeof(focused));
        int nav_revision = navigation_revision;
        const char *anchor = "";
        if (screen == SCREEN_TRANSCRIPT && !follow)
            for (int i = 0; i < measured_count; i++) if (row_y[i] + row_height[i] > scroll) {
                anchor = measured_keys[i]; break;
            }
        hub_set_reading_anchor(anchor);
        if (history < 0) hub_older();
        else if (history > 0) hub_newer();
        enum DraftState before = draft.state;
        if (config.use_tailnet) {
            tailnet_device_set_voice_active(voice_state == VOICE_RECORDING || voice_state == VOICE_FINISHING);
            tailnet_device_set_hub_active(hub_foreground_busy());
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
                        return render(draw);
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
    if (recover_network()) return 0;
    return render(draw);
}
#ifdef _WINDLL
__declspec(dllexport)
#endif
int eventHandler(PlaydateAPI *playdate, PDSystemEvent event, uint32_t arg) {
    (void)arg;
    // The SDK owns teardown once a runtime restart is requested. Normal
    // shutdown would enqueue more TCP work against the runtime being removed.
    if (network_restarting) return 0;
    if (pd && (event == kEventLock || event == kEventPause || event == kEventTerminate ||
        event == kEventResume || event == kEventUnlock))
        app_log("Application lifecycle event %u at %.2f seconds", (unsigned)event, (double)app_time());
    if (event == kEventInit) {
        pd = playdate;
        initialize_reader();
        canvas = pd->graphics->newBitmap(LCD_COLUMNS, LCD_ROWS, kColorWhite);
        if (!canvas) app_log("Could not allocate the offscreen display buffer");
        pd->display->setRefreshRate(30);
        pd->system->setUpdateCallback(update, NULL);
        if (!read_configuration()) snprintf(error, sizeof(error), "Device configuration is missing or could not be read.");
        else {
            setup_requested = !settings_ready();
            if (!setup_requested) restore_network_context();
        }
        network_restart_at = app_time() + 60;
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
        pd->system->getCrankChange(); crank_remainder = 0;
        if (pairing_active()) { pairing_resume(); return 0; }
        if (!permissions_ready) return 0;
        if (screen == SCREEN_COMPOSER) recording_started = voice_recover() && voice_state != VOICE_ERROR;
        refresh_saved_audio();
        hub_connect();
    }
    return 0;
}

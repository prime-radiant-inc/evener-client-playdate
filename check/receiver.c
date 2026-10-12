/* Real receiver, HTTP poller and persistence; only SDK/TCP boundaries are hosted. */
#include "platform.h"
#include "pairing.h"
#include "settings.h"
#include "storage.h"
#include <dirent.h>
#include <string.h>
#include <unistd.h>

void check_network_start(const char *endpoint);
extern unsigned network_opens;
void random_fill(uint8_t *bytes, size_t count) { memset(bytes, 0x3a, count); }
static void clear(LCDColor color) { (void)color; }
static void font(LCDFont *value) { (void)value; }
static LCDBitmapDrawMode mode(LCDBitmapDrawMode value) { return value; }
static void rect(int x, int y, int w, int h, LCDColor color) { (void)x; (void)y; (void)w; (void)h; (void)color; }
static void draw(const void *text, size_t len, PDStringEncoding encoding, int x, int y, int w, int h, PDTextWrappingMode wrap, PDTextAlignment align) {
    (void)text; (void)len; (void)encoding; (void)x; (void)y; (void)w; (void)h; (void)wrap; (void)align;
}
static struct playdate_graphics graphics = {.clear = clear, .setFont = font, .setDrawMode = mode, .fillRect = rect, .drawTextInRect = draw};
static void receive(FILE *pcm) {
    rewind(pcm);
    int16_t samples[512]; size_t n;
    while ((n = fread(samples, sizeof(*samples), 512, pcm))) {
        if (mic_callback) CHECK(mic_callback(mic_context, samples, (int)n) == 1);
        fixture_time += (float)n / 44100;
        CHECK(pairing_update(0) == PAIRING_RUNNING);
    }
    CHECK(!mic_callback && pairing_active());
    CHECK(!config.hub_token[0]);
}
static void wait_for_review(void) {
    for (unsigned i = 0; i < 4500; i++) {
        CHECK(pairing_update(0) == PAIRING_RUNNING);
        pairing_draw(NULL);
        if (pairing_ready_to_save()) { CHECK(!config.hub_token[0]); return; }
        fixture_time += 0.01f; usleep(10000);
    }
    CHECK(!"Timed out waiting for Hub approval");
}
static void old_draft(void) { CHECK(draft_load("session")); CHECK(!strcmp(draft.text, "Unsent on original Hub")); }
int main(int argc, char **argv) {
    CHECK(argc == 3); pd->graphics = &graphics;
    FILE *pcm = fopen(argv[1], "rb"); CHECK(pcm); check_network_start(argv[2]);
    char directory[] = "/tmp/playdate-receiver-XXXXXX";
    CHECK(mkdtemp(directory)); CHECK(!chdir(directory));
    PairingSettings original = destination("prior-hub"); bundle(&original);
    CHECK(settings_initialize()); CHECK(settings_bind_peer("original-peer"));
    CHECK(draft_load("session")); CHECK(draft_set("Unsent on original Hub", true));
    CHECK(pairing_begin()); CHECK(!mic_callback);
    CHECK(mic_permission); mic_permission(false, permission_context);
    CHECK(!mic_callback && pairing_active());
    CHECK(pairing_update(kButtonA) == PAIRING_RUNNING);
    mic_permission(true, permission_context); CHECK(mic_callback && mic_starts == 1);
    pairing_pause(); CHECK(!mic_callback); pairing_resume(); CHECK(mic_callback && mic_starts == 2);
    receive(pcm); old_draft();
    CHECK(pairing_update(kButtonB) == PAIRING_CANCELLED);
    CHECK(!pairing_active() && !mic_callback && !strcmp(settings_host(), original.host)); old_draft();
    CHECK(pairing_begin()); receive(pcm);
    pairing_pause(); CHECK(!mic_callback); pairing_resume();
    wait_for_review(); old_draft(); CHECK(network_opens >= 2);
    fail_write = true; CHECK(pairing_update(kButtonA) == PAIRING_RUNNING); fail_write = false;
    CHECK(!config.hub_token[0]); old_draft();
    fail_unlink = true; CHECK(pairing_update(kButtonB) == PAIRING_RUNNING); CHECK(pairing_active());
    fail_unlink = false; CHECK(pairing_update(kButtonB) == PAIRING_CANCELLED);
    bundle(&original); CHECK(settings_initialize()); CHECK(!strcmp(settings_host(), original.host));
    CHECK(settings_bind_peer("original-peer")); old_draft();
    CHECK(pairing_begin()); receive(pcm); wait_for_review();
    fail_flush = true; CHECK(pairing_update(kButtonA) == PAIRING_RUNNING); fail_flush = false;
    CHECK(!config.hub_token[0]); old_draft();
    fail_rename = true; CHECK(pairing_update(kButtonA) == PAIRING_APPLIED); fail_rename = false;
    CHECK(!pairing_active() && !mic_callback);
    CHECK(!strcmp(settings_host(), "100.64.1.2")); CHECK(!strcmp(config.hub_token, "synthetic-pairing-token"));
    bundle(&original); CHECK(settings_initialize()); CHECK(!strcmp(settings_host(), "100.64.1.2"));
    CHECK(!strcmp(config.hub_token, "synthetic-pairing-token"));
    CHECK(draft_load("session")); CHECK(draft.state == DRAFT_EMPTY);
    CHECK(settings_save(&original)); CHECK(settings_bind_peer("original-peer")); old_draft();
    CHECK(pairing_begin()); fixture_time += 601;
    CHECK(pairing_update(0) == PAIRING_RUNNING); CHECK(!mic_callback);
    CHECK(pairing_update(kButtonA) == PAIRING_RUNNING); CHECK(mic_callback);
    CHECK(pairing_update(kButtonB) == PAIRING_CANCELLED); CHECK(!mic_callback); old_draft();
    fclose(pcm); DIR *dir = opendir("."); CHECK(dir); struct dirent *entry;
    while ((entry = readdir(dir))) if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, "..")) CHECK(!unlink(entry->d_name));
    closedir(dir); CHECK(!chdir("/")); CHECK(!rmdir(directory));
    puts("PASS C receiver -> real Hub approval -> reviewed save, dropped reply, short reads/writes, pause/resume, failed-save retry/cancel, restart and draft preservation");
    return 0;
}

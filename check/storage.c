/* Run real profile, draft and audio persistence against an isolated filesystem. */
#include "platform.h"
#include "settings.h"
#include "storage.h"
#include "recording.h"
#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void cleanup(const char *directory) {
    DIR *dir = opendir("."); CHECK(dir); struct dirent *entry;
    while ((entry = readdir(dir))) if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, "..")) CHECK(!unlink(entry->d_name));
    closedir(dir); CHECK(!chdir("/")); CHECK(!rmdir(directory));
}
static void profiles(void) {
    char directory[] = "/tmp/playdate-storage-XXXXXX";
    CHECK(mkdtemp(directory)); CHECK(!chdir(directory));
    PairingSettings a = destination("hub-a"), b = destination("hub-b"), c = destination("hub-c");
    a.tailnet = b.tailnet = c.tailnet = false;
    bundle(&a); CHECK(settings_initialize()); CHECK(settings_bundled_profile());
    const char *ref = "same-session-reference";
    CHECK(draft_load(ref)); CHECK(draft_set("Draft for Hub A", true));
    bundle(&c); CHECK(settings_initialize());
    CHECK(!strcmp(settings_host(), "hub-a"));
    CHECK(draft_load(ref)); CHECK(!strcmp(draft.text, "Draft for Hub A"));
    bundle(&a); CHECK(settings_initialize());
    const uint8_t samples[] = {1, 2, 3, 4}; size_t accepted;
    CHECK(recording_begin(ref)); CHECK(recording_append(samples, sizeof(samples), &accepted));
    CHECK(accepted == sizeof(samples)); recording_close();
    CHECK(settings_save(&b)); CHECK(!settings_bundled_profile());
    CHECK(!strcmp(config.openai_key, "synthetic-embedded-key") && !strcmp(config.seed, "synthetic-build-seed"));
    CHECK(draft_load(ref)); CHECK(draft.state == DRAFT_EMPTY); CHECK(!recording_exists(ref));
    CHECK(draft_set("Draft for Hub B", true));
    char profile[64], rotated[64]; settings_path(profile, sizeof(profile), "draft");
    strcpy(b.token, "rotated-token"); CHECK(settings_save(&b)); settings_path(rotated, sizeof(rotated), "draft");
    CHECK(!strcmp(profile, rotated)); CHECK(draft_load(ref)); CHECK(!strcmp(draft.text, "Draft for Hub B"));
    bundle(&c); CHECK(settings_initialize()); settings_path(rotated, sizeof(rotated), "draft");
    CHECK(!strcmp(profile, rotated)); CHECK(!strcmp(settings_host(), "hub-b"));
    CHECK(settings_save(&a)); CHECK(draft_load(ref)); CHECK(!strcmp(draft.text, "Draft for Hub A"));
    CHECK(recording_load(ref) == RECORDING_PRESENT);
    uint8_t restored[4]; CHECK(recording_read(0, restored, sizeof(restored)) == sizeof(restored));
    CHECK(!memcmp(samples, restored, sizeof(samples))); recording_close();
    CHECK(!settings_bundled_profile()); // The replacement build's identity belongs to Hub C.
    fail_write = true; CHECK(!settings_save(&b)); fail_write = false;
    CHECK(!strcmp(settings_host(), "hub-a")); bundle(&c); CHECK(settings_initialize());
    CHECK(!strcmp(settings_host(), "hub-a")); CHECK(draft_load(ref)); CHECK(!strcmp(draft.text, "Draft for Hub A"));
    fail_rename = true; CHECK(settings_save(&b));
    CHECK(!strcmp(settings_host(), "hub-b")); bundle(&c); CHECK(settings_initialize());
    CHECK(!strcmp(settings_host(), "hub-b")); CHECK(!settings_save(&a));
    CHECK(settings_cancel_pending()); // The committed stage cannot be cancelled by a later failed save.
    fail_rename = false; bundle(&c); CHECK(settings_initialize());
    CHECK(!strcmp(settings_host(), "hub-b")); CHECK(draft_load(ref)); CHECK(!strcmp(draft.text, "Draft for Hub B"));
    b.port = 0; CHECK(!settings_save(&b)); CHECK(config.hub_port == 9180);
    cleanup(directory);
}
static void peers(void) {
    char directory[] = "/tmp/playdate-peers-XXXXXX";
    CHECK(mkdtemp(directory)); CHECK(!chdir(directory));
    PairingSettings hub = destination("hub"), replacement = destination("another-hub");
    bundle(&hub); fail_write = true; CHECK(!settings_initialize()); CHECK(!settings_ready());
    fail_write = false; CHECK(settings_cancel_pending()); CHECK(settings_initialize());
    fail_write = true; CHECK(!settings_bind_peer("peer-a")); fail_write = false;
    CHECK(settings_bind_peer("peer-a")); CHECK(settings_bundled_profile());
    const char *ref = "same-session-reference";
    CHECK(draft_load(ref)); CHECK(draft_set("Draft for peer A", true));
    uint8_t samples[] = {1, 2, 3, 4}; size_t accepted;
    CHECK(recording_begin(ref)); CHECK(recording_append(samples, sizeof(samples), &accepted)); recording_close();
    char path[64], connection[64], other_connection[64];
    settings_path(path, sizeof(path), "draft"); CHECK(!strcmp(path, "draft"));
    settings_connection_path(connection, sizeof(connection), "tailnet-state.bin");
    CHECK(settings_bind_peer("")); CHECK(draft_load(ref)); CHECK(draft.state == DRAFT_EMPTY);
    CHECK(!recording_exists(ref));
    CHECK(settings_bind_peer("peer-b")); CHECK(draft_load(ref)); CHECK(draft.state == DRAFT_EMPTY);
    CHECK(!recording_exists(ref)); CHECK(draft_set("Draft for peer B", true));
    settings_connection_path(other_connection, sizeof(other_connection), "tailnet-state.bin");
    CHECK(!strcmp(connection, other_connection)); // Connection keys are needed before the peer can be resolved.
    strcpy(hub.token, "rotated-token"); CHECK(settings_save(&hub));
    CHECK(draft_load(ref)); CHECK(!strcmp(draft.text, "Draft for peer B"));
    bundle(&replacement); CHECK(settings_initialize()); CHECK(!strcmp(settings_host(), "hub"));
    CHECK(!settings_bundled_profile()); CHECK(draft_load(ref)); CHECK(draft.state == DRAFT_EMPTY);
    CHECK(settings_bind_peer("peer-b")); CHECK(draft_load(ref)); CHECK(!strcmp(draft.text, "Draft for peer B"));
    CHECK(settings_bind_peer("peer-a")); CHECK(draft_load(ref)); CHECK(!strcmp(draft.text, "Draft for peer A"));
    CHECK(recording_load(ref) == RECORDING_PRESENT);
    uint8_t restored[4]; CHECK(recording_read(0, restored, sizeof(restored)) == sizeof(restored));
    CHECK(!memcmp(samples, restored, sizeof(samples))); recording_close();
    cleanup(directory);
}
static void bootstrap(void) {
    char directory[] = "/tmp/playdate-bootstrap-XXXXXX"; CHECK(mkdtemp(directory)); CHECK(!chdir(directory));
    PairingSettings old = destination("saved-hub"), next = destination("new-hub");
    bundle(&old); CHECK(settings_initialize()); CHECK(settings_bind_peer("old-peer"));
    CHECK(draft_load("ref")); CHECK(draft_set("Keep my original draft", true));
    char original[64], pending[64], saved_connection[64], new_connection[64];
    settings_path(original, sizeof(original), "draft"); settings_connection_path(saved_connection, sizeof(saved_connection), "identity");
    settings_bootstrap_begin(&next); CHECK(settings_bind_peer("new-peer"));
    settings_path(pending, sizeof(pending), "draft"); CHECK(!strcmp(original, pending));
    settings_connection_path(new_connection, sizeof(new_connection), "identity"); CHECK(strcmp(saved_connection, new_connection));
    CHECK(!config.hub_token[0]); CHECK(!strcmp(config.hub_host, "new-hub"));
    settings_bootstrap_end(); CHECK(!strcmp(config.hub_host, "saved-hub")); CHECK(config.hub_token[0]);
    CHECK(draft_load("ref")); CHECK(!strcmp(draft.text, "Keep my original draft"));
    settings_bootstrap_begin(&next); CHECK(settings_bind_peer("new-peer"));
    fail_write = true; CHECK(!settings_save(&next)); fail_write = false;
    settings_path(pending, sizeof(pending), "draft"); CHECK(!strcmp(original, pending));
    CHECK(settings_save(&next));
    settings_connection_path(pending, sizeof(pending), "identity"); CHECK(!strcmp(new_connection, pending));
    CHECK(draft_load("ref")); CHECK(draft.state == DRAFT_EMPTY);
    CHECK(settings_save(&old)); CHECK(settings_bind_peer("old-peer"));
    CHECK(draft_load("ref")); CHECK(!strcmp(draft.text, "Keep my original draft"));
    cleanup(directory);
}
int main(void) {
    profiles(); peers(); bootstrap();
    puts("PASS destination/peer isolation, initial ownership, key preservation, rotation, build replacement, saved audio and committed-stage recovery");
    return 0;
}

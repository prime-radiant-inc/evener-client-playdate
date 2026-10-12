#include "settings.h"
#include "app.h"
#include "storage.h"
#include "tweetnacl.h"
#include <stdio.h>
#include <string.h>

typedef struct { uint32_t magic, version; PairingSettings settings; uint8_t root[8]; char root_peer[64]; uint32_t checksum; } Record;
static PairingSettings bundled, active;
static uint8_t root[8];
static char root_peer[64], peer[64], prefix[20], connection_prefix[20];
static bool pending_write, bootstrapping;
static PairingSettings bootstrap;
static char bootstrap_peer[64], bootstrap_prefix[20];
static bool same_destination(const PairingSettings *a, const PairingSettings *b) {
    return a->port == b->port && a->tls == b->tls && a->tailnet == b->tailnet &&
        !strcmp(a->host, b->host) && !strcmp(a->path, b->path);
}
static void destination_hash(const PairingSettings *s, uint8_t out[8]) {
    char identity[560]; uint8_t hash[64];
    int n = snprintf(identity, sizeof(identity), "%s:%u%s/%u/%u", s->host, s->port, s->path, s->tailnet, s->tls);
    crypto_hash(hash, (const uint8_t *)identity, (size_t)n);
    memcpy(out, hash, 8);
}
static void format_prefix(char out[20], const uint8_t hash[8]) {
    memcpy(out, "p-", 2);
    for (unsigned i = 0; i < 8; i++) snprintf(out + 2 + 2 * i, 3, "%02x", hash[i]);
    out[18] = '-'; out[19] = 0;
}
static void scope_paths(void) {
    uint8_t hash[8]; destination_hash(&active, hash);
    bool original = !memcmp(hash, root, sizeof(root));
    connection_prefix[0] = 0;
    if (!original) format_prefix(connection_prefix, hash);
    prefix[0] = 0;
    if (original && (!active.tailnet || (peer[0] && !strcmp(peer, root_peer)))) return;
    if (active.tailnet) {
        uint8_t identity[8 + sizeof(peer)] = {0}, digest[64];
        memcpy(identity, hash, 8); memcpy(identity + 8, peer, strlen(peer));
        crypto_hash(digest, identity, sizeof(identity)); memcpy(hash, digest, 8);
    }
    format_prefix(prefix, hash);
}
static void configure(const PairingSettings *s) {
    snprintf(config.hub_host, sizeof(config.hub_host), "%s", s->host);
    snprintf(config.hub_path, sizeof(config.hub_path), "%s", s->path);
    snprintf(config.hub_token, sizeof(config.hub_token), "%s", s->token);
    config.hub_port = s->port; config.hub_tls = s->tls; config.use_tailnet = s->tailnet;
}
static void apply(const PairingSettings *s) {
    if (!same_destination(&active, s)) peer[0] = 0;
    active = *s; configure(s); scope_paths();
}
void settings_bootstrap_begin(const PairingSettings *destination) {
    bootstrap = *destination; bootstrap.token[0] = 0;
    bootstrapping = true; bootstrap_peer[0] = bootstrap_prefix[0] = 0;
    uint8_t hash[8]; destination_hash(&bootstrap, hash);
    if (memcmp(hash, root, sizeof(root))) format_prefix(bootstrap_prefix, hash);
    configure(&bootstrap);
}
void settings_bootstrap_end(void) {
    bootstrapping = false; memset(&bootstrap, 0, sizeof(bootstrap));
    memset(bootstrap_peer, 0, sizeof(bootstrap_peer)); configure(&active);
}
static int read_record(const char *path, Record *record) {
    FileStat metadata;
    if (pd->file->stat(path, &metadata)) return 0;
    SDFile *file = pd->file->open(path, kFileReadData);
    if (!file) return -1;
    int n = pd->file->read(file, record, sizeof(*record));
    pd->file->close(file);
    return metadata.size == sizeof(*record) && n == sizeof(*record) && record->magic == 0x45565031 && record->version == 1 &&
        record->checksum == pairing_crc((const uint8_t *)record, offsetof(Record, checksum)) &&
        memchr(record->root_peer, 0, sizeof(record->root_peer)) && pairing_validate(&record->settings) ? 1 : -1;
}
bool settings_cancel_pending(void) {
    if (!pending_write) return true;
    if (pd->file->unlink("hub-settings.tmp", 0)) return false;
    pending_write = false;
    return true;
}
static bool write_record(const PairingSettings *s, const char *owner) {
    static Record record;
    if (!settings_cancel_pending()) return false;
    if (read_record("hub-settings.tmp", &record) == 1 && !storage_replace("hub-settings.tmp", "hub-settings.bin")) return false;
    memset(&record, 0, sizeof(record));
    record.magic = 0x45565031; record.version = 1; record.settings = *s;
    memcpy(record.root, root, sizeof(root));
    snprintf(record.root_peer, sizeof(record.root_peer), "%s", owner);
    record.checksum = pairing_crc((const uint8_t *)&record, offsetof(Record, checksum));
    SDFile *file = pd->file->open("hub-settings.tmp", kFileWrite);
    if (!file) return false;
    pending_write = true;
    bool ok = pd->file->write(file, &record, sizeof(record)) == sizeof(record) && pd->file->flush(file) >= 0;
    if (pd->file->close(file)) ok = false;
    if (!ok) return false;
    // A complete flushed record is the commit point. Promotion may be retried
    // after restart; never report it as cancellable or overwrite it beforehand.
    pending_write = false;
    storage_replace("hub-settings.tmp", "hub-settings.bin");
    return true;
}
bool settings_initialize(void) {
    memset(&active, 0, sizeof(active)); prefix[0] = connection_prefix[0] = peer[0] = root_peer[0] = 0;
    pending_write = bootstrapping = false;
    memset(&bundled, 0, sizeof(bundled));
    snprintf(bundled.host, sizeof(bundled.host), "%s", config.hub_host);
    snprintf(bundled.path, sizeof(bundled.path), "%s", config.hub_path);
    snprintf(bundled.token, sizeof(bundled.token), "%s", config.hub_token);
    bundled.port = config.hub_port; bundled.tls = config.hub_tls; bundled.tailnet = config.use_tailnet;
    // Root files belong to the original bundled destination even after a build
    // changes its defaults. Save that ownership before any profile data is used.
    destination_hash(&bundled, root);
    static Record record;
    int staged = read_record("hub-settings.tmp", &record);
    if (staged == 1) {
        storage_replace("hub-settings.tmp", "hub-settings.bin");
    } else {
        int saved = read_record("hub-settings.bin", &record);
        if (saved != 1) {
            if (saved < 0 || staged < 0) return false;
            if (pairing_validate(&bundled) && !write_record(&bundled, "")) return false;
            apply(&bundled); return true;
        }
    }
    memcpy(root, record.root, sizeof(root)); memcpy(root_peer, record.root_peer, sizeof(root_peer));
    apply(&record.settings); return true;
}
bool settings_ready(void) { return pairing_validate(&active); }
const char *settings_host(void) { return bootstrapping ? bootstrap.host : active.host; }
bool settings_bundled_profile(void) { return bootstrapping ? !bootstrap_prefix[0] && same_destination(&bootstrap, &bundled) : !connection_prefix[0] && same_destination(&active, &bundled); }
void settings_path(char *out, size_t capacity, const char *leaf) { snprintf(out, capacity, "%s%s", prefix, leaf); }
void settings_connection_path(char *out, size_t capacity, const char *leaf) { snprintf(out, capacity, "%s%s", bootstrapping ? bootstrap_prefix : connection_prefix, leaf); }
bool settings_save(const PairingSettings *s) {
    if (!pairing_validate(s)) return false;
    char owner[64]; snprintf(owner, sizeof(owner), "%s", root_peer);
    if (bootstrapping && !bootstrap_prefix[0] && s->tailnet && !owner[0])
        snprintf(owner, sizeof(owner), "%s", bootstrap_peer);
    if (!write_record(s, owner)) return false;
    snprintf(root_peer, sizeof(root_peer), "%s", owner);
    apply(s);
    if (bootstrapping) {
        snprintf(peer, sizeof(peer), "%s", bootstrap_peer);
        bootstrapping = false; scope_paths();
        memset(&bootstrap, 0, sizeof(bootstrap)); memset(bootstrap_peer, 0, sizeof(bootstrap_peer));
    }
    return true;
}
bool settings_bind_peer(const char *stable_id) {
    size_t n = strnlen(stable_id, sizeof(peer));
    if (n >= sizeof(peer)) return false;
    for (size_t i = 0; i < n; i++) if ((unsigned char)stable_id[i] < 33 || (unsigned char)stable_id[i] > 126) return false;
    if (bootstrapping) { snprintf(bootstrap_peer, sizeof(bootstrap_peer), "%s", stable_id); return true; }
    if (!strcmp(peer, stable_id)) return true;
    if (active.tailnet && !connection_prefix[0] && !root_peer[0] && n) {
        if (!write_record(&active, stable_id)) return false;
        snprintf(root_peer, sizeof(root_peer), "%s", stable_id);
    }
    snprintf(peer, sizeof(peer), "%s", stable_id); scope_paths();
    return true;
}

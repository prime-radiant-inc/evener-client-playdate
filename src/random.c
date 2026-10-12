#include "app.h"
#include "random.h"
#include "storage.h"
#include "tweetnacl.h"
#include <stdio.h>
#include <string.h>

typedef struct { uint32_t magic, seed_hash; uint64_t boot; uint32_t checksum; } Counter;
static uint8_t seed[32];
static uint64_t boot, counter;
static uint32_t hash(const void *bytes, size_t length) {
    const uint8_t *p = bytes; uint32_t value = 2166136261u;
    while (length--) value = (value ^ *p++) * 16777619u;
    return value;
}
static int load(const char *path, Counter *value) {
    FileStat metadata;
    if (pd->file->stat(path, &metadata)) return 0;
    SDFile *file = pd->file->open(path, kFileReadData);
    if (!file) return -1;
    int n = pd->file->read(file, value, sizeof(*value));
    pd->file->close(file);
    if (n != sizeof(*value) || value->magic != 0x45565231 ||
        value->checksum != hash(value, offsetof(Counter, checksum))) return -1;
    return 1;
}
bool random_initialize(const char *encoded) {
    if (strlen(encoded) != 64) return false;
    for (unsigned i = 0; i < 32; i++) {
        unsigned value;
        if (sscanf(encoded + i * 2, "%2x", &value) != 1) return false;
        seed[i] = (uint8_t)value;
    }
    Counter saved = {0}, staged = {0};
    int primary = load("random-state.bin", &saved);
    int temporary = load("random-state.tmp", &staged);
    uint32_t identity = hash(seed, sizeof(seed));
    if (primary < 0 && temporary != 1) return false;
    if (temporary < 0 && primary != 1) return false;
    if (temporary == 1 && staged.seed_hash == identity &&
        (primary != 1 || saved.seed_hash != identity || staged.boot > saved.boot)) saved = staged;
    if (saved.seed_hash == identity && saved.boot == UINT64_MAX) return false;
    boot = saved.seed_hash == identity ? saved.boot + 1 : 1;
    Counter next = { .magic = 0x45565231, .seed_hash = identity, .boot = boot };
    next.checksum = hash(&next, offsetof(Counter, checksum));
    SDFile *file = pd->file->open("random-state.tmp", kFileWrite);
    if (!file) return false;
    bool ok = pd->file->write(file, &next, sizeof(next)) == sizeof(next) && pd->file->flush(file) >= 0;
    if (pd->file->close(file)) ok = false;
    if (!ok || !storage_replace("random-state.tmp", "random-state.bin")) return false;
    counter = 0;
    return true;
}
static void little64(uint8_t *out, uint64_t n) { for (unsigned i = 0; i < 8; i++) out[i] = (uint8_t)(n >> (8 * i)); }
void random_fill(uint8_t *bytes, size_t length) {
    uint8_t nonce[24];
    little64(nonce, boot);
#if TARGET_PLAYDATE
    little64(nonce + 8, UINT64_C(0x4556454e45520001));
#else
    little64(nonce + 8, UINT64_C(0x4556454e45520002));
#endif
    little64(nonce + 16, ++counter);
    crypto_stream(bytes, length, nonce, seed);
}

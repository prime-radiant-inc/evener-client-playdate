#include "pairing_protocol.h"
#include "tweetnacl.h"
#include <string.h>

uint32_t pairing_crc(const uint8_t *bytes, size_t length) {
    uint32_t crc = 0xffffffffu;
    while (length--) {
        crc ^= *bytes++;
        for (unsigned i = 0; i < 8; i++) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1)));
    }
    return ~crc;
}
void pairing_key(const uint8_t secret[32], const uint8_t session[8], char qr[PAIRING_QR_LENGTH]) {
    uint8_t public[32];
    static const char hex[] = "0123456789abcdef";
    crypto_scalarmult_base(public, secret);
    memcpy(qr, "EVENER1:", 8);
    for (unsigned i = 0; i < 40; i++) {
        uint8_t byte = i < 8 ? session[i] : public[i - 8];
        qr[8 + i * 2] = hex[byte >> 4]; qr[9 + i * 2] = hex[byte & 15];
    }
    qr[88] = 0;
}
static bool valid_destination(const PairingSettings *s) {
    if (!s->port || !memchr(s->host, 0, sizeof(s->host)) ||
        !memchr(s->path, 0, sizeof(s->path)) || !memchr(s->token, 0, sizeof(s->token)) ||
        !s->host[0] || s->path[0] != '/' || (s->tailnet && s->tls)) return false;
    for (const unsigned char *p = (const unsigned char *)s->host; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '.' || *p == '-')) return false;
    for (const unsigned char *p = (const unsigned char *)s->path; *p; p++)
        if (*p <= 32 || *p > 126 || *p == '#' || *p == '\\') return false;
    return true;
}
bool pairing_validate(const PairingSettings *s) {
    if (!valid_destination(s) || !s->token[0]) return false;
    for (const unsigned char *p = (const unsigned char *)s->token; *p; p++)
        if (*p < 33 || *p > 126) return false;
    return true;
}
bool pairing_open(const uint8_t *bytes, size_t length, const uint8_t secret[32],
    const uint8_t session[8], PairingSettings *out, unsigned *comparison) {
    // Static scratch keeps the decrypt path off the firmware's small task stack.
    static uint8_t cipher[PAIRING_MAX_BYTES], plain[PAIRING_MAX_BYTES];
    static PairingSettings candidate;
    uint8_t digest[64], shared[32];
    bool ok = false;
    if (length < 92 || length > PAIRING_MAX_BYTES || memcmp(bytes, "EVP1", 4) || memcmp(bytes + 4, session, 8)) return false;
    crypto_scalarmult(shared, secret, bytes + 12);
    uint8_t any = 0; for (unsigned i = 0; i < sizeof(shared); i++) any |= shared[i];
    memset(shared, 0, sizeof(shared));
    if (!any) return false;
    size_t boxed = length - 68 + 16;
    memset(cipher, 0, 16); memcpy(cipher + 16, bytes + 68, length - 68);
    if (crypto_box_open(plain, cipher, boxed, bytes + 44, bytes + 12, secret)) goto done;
    const uint8_t *p = plain + 32;
    size_t host = p[4], path = p[5], token = (size_t)p[6] * 256 + p[7];
    if (p[0] != 1 || (p[1] & ~3) || !host || !path || !token || token > 511 ||
        8 + host + path + token != boxed - 32) goto done;
    memset(&candidate, 0, sizeof(candidate));
    candidate.port = (uint16_t)((unsigned)p[2] * 256 + p[3]);
    candidate.tailnet = (p[1] & 1) != 0; candidate.tls = (p[1] & 2) != 0;
    memcpy(candidate.host, p + 8, host);
    memcpy(candidate.path, p + 8 + host, path);
    memcpy(candidate.token, p + 8 + host + path, token);
    if (strlen(candidate.host) != host || strlen(candidate.path) != path || strlen(candidate.token) != token ||
        !pairing_validate(&candidate)) goto done;
    crypto_hash(digest, bytes, length);
    *comparison = (((unsigned)digest[0] << 16) | ((unsigned)digest[1] << 8) | digest[2]) % 1000000;
    *out = candidate; ok = true;
done:
    memset(cipher, 0, sizeof(cipher)); memset(plain, 0, sizeof(plain)); memset(&candidate, 0, sizeof(candidate));
    return ok;
}

bool pairing_invitation(const uint8_t *bytes, size_t length, PairingInvitation *out) {
    if (length < 38 || length > 548 || memcmp(bytes, "EVI1", 4) || bytes[4] > 2 || !bytes[7] || bytes[8] != 4 ||
        length != 33u + bytes[7] + bytes[8]) return false;
    memset(out, 0, sizeof(*out));
    out->destination.port = (uint16_t)((unsigned)bytes[5] * 256 + bytes[6]);
    out->destination.tailnet = (bytes[4] & 1) != 0; out->destination.tls = (bytes[4] & 2) != 0;
    memcpy(out->secret, bytes + 9, 24);
    memcpy(out->destination.host, bytes + 33, bytes[7]);
    memcpy(out->destination.path, bytes + 33 + bytes[7], bytes[8]);
    return strlen(out->destination.host) == bytes[7] && !strcmp(out->destination.path, "/rpc") && valid_destination(&out->destination);
}
void pairing_claim_key(const PairingInvitation *invitation, const uint8_t secret[32], char public_hex[65], char comparison[PAIRING_COMPARISON_SIZE]) {
    static const char domain[] = "evener-pairing-v1", hex[] = "0123456789abcdef";
    uint8_t public[32], input[sizeof(domain) - 1 + 24 + 32], digest[64];
    crypto_scalarmult_base(public, secret);
    for (unsigned i = 0; i < 32; i++) { public_hex[i * 2] = hex[public[i] >> 4]; public_hex[i * 2 + 1] = hex[public[i] & 15]; }
    public_hex[64] = 0;
    memcpy(input, domain, sizeof(domain) - 1); memcpy(input + sizeof(domain) - 1, invitation->secret, 24);
    memcpy(input + sizeof(domain) - 1 + 24, public, 32); crypto_hash(digest, input, sizeof(input));
    unsigned at = 0;
    for (unsigned i = 0; i < 16; i++) {
        if (i && !(i % 2)) comparison[at++] = '-';
        comparison[at++] = hex[digest[i] >> 4]; comparison[at++] = hex[digest[i] & 15];
    }
    comparison[at] = 0;
}
bool pairing_approved(const uint8_t *bytes, size_t length, const PairingInvitation *invitation,
    const uint8_t secret[32], PairingSettings *out) {
    unsigned unused;
    if (!pairing_open(bytes, length, secret, invitation->secret, out, &unused)) return false;
    const PairingSettings *d = &invitation->destination;
    if (out->port == d->port && out->tailnet == d->tailnet && out->tls == d->tls &&
        !strcmp(out->host, d->host) && !strcmp(out->path, d->path)) return true;
    memset(out, 0, sizeof(*out)); return false;
}

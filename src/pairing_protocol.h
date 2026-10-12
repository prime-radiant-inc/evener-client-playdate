#ifndef EVENER_PAIRING_PROTOCOL_H
#define EVENER_PAIRING_PROTOCOL_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PAIRING_MAX_BYTES 1152
#define PAIRING_QR_LENGTH 89
#define PAIRING_COMPARISON_SIZE 40
typedef struct {
    char host[256], path[256], token[512];
    uint16_t port;
    bool tailnet, tls;
} PairingSettings;

void pairing_key(const uint8_t secret[32], const uint8_t session[8], char qr[PAIRING_QR_LENGTH]);
bool pairing_validate(const PairingSettings *settings);
bool pairing_open(const uint8_t *bytes, size_t length, const uint8_t secret[32],
    const uint8_t session[8], PairingSettings *out, unsigned *comparison);
uint32_t pairing_crc(const uint8_t *bytes, size_t length);
typedef struct { PairingSettings destination; uint8_t secret[24]; } PairingInvitation;
bool pairing_invitation(const uint8_t *bytes, size_t length, PairingInvitation *out);
void pairing_claim_key(const PairingInvitation *invitation, const uint8_t secret[32], char public_hex[65], char comparison[PAIRING_COMPARISON_SIZE]);
bool pairing_approved(const uint8_t *bytes, size_t length, const PairingInvitation *invitation,
    const uint8_t secret[32], PairingSettings *out);
#endif

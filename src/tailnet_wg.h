#ifndef PLAYDATE_TAILNET_WG_H
#define PLAYDATE_TAILNET_WG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Single-peer WireGuard transport; initiates rekey while retaining previous keys. */
bool tn_wg_initialize(const uint8_t private_key[32], const uint8_t peer_public_key[32]);
bool tn_wg_ready(void);
bool tn_wg_rekey_due(void);

/* Packet producers return the written byte count, or -1 on failure. */
int tn_wg_handshake(uint8_t *out, size_t capacity);
int tn_wg_encrypt(const uint8_t *plaintext, size_t length, uint8_t *out, size_t capacity);

/* Returns plaintext bytes, 0 for handshake/cookie/keepalive, or -1 on rejection.
 * Plaintext includes WireGuard's zero padding; the IP header supplies its length.
 * After the first successful handshake, send an empty encrypted confirmation.
 * A consumed cookie requires another handshake attempt while tn_wg_ready is false.
 */
int tn_wg_receive(const uint8_t *packet, size_t length, uint8_t *plaintext, size_t capacity);

/* The caller supplies fresh random bytes and its monotonic/Unix clocks. */
void tn_wg_random(uint8_t *out, size_t length);
uint32_t tn_wg_now_ms(void);
uint64_t tn_wg_unix_ms(void);

#endif

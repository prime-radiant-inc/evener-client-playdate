#include "tailnet_wg.h"
#include "../vendor/wireguard/wireguard.h"
#include "../vendor/wireguard/crypto.h"
#include <string.h>

static struct wireguard_device device;
static union {
    struct message_handshake_initiation initiation;
    struct message_handshake_response response;
    struct message_cookie_reply cookie;
} handshake_packet;

static struct wireguard_peer *peer(void) {
    return &device.peers[0];
}

bool tn_wg_initialize(const uint8_t private_key[32], const uint8_t peer_public_key[32]) {
    crypto_zero(&device, sizeof(device));
    crypto_zero(&handshake_packet, sizeof(handshake_packet));
    if (!private_key || !peer_public_key) return false;
    wireguard_init();
    if (!wireguard_device_init(&device, private_key)) return false;
    return wireguard_peer_init(&device, peer(), peer_public_key, NULL);
}

bool tn_wg_ready(void) {
    struct wireguard_keypair *keypair = &peer()->curr_keypair;
    return device.valid && peer()->valid && keypair->valid && keypair->sending_valid &&
        keypair->sending_counter < REJECT_AFTER_MESSAGES &&
        !wireguard_expired(keypair->keypair_millis, REJECT_AFTER_TIME);
}

bool tn_wg_rekey_due(void) {
    struct wireguard_keypair *keypair = &peer()->curr_keypair;
    return !tn_wg_ready() || keypair->sending_counter >= REKEY_AFTER_MESSAGES ||
        wireguard_expired(keypair->keypair_millis, REKEY_AFTER_TIME);
}

int tn_wg_handshake(uint8_t *out, size_t capacity) {
    struct message_handshake_initiation *message = &handshake_packet.initiation;
    if (!out || capacity < sizeof(*message) || !device.valid || !peer()->valid) return -1;
    if (!wireguard_create_handshake_initiation(&device, peer(), message)) return -1;
    memcpy(peer()->handshake_mac1, message->mac1, WIREGUARD_COOKIE_LEN);
    peer()->handshake_mac1_valid = true;
    peer()->last_initiation_tx = tn_wg_now_ms();
    memcpy(out, message, sizeof(*message));
    return (int)sizeof(*message);
}

int tn_wg_receive(const uint8_t *packet, size_t length, uint8_t *plaintext, size_t capacity) {
    if (!packet || !device.valid || !peer()->valid) return -1;
    uint8_t type = wireguard_get_message_type(packet, length);
    if (type == MESSAGE_HANDSHAKE_RESPONSE) {
        struct message_handshake_response *message = &handshake_packet.response;
        memcpy(message, packet, sizeof(*message));
        if (message->receiver != peer()->handshake.local_index ||
            !wireguard_check_mac1(&device, packet, sizeof(*message) - 2 * WIREGUARD_COOKIE_LEN, message->mac1) ||
            !wireguard_process_handshake_response(&device, peer(), message)) return -1;
        wireguard_start_session(peer(), true);
        return 0;
    }
    if (type == MESSAGE_COOKIE_REPLY) {
        struct message_cookie_reply *message = &handshake_packet.cookie;
        memcpy(message, packet, sizeof(*message));
        if (!peer()->handshake.valid || message->receiver != peer()->handshake.local_index ||
            !wireguard_process_cookie_message(&device, peer(), message)) return -1;
        return 0;
    }
    if (type != MESSAGE_TRANSPORT_DATA) return -1;

    const size_t header_length = sizeof(struct message_transport_data);
    size_t encrypted_length = length - header_length;
    size_t plaintext_length = encrypted_length - WIREGUARD_AUTHTAG_LEN;
    if (plaintext_length > capacity || plaintext_length > 65536 || (plaintext_length && !plaintext)) return -1;
    uint32_t receiver = U8TO32_LITTLE(packet + 4);
    uint64_t counter = U8TO64_LITTLE(packet + 8);
    struct wireguard_keypair *keypair = get_peer_keypair_for_idx(peer(), receiver);
    if (!keypair || !keypair->receiving_valid || counter >= REJECT_AFTER_MESSAGES ||
        wireguard_expired(keypair->keypair_millis, REJECT_AFTER_TIME)) return -1;
    if (!wireguard_decrypt_packet(plaintext, packet + header_length, encrypted_length, counter, keypair) ||
        !wireguard_check_replay(keypair, counter)) return -1;
    keypair->last_rx = tn_wg_now_ms();
    peer()->last_rx = keypair->last_rx;
    keypair_update(peer(), keypair);
    return (int)plaintext_length;
}

int tn_wg_encrypt(const uint8_t *plaintext, size_t length, uint8_t *out, size_t capacity) {
    if (!tn_wg_ready() || !out || length > 65535 || (length && !plaintext)) return -1;
    const size_t header_length = sizeof(struct message_transport_data);
    size_t padded_length = (length + 15) & ~(size_t)15;
    size_t packet_length = header_length + padded_length + WIREGUARD_AUTHTAG_LEN;
    if (capacity < packet_length) return -1;
    struct wireguard_keypair *keypair = &peer()->curr_keypair;
    if (length) memmove(out + header_length, plaintext, length);
    memset(out + header_length + length, 0, padded_length - length);
    memset(out, 0, header_length);
    out[0] = MESSAGE_TRANSPORT_DATA;
    U32TO8_LITTLE(out + 4, keypair->remote_index);
    U64TO8_LITTLE(out + 8, keypair->sending_counter);
    wireguard_encrypt_packet(out + header_length, out + header_length, padded_length, keypair);
    keypair->last_tx = tn_wg_now_ms();
    peer()->last_tx = keypair->last_tx;
    return (int)packet_length;
}

#include "tailnet_wg.h"
#include "../vendor/wireguard/wireguard-platform.h"

uint32_t wireguard_sys_now(void) {
    return tn_wg_now_ms();
}

void wireguard_random_bytes(void *bytes, size_t size) {
    tn_wg_random(bytes, size);
}

void wireguard_tai64n_now(uint8_t *output) {
    uint64_t milliseconds = tn_wg_unix_ms();
    uint64_t seconds = UINT64_C(0x400000000000000a) + milliseconds / 1000;
    uint32_t nanoseconds = (uint32_t)(milliseconds % 1000) * 1000000;
    for (unsigned i = 0; i < 8; i++) output[i] = (uint8_t)(seconds >> (56 - 8 * i));
    for (unsigned i = 0; i < 4; i++) output[i + 8] = (uint8_t)(nanoseconds >> (24 - 8 * i));
}

bool wireguard_is_under_load(void) {
    return false;
}

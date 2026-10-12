#ifndef EVENER_SETTINGS_H
#define EVENER_SETTINGS_H
#include "pairing_protocol.h"
bool settings_initialize(void);
bool settings_ready(void);
void settings_bootstrap_begin(const PairingSettings *destination);
void settings_bootstrap_end(void);
bool settings_save(const PairingSettings *settings);
bool settings_cancel_pending(void);
bool settings_bind_peer(const char *stable_id);
const char *settings_host(void);
bool settings_bundled_profile(void);
void settings_path(char *out, size_t capacity, const char *leaf);
void settings_connection_path(char *out, size_t capacity, const char *leaf);
#endif

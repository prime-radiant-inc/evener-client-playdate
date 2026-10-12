#ifndef EVENER_PAIRING_HTTP_H
#define EVENER_PAIRING_HTTP_H
#include "pairing_protocol.h"
enum PairingHTTPResult { PAIRING_HTTP_WAIT, PAIRING_HTTP_APPROVED, PAIRING_HTTP_ENDED };
void pairing_http_start(const PairingInvitation *invitation, const char public_key[65], unsigned comparison);
void pairing_http_stop(void);
enum PairingHTTPResult pairing_http_update(uint8_t *envelope, size_t capacity, size_t *length);
const char *pairing_http_status(void);
#endif

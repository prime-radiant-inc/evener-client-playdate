#ifndef PLAYDATE_TAILNET_H
#define PLAYDATE_TAILNET_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    char derp_host[256];
    uint8_t node_private[32], node_public[32], peer_public[32], derp_public[32], seed[32];
    uint8_t disco_private[32], disco_public[32], peer_disco[32];
    uint8_t node_ip[4], peer_ip[4];
    uint16_t peer_port, derp_port, derp_region;
} TailnetConfig;

typedef enum { TN_STOPPED, TN_CONNECTING, TN_READY, TN_FAILED } TailnetState;
typedef enum { TN_TCP_CLOSED, TN_TCP_CONNECTING, TN_TCP_OPEN, TN_TCP_EOF, TN_TCP_FAILED } TailnetTCPState;

/* Single instance; call everything on the game loop, never inside IRQ callbacks.
 * The platform owns verified TLS sockets and connection retry. Start after TLS
 * connects. Outer failure ends this TCP generation; call start on a fresh socket.
 * Config is copied. Platform RNG must supply fresh cryptographic randomness.
 */
bool tn_configure(TailnetConfig *out, const char *json, size_t length);
bool tn_start(const TailnetConfig *config);
void tn_stop(void);
void tn_fail(const char *reason);
TailnetState tn_state(void);
const char *tn_error(void);
void tn_tick(void);

/* Input returns consumed bytes; retain and retry the remainder after tick.
 * Output is borrowed until any mutation; consume only successfully written bytes.
 * A TLS WANT_WRITE retry must keep its own stable copy of the submitted bytes.
 */
size_t tn_input(const uint8_t *bytes, size_t length);
const uint8_t *tn_output(size_t *length);
void tn_consume(size_t length);

/* Open one configured peer TCP stream once TN_READY. Write returns bytes copied
 * into TCP ownership (0 = backpressure, -1 = no usable connection). Read returns
 * copied bytes (0 = currently empty). EOF/failed may still have readable bytes.
 * pending counts unacknowledged accepted writes. Close abandons this generation;
 * application mutation identity/recovery remains the Hub owner's responsibility.
 */
bool tn_open(void);
int tn_write(const void *bytes, size_t length);
size_t tn_read(void *bytes, size_t capacity);
size_t tn_available(void);
size_t tn_pending(void);
TailnetTCPState tn_tcp_state(void);
void tn_close(void);

uint32_t tn_now_ms(void);
uint64_t tn_unix_ms(void);
void tn_random(uint8_t *bytes, size_t length);
void tn_log(const char *message);
#endif

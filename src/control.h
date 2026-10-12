#ifndef PLAYDATE_CONTROL_H
#define PLAYDATE_CONTROL_H
#include "tailnet.h"

#define CONTROL_HOST "controlplane.tailscale.com"
#define CONTROL_MAX_MAP (256 * 1024)

typedef struct {
    TailnetConfig tailnet;
    uint8_t machine_private[32], control_public[32], staged_node_private[32];
    char peer_id[64], node_id[64], hostname[128], enrolled_os[32];
    uint64_t node_expiry_ms, peer_expiry_ms;
    unsigned derp_region;
} ControlConfig;

typedef enum {
    CONTROL_STOPPED, CONTROL_KEY_FETCH, CONTROL_KEY_READY, CONTROL_CONNECTING,
    CONTROL_REGISTERING, CONTROL_MAPPING, CONTROL_READY, CONTROL_REAUTH, CONTROL_FAILED
} ControlState;

/* Called BEFORE any request containing a replacement node key. Return true only
 * after atomically persisting config, including staged_node_private. On restart,
 * reload it and retry the same registration; never generate another candidate.
 * READY result must be durably saved before activating its tailnet identity.
 * An interrupted registration may already have succeeded: retain BOTH old and
 * staged keys until a successful authenticated registration and map reconcile it.
 */
typedef bool (*ControlStage)(const ControlConfig *config, void *context);

bool control_configure(ControlConfig *out, const char *json, size_t length);
/* Generate an isolated identity; the caller must persist it before registration. */
void control_create_identity(ControlConfig *out, uint16_t peer_port);
/* A fresh identity learns stable IDs from its authenticated map. */
void control_set_peer_target(const char *host);
void control_set_auth_url(const char *url);
/* Both starts require a freshly connected, hostname-verified TLS stream to
 * CONTROL_HOST:443. KEY_READY result is only a verified control key update;
 * close that stream and reconnect before control_start. All calls are game-loop
 * only, single instance. Input consumes a prefix; retain unconsumed remainder.
 * Output is borrowed until mutation. Consume only bytes accepted by the socket.
 * Platform supplies tn_random, tn_now_ms and tn_unix_ms. Stop clears secrets.
 */
bool control_start_key(const ControlConfig *config);
bool control_start(const ControlConfig *config, ControlStage stage, void *context);
/* Keep current-key maps fresh while a staged renewal awaits authorization.
 * No new candidate is generated, and READY preserves the staged private key. */
bool control_start_current(const ControlConfig *config);
void control_stop(void);
void control_fail(const char *reason);
void control_tick(void);
size_t control_input(const uint8_t *bytes, size_t length);
const uint8_t *control_output(size_t *length);
void control_consume(size_t length);
ControlState control_state(void);
const char *control_error(void);
/* Valid only for KEY_READY or READY. Reauth preserves original identity and
 * staged candidate; control_auth_url returns the authenticated enrollment URL.
 * Retry fresh map snapshots every 60 s and immediately after transport reconnect.
 * Expired/unauthorized peers must not be used; REAUTH requires one-shot enrollment.
 */
const ControlConfig *control_result(void);
const char *control_auth_url(void);
bool control_authorized(void);
#endif

#include "app.h"
#include "tailnet_device.h"
#include "tailnet.h"
#include "control.h"
#include "net_socket.h"
#include "random.h"
#include "storage.h"
#include "settings.h"
#include <stdio.h>
#include <string.h>

typedef struct {
    NetSocket *socket;
    uint8_t input[4096];
    size_t length, offset;
    bool started;
    uint32_t opened;
} Channel;
typedef struct { char host[256]; int port; bool allowed, pending, denied; } Access;
typedef struct { uint32_t magic, version; ControlConfig config; char auth_url[1024]; bool mapped; uint32_t checksum; } SavedConfig;
static ControlConfig current;
static Channel relay, controller;
static Access relay_access, control_access;
static bool mapped, enabled, reauth, renewal_pending, current_only, map_phase, relay_pinning;
static bool maintenance, voice_active;
static bool replacing_identity;
static ControlConfig replacement;
static char auth_url[1024], notice[1100];
static uint32_t renewal_retry;
static uint32_t relay_retry, control_retry, relay_delay = 1000;
static char status[192] = "Connecting to tailnet...";
static char pin_bytes[8192], pin_request[512];
static char state_path[64], stage_path[64];
static size_t pin_length, pin_sent, pin_request_length;

uint32_t tn_now_ms(void) { return pd->system->getCurrentTimeMilliseconds(); }
uint64_t tn_unix_ms(void) {
    unsigned ms; unsigned seconds = pd->system->getSecondsSinceEpoch(&ms);
    return ((uint64_t)seconds + 946684800) * 1000 + ms;
}
void tn_random(uint8_t *bytes, size_t length) { random_fill(bytes, length); }
void tn_log(const char *message) { app_log("Tailnet: %s", message); }
static bool due(uint32_t deadline) { return (int32_t)(tn_now_ms() - deadline) >= 0; }
static uint32_t checksum(const void *bytes, size_t length) {
    const uint8_t *p = bytes; uint32_t sum = 2166136261u;
    while (length--) sum = (sum ^ *p++) * 16777619u;
    return sum;
}
static bool read_saved(const char *path, SavedConfig *saved) {
    SDFile *file = pd->file->open(path, kFileReadData);
    if (!file) return false;
    int n = pd->file->read(file, saved, sizeof(*saved));
    pd->file->close(file);
    return n == sizeof(*saved) && saved->magic == 0x544e4554 && saved->version == 2 &&
        saved->checksum == checksum(saved, offsetof(SavedConfig, checksum));
}
static bool persist_config(const ControlConfig *value, bool map_received) {
    static SavedConfig saved;
    memset(&saved, 0, sizeof(saved));
    saved.magic = 0x544e4554; saved.version = 2; saved.config = *value; saved.mapped = map_received;
    snprintf(saved.auth_url, sizeof(saved.auth_url), "%s", auth_url);
    saved.checksum = checksum(&saved, offsetof(SavedConfig, checksum));
    SDFile *file = pd->file->open(stage_path, kFileWrite);
    if (!file) return false;
    bool ok = pd->file->write(file, &saved, sizeof(saved)) == sizeof(saved) && pd->file->flush(file) >= 0;
    if (pd->file->close(file)) ok = false;
    if (!ok || !storage_replace(stage_path, state_path)) return false;
    current = *value; mapped = map_received;
    return true;
}
static bool persist(const ControlConfig *value, void *context) {
    (void)context;
    return persist_config(value, mapped);
}
bool tailnet_device_initialize(void) {
    tailnet_device_stop();
    memset(&current, 0, sizeof(current));
    mapped = reauth = renewal_pending = current_only = relay_pinning = false;
    replacing_identity = false;
    auth_url[0] = 0;
    settings_connection_path(state_path, sizeof(state_path), "tailnet-state.bin");
    settings_connection_path(stage_path, sizeof(stage_path), "tailnet-state.tmp");
    control_set_peer_target(settings_host()); control_set_auth_url("");
    static char json[8192];
    SDFile *file = settings_bundled_profile() ? pd->file->open("tailnet.json", kFileRead) : NULL;
    bool bundled = false;
    if (file) {
        int n = pd->file->read(file, json, sizeof(json));
        pd->file->close(file);
        bundled = n > 0 && n < sizeof(json) && control_configure(&current, json, (size_t)n);
        if (!bundled) return false;
    }
    memset(json, 0, sizeof(json));
    static SavedConfig saved;
    bool staged = read_saved(stage_path, &saved);
    bool valid = staged || read_saved(state_path, &saved);
    // A deliberately replaced provisioning seed supersedes saved state.
    // Ordinary refreshes and native renewal preserve it so current keys win.
    if (valid && saved.magic == 0x544e4554 && saved.version == 2 &&
        saved.checksum == checksum(&saved, offsetof(SavedConfig, checksum)) &&
        (!bundled || !memcmp(saved.config.tailnet.seed, current.tailnet.seed, sizeof(current.tailnet.seed)))) {
        current = saved.config; mapped = saved.mapped;
        if (memchr(saved.auth_url, 0, sizeof(saved.auth_url)))
            snprintf(auth_url, sizeof(auth_url), "%s", saved.auth_url);
        renewal_pending = auth_url[0] != 0;
        if (staged && !storage_replace(stage_path, state_path)) return false;
    } else if (!bundled) {
        FileStat metadata;
        if (!pd->file->stat(state_path, &metadata) || !pd->file->stat(stage_path, &metadata)) return false;
        control_create_identity(&current, (uint16_t)config.hub_port);
        if (!persist_config(&current, false)) return false;
    }
    if (!settings_bind_peer(current.peer_id)) return false;
    if (mapped) {
        snprintf(config.hub_host, sizeof(config.hub_host), "%u.%u.%u.%u", current.tailnet.peer_ip[0],
            current.tailnet.peer_ip[1], current.tailnet.peer_ip[2], current.tailnet.peer_ip[3]);
        config.hub_port = current.tailnet.peer_port;
    }
    control_set_auth_url(auth_url);
    snprintf(status, sizeof(status), "%s", auth_url[0] ? "Tailscale sign-in needed." : "Connecting to tailnet...");
    return true;
}
bool tailnet_device_new_identity(void) {
    static char previous_url[sizeof(auth_url)];
    if (!replacing_identity) {
        control_create_identity(&replacement, (uint16_t)config.hub_port);
        if (settings_bundled_profile()) memcpy(replacement.tailnet.seed, current.tailnet.seed, 32);
        replacing_identity = true;
    }
    tailnet_device_stop();
    snprintf(previous_url, sizeof(previous_url), "%s", auth_url); auth_url[0] = 0;
    if (!persist_config(&replacement, false)) { snprintf(auth_url, sizeof(auth_url), "%s", previous_url); return false; }
    settings_bind_peer("");
    auth_url[0] = 0; reauth = renewal_pending = false;
    replacing_identity = false; memset(&replacement, 0, sizeof(replacement));
    control_set_auth_url(""); control_set_peer_target(settings_host());
    tailnet_device_stop();
    return true;
}
static void disconnect(Channel *channel) {
    net_close(channel->socket);
    memset(channel, 0, sizeof(*channel));
}
static void permission(bool allowed, void *context) {
    Access *access = context;
    access->pending = false; access->allowed = allowed; access->denied = !allowed;
}
static bool access_allowed(Access *access, const char *host, int port, const char *purpose) {
    if (relay_access.pending || control_access.pending) return false;
    if (strcmp(access->host, host) || access->port != port) {
        memset(access, 0, sizeof(*access));
        snprintf(access->host, sizeof(access->host), "%s", host); access->port = port;
    }
    if (access->allowed) return true;
    if (access->denied) { snprintf(status, sizeof(status), "Allow Tailscale network access in Settings."); return false; }
    access->pending = true;
    enum accessReply reply = pd->network->tcp->requestAccess(host, port, true, purpose, permission, access);
    if (reply != kAccessAsk) permission(reply == kAccessAllow, access);
    return access->allowed;
}
static bool socket_open(Channel *channel, const char *host, int port) {
    channel->socket = net_open(host, port, true);
    channel->opened = tn_now_ms();
    return channel->socket != NULL;
}
static bool flush_output(Channel *channel, bool control) {
    for (unsigned budget = 0; budget < 8 && net_state(channel->socket) == NS_OPEN; budget++) {
        size_t count;
        const uint8_t *bytes = control ? control_output(&count) : tn_output(&count);
        if (!count || net_pending(channel->socket) > 16384) break;
        if (count > 2048) count = 2048;
        int n = net_write(channel->socket, bytes, count);
        if (n < 0) return false;
        if (!n) break;
        if (control) control_consume((size_t)n); else tn_consume((size_t)n);
    }
    return true;
}
void tailnet_device_flush(void) {
    // Draining Hub bytes returns TCP receive credit. Send that credit during
    // the same game update instead of leaving the peer waiting another frame.
    if (!maintenance && !relay_pinning && relay.started && net_state(relay.socket) == NS_OPEN &&
        !flush_output(&relay, false)) tn_fail(net_error(relay.socket));
}
static bool pump(Channel *channel, bool control) {
    if (!flush_output(channel, control)) return false;
    for (unsigned budget = 0; budget < 16; budget++) {
        if (channel->offset < channel->length) {
            const uint8_t *bytes = channel->input + channel->offset;
            size_t count = channel->length - channel->offset;
            size_t n = control ? control_input(bytes, count) : tn_input(bytes, count);
            if (!n) break;
            channel->offset += n;
            if (channel->offset < channel->length) continue;
        }
        channel->offset = channel->length = 0;
        int n = net_read(channel->socket, channel->input, sizeof(channel->input));
        if (n < 0) return false;
        if (!n) break;
        channel->length = (size_t)n;
    }
    if (control) control_tick(); else tn_tick();
    enum NetState state = net_state(channel->socket);
    return (state != NS_CLOSED && state != NS_FAILED) ||
        channel->length > channel->offset || net_available(channel->socket);
}
static bool same_transport(const TailnetConfig *a, const TailnetConfig *b) {
    return !strcmp(a->derp_host, b->derp_host) && a->derp_port == b->derp_port && a->peer_port == b->peer_port &&
        !memcmp(a->node_private, b->node_private, 32) && !memcmp(a->peer_public, b->peer_public, 32) &&
        !memcmp(a->derp_public, b->derp_public, 32) && !memcmp(a->node_ip, b->node_ip, 4) && !memcmp(a->peer_ip, b->peer_ip, 4) &&
        !memcmp(a->disco_private, b->disco_private, 32) && !memcmp(a->disco_public, b->disco_public, 32) &&
        !memcmp(a->peer_disco, b->peer_disco, 32) && a->derp_region == b->derp_region;
}
static void control_update(void) {
    if (voice_active) return;
    if (!maintenance) {
        if (!due(control_retry) || !access_allowed(&control_access, CONTROL_HOST, 443,
            "Maintain this Playdate's Tailscale identity and Hub connection.")) return;
        maintenance = true;
        // These TLS handshakes need the device's full networking resources.
        // Keep the relay paused across both control connections, including the
        // accepted close between the key fetch and authenticated map request.
        if (relay.socket) {
            tn_fail("Refreshing Tailscale connection...");
            disconnect(&relay);
        }
    }
    if (!controller.socket) {
        if (!net_idle() || !due(control_retry)) return;
        if (!socket_open(&controller, CONTROL_HOST, 443)) { control_retry = tn_now_ms() + 10000; return; }
    }
    if (net_waiting(controller.socket)) controller.opened = tn_now_ms();
    if (net_state(controller.socket) == NS_OPEN && !controller.started) {
        controller.started = true;
        control_set_auth_url(auth_url);
        if (map_phase) {
            current_only = renewal_pending && !due(renewal_retry);
            if (current_only) control_start_current(&current);
            else control_start(&current, persist, NULL);
        } else control_start_key(&current);
    }
    if (controller.started && !pump(&controller, true) &&
        control_state() != CONTROL_READY && control_state() != CONTROL_KEY_READY && control_state() != CONTROL_REAUTH)
        control_fail(net_error(controller.socket));
    if ((net_state(controller.socket) == NS_FAILED || tn_now_ms() - controller.opened > 45000) &&
        control_state() != CONTROL_READY && control_state() != CONTROL_KEY_READY && control_state() != CONTROL_REAUTH)
        control_fail(net_error(controller.socket));
    ControlState state = control_state();
    if (state == CONTROL_KEY_READY) {
        if (!persist(control_result(), NULL)) { control_fail("Could not save Tailscale configuration"); return; }
        disconnect(&controller); control_stop(); map_phase = true; control_retry = tn_now_ms();
    } else if (state == CONTROL_READY) {
        if (!settings_bind_peer(control_result()->peer_id)) { control_fail("Could not save Hub data ownership"); return; }
        bool changed = !same_transport(&current.tailnet, &control_result()->tailnet);
        uint8_t candidate = 0;
        for (unsigned i = 0; i < 32; i++) candidate |= control_result()->staged_node_private[i];
        if (!candidate) { auth_url[0] = 0; renewal_pending = false; }
        if (!persist_config(control_result(), true)) { control_fail("Could not save Tailscale configuration"); return; }
        snprintf(config.hub_host, sizeof(config.hub_host), "%u.%u.%u.%u", current.tailnet.peer_ip[0],
            current.tailnet.peer_ip[1], current.tailnet.peer_ip[2], current.tailnet.peer_ip[3]);
        config.hub_port = current.tailnet.peer_port;
        disconnect(&controller); control_stop(); map_phase = false; control_retry = tn_now_ms() + 60000;
        maintenance = false;
        reauth = false;
        if (changed) {
            tn_fail("Tailscale configuration refreshed. Reconnecting...");
            disconnect(&relay); relay_retry = tn_now_ms();
        }
        app_log("Tailscale node and Hub map refreshed");
    } else if (state == CONTROL_FAILED || state == CONTROL_REAUTH) {
        app_log("Tailscale maintenance: %s", controller.started ? control_error() : net_error(controller.socket));
        if (!mapped) snprintf(status, sizeof(status), "%s", control_error());
        if (control_authorized() && !current_only && auth_url[0]) {
            // An approved identity can still lack access to the requested Hub.
            // Keep that failure visible instead of showing an expired sign-in QR.
            auth_url[0] = 0; renewal_pending = reauth = false;
            if (!persist(&current, NULL)) app_log("Could not save completed sign-in");
        }
        if (state == CONTROL_REAUTH) {
            uint8_t candidate = 0;
            for (unsigned i = 0; i < 32; i++) candidate |= current.staged_node_private[i];
            renewal_pending = !current_only && candidate &&
                current.node_expiry_ms && tn_unix_ms() < current.node_expiry_ms;
            reauth = !renewal_pending;
            snprintf(auth_url, sizeof(auth_url), "%s", control_auth_url());
            if (!persist(&current, NULL)) app_log("Could not save Tailscale sign-in notice");
            renewal_retry = tn_now_ms() + 15 * 60 * 1000;
            if (reauth) {
                snprintf(status, sizeof(status), "Tailscale sign-in needed. See the footer.");
                tn_fail(status); disconnect(&relay);
            }
        }
        disconnect(&controller); control_stop(); map_phase = false;
        maintenance = false;
        // Confirm that the current key is still authorized immediately after a
        // staged renewal asks for sign-in. Keep its maps fresh while waiting.
        control_retry = tn_now_ms() + (renewal_pending ? 1000 : auth_url[0] ? 10000 : 60000);
    }
}
void tailnet_device_set_voice_active(bool active) {
    if (active && !voice_active && maintenance) {
        // Capture and its saved audio have priority over routine maintenance.
        // Any staged renewal key is already durable and remains available.
        disconnect(&controller); control_stop(); map_phase = maintenance = false;
        control_retry = tn_now_ms() + (mapped ? 60000 : 0);
    }
    voice_active = active;
}
static bool has_pin(void) {
    uint8_t any = 0; for (unsigned i = 0; i < 32; i++) any |= current.tailnet.derp_public[i];
    return any != 0;
}
static bool pin_relay(void) {
    if (tn_now_ms() - relay.opened > 20000) return false;
    if (pin_sent < pin_request_length) {
        int n = net_write(relay.socket, pin_request + pin_sent, pin_request_length - pin_sent);
        if (n < 0) return false;
        pin_sent += (size_t)n;
    }
    int n = net_read(relay.socket, pin_bytes + pin_length, sizeof(pin_bytes) - 1 - pin_length);
    if (n < 0) return false;
    pin_length += (size_t)n; pin_bytes[pin_length] = 0;
    bool ended = net_state(relay.socket) == NS_CLOSED || net_state(relay.socket) == NS_FAILED;
    char *end = strstr(pin_bytes, "\r\n\r\n");
    if (!end) return !ended && pin_length < sizeof(pin_bytes) - 1;
    if (strncmp(pin_bytes, "HTTP/1.1 101", 12) && strncmp(pin_bytes, "HTTP/1.0 101", 12)) return false;
    size_t header = (size_t)(end + 4 - pin_bytes);
    if (pin_length - header < 45) return !ended;
    const uint8_t *frame = (uint8_t *)pin_bytes + header;
    if (frame[0] != 1 || frame[1] || frame[2] || frame[3] || frame[4] != 40 ||
        memcmp(frame + 5, "DERP\xf0\x9f\x94\x91", 8)) return false;
    memcpy(current.tailnet.derp_public, frame + 13, 32);
    if (!persist(&current, NULL)) return false;
    disconnect(&relay); relay_pinning = false; relay_retry = tn_now_ms();
    return true;
}
void tailnet_device_start(void) {
    if (!enabled) app_log("Native tailnet start; UTC seconds %lu", (unsigned long)(tn_unix_ms() / 1000));
    enabled = true;
}
void tailnet_device_stop(void) {
    enabled = false;
    disconnect(&relay); disconnect(&controller);
    tn_stop(); control_stop(); map_phase = maintenance = voice_active = false;
    relay_retry = control_retry = tn_now_ms();
}
const char *tailnet_device_status(void) { return status; }
const char *tailnet_device_auth_url(void) { return auth_url; }
const char *tailnet_device_notice(void) {
    if (auth_url[0]) {
        const char *url = !strncmp(auth_url, "https://", 8) ? auth_url + 8 : auth_url;
        snprintf(notice, sizeof(notice), "Sign in: %s", url);
        return notice;
    }
    return reauth ? "Tailscale authorization needed. Check the device in admin." : "";
}
void tailnet_device_update(void) {
    net_tick();
    if (!enabled) return;
    control_update();
    if (maintenance || reauth || !mapped) return;
    uint64_t now = tn_unix_ms();
    if ((current.node_expiry_ms && now >= current.node_expiry_ms) ||
        (current.peer_expiry_ms && now >= current.peer_expiry_ms)) {
        snprintf(status, sizeof(status), "Refreshing expired Tailscale identity...");
        if (relay.socket) { disconnect(&relay); tn_fail(status); }
        return;
    }
    if (!relay.socket) {
        // An established relay can coexist with transcription, but its TLS
        // handshake must wait until transcription has released the SDK slot.
        if (voice_active || !net_idle() || !due(relay_retry) || !access_allowed(&relay_access, current.tailnet.derp_host, current.tailnet.derp_port,
            "Connect directly to your Evener Hub through Tailscale's encrypted relay.")) return;
        if (!socket_open(&relay, current.tailnet.derp_host, current.tailnet.derp_port)) { relay_retry = tn_now_ms() + 1000; return; }
        snprintf(status, sizeof(status), "Connecting through Tailscale...");
    }
    if (net_waiting(relay.socket)) relay.opened = tn_now_ms();
    bool ok = true;
    if (net_state(relay.socket) == NS_OPEN && !relay.started) {
        relay.started = true;
        relay_pinning = !has_pin();
        if (relay_pinning) {
            pin_length = pin_sent = 0;
            pin_request_length = (size_t)snprintf(pin_request, sizeof(pin_request),
                "GET /derp HTTP/1.1\r\nHost: %s\r\nConnection: Upgrade\r\nUpgrade: DERP\r\n\r\n", current.tailnet.derp_host);
        } else ok = tn_start(&current.tailnet);
    }
    if (relay.started && ok) ok = relay_pinning ? pin_relay() : pump(&relay, false);
    if (!relay.socket) return;
    if (net_state(relay.socket) == NS_FAILED || (net_state(relay.socket) == NS_CONNECTING && tn_now_ms() - relay.opened > 25000)) ok = false;
    if ((!relay_pinning && relay.started && tn_state() == TN_FAILED) || !ok) {
        snprintf(status, sizeof(status), "%s", tn_state() == TN_FAILED ? tn_error() : net_error(relay.socket));
        if (tn_state() != TN_FAILED) tn_fail(status);
        disconnect(&relay);
        relay_retry = tn_now_ms() + relay_delay;
        if (relay_delay < 16000) relay_delay *= 2;
        control_retry = tn_now_ms();
    } else if (!relay_pinning && tn_state() == TN_READY) {
        relay_delay = 1000;
        snprintf(status, sizeof(status), "Connected through Tailscale");
    }
}

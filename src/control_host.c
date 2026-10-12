/* Opt-in official control protocol qualification; no node use without exclusive
 * ownership. Binary snapshots contain credentials and must remain ignored. */
#include "control.h"
#include "json.h"
#include <openssl/ssl.h>
#include <openssl/rand.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netdb.h>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#ifdef CONTROL_DIAGNOSTICS
void control_diagnostic_map(const char *bytes, size_t length) {
    const char *path = getenv("PLAYDATE_CONTROL_MAP_FILE");
    if (!path) return;
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0) { if (write(fd, bytes, length) != (ssize_t)length) fputs("Diagnostic map write failed\n", stderr); close(fd); }
}
#endif
static ControlConfig config;
static char configuration[16384];
static uint8_t input[4096], pending[4096];
void tn_log(const char *message) { fprintf(stderr, "%s\n", message); }
uint32_t tn_now_ms(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)((uint64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000);
}
uint64_t tn_unix_ms(void) {
    struct timeval t; gettimeofday(&t, NULL); return (uint64_t)t.tv_sec * 1000 + t.tv_usec / 1000;
}
void tn_random(uint8_t *bytes, size_t length) { if (length > 2147483647 || RAND_bytes(bytes, (int)length) != 1) abort(); }
static bool save(const ControlConfig *snapshot, const char *path) {
    if (!path) return false;
    char *temp = malloc(strlen(path) + 16);
    if (!temp) return false;
    sprintf(temp, "%s.XXXXXX", path);
    int fd = mkstemp(temp); bool ok = fd >= 0;
    if (ok) ok = write(fd, snapshot, sizeof(*snapshot)) == sizeof(*snapshot) && fsync(fd) == 0;
    if (fd >= 0 && close(fd)) ok = false;
    if (ok) ok = rename(temp, path) == 0;
    unlink(temp); free(temp); return ok;
}
static void hexkey(char out[65], const uint8_t *key) {
    for (int i = 0; i < 32; i++) sprintf(out + i * 2, "%02x", key[i]);
}
static void expiry_json(char out[64], uint64_t ms) {
    if (!ms) { strcpy(out, "null"); return; }
    time_t seconds = (time_t)(ms / 1000); struct tm value; gmtime_r(&seconds, &value);
    strftime(out, 64, "\"%Y-%m-%dT%H:%M:%SZ\"", &value);
}
static bool save_json(const ControlConfig *value, const char *path) {
    char keys[11][65], staged_field[128], node_expiry[64], peer_expiry[64], host[520], hostname[260], os[68], node_id[132], peer_id[132];
    const uint8_t *raw[] = {value->tailnet.node_private, value->tailnet.node_public, value->tailnet.peer_public, value->tailnet.derp_public, value->tailnet.seed, value->machine_private, value->control_public, value->staged_node_private, value->tailnet.disco_private, value->tailnet.disco_public, value->tailnet.peer_disco};
    for (int i = 0; i < 11; i++) hexkey(keys[i], raw[i]);
    uint8_t has_staged = 0; for (int i = 0; i < 32; i++) has_staged |= value->staged_node_private[i];
    if (has_staged) snprintf(staged_field, sizeof(staged_field), "  \"staged_node_private_key\":\"%s\",\n", keys[7]);
    else staged_field[0] = 0;
    expiry_json(node_expiry, value->node_expiry_ms); expiry_json(peer_expiry, value->peer_expiry_ms);
    if (!json_quote(value->tailnet.derp_host, host, sizeof(host)) || !json_quote(value->hostname, hostname, sizeof(hostname)) || !json_quote(value->enrolled_os, os, sizeof(os)) || !json_quote(value->node_id, node_id, sizeof(node_id)) || !json_quote(value->peer_id, peer_id, sizeof(peer_id))) return false;
    char *text = malloc(4096), *temp = malloc(strlen(path) + 16);
    if (!text || !temp) { free(text); free(temp); return false; }
    int n = snprintf(text, 4096, "{\n%s  \"node_private_key\":\"%s\",\n  \"node_public_key\":\"%s\",\n  \"peer_public_key\":\"%s\",\n  \"derp_public_key\":\"%s\",\n  \"seed\":\"%s\",\n  \"machine_private_key\":\"%s\",\n  \"control_public_key\":\"%s\",\n  \"disco_private_key\":\"%s\",\n  \"disco_public_key\":\"%s\",\n  \"peer_disco_key\":\"%s\",\n  \"node_ip\":\"%u.%u.%u.%u\",\n  \"peer_ip\":\"%u.%u.%u.%u\",\n  \"peer_port\":%u,\n  \"derp_host\":%s,\n  \"derp_port\":%u,\n  \"node_id\":%s,\n  \"peer_id\":%s,\n  \"hostname\":%s,\n  \"enrolled_os\":%s,\n  \"node_key_expiry\":%s,\n  \"peer_key_expiry\":%s,\n  \"derp_region_id\":%u\n}\n", staged_field, keys[0], keys[1], keys[2], keys[3], keys[4], keys[5], keys[6], keys[8], keys[9], keys[10], value->tailnet.node_ip[0], value->tailnet.node_ip[1], value->tailnet.node_ip[2], value->tailnet.node_ip[3], value->tailnet.peer_ip[0], value->tailnet.peer_ip[1], value->tailnet.peer_ip[2], value->tailnet.peer_ip[3], value->tailnet.peer_port, host, value->tailnet.derp_port, node_id, peer_id, hostname, os, node_expiry, peer_expiry, value->derp_region);
    sprintf(temp, "%s.XXXXXX", path); int fd = mkstemp(temp); bool ok = fd >= 0 && n > 0 && n < 4096;
    if (ok) ok = write(fd, text, (size_t)n) == n && fsync(fd) == 0;
    if (fd >= 0 && close(fd)) ok = false;
    if (ok) ok = rename(temp, path) == 0;
    unlink(temp); memset(text, 0, 4096); memset(keys, 0, sizeof(keys)); free(text); free(temp); return ok;
}
static bool stage(const ControlConfig *snapshot, void *path) { return save(snapshot, path); }
static bool exchange(SSL_CTX *ctx, bool key, const char *stage_path) {
    struct addrinfo hints = {.ai_socktype = SOCK_STREAM, .ai_family = AF_INET}, *addresses = NULL;
    if (getaddrinfo(CONTROL_HOST, "443", &hints, &addresses)) { fputs("Control DNS failed\n", stderr); return false; }
    int fd = -1;
    for (struct addrinfo *a = addresses; a; a = a->ai_next) {
        fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd < 0) continue;
        struct timeval timeout = {.tv_sec = 15}; setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)); setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        if (!connect(fd, a->ai_addr, a->ai_addrlen)) break;
        close(fd); fd = -1;
    }
    freeaddrinfo(addresses);
    if (fd < 0) { perror("Control connect"); return false; }
    SSL *ssl = SSL_new(ctx);
    if (!ssl) { close(fd); return false; }
    SSL_set_tlsext_host_name(ssl, CONTROL_HOST); SSL_set1_host(ssl, CONTROL_HOST); SSL_set_fd(ssl, fd);
    bool ok = false;
    if (SSL_connect(ssl) != 1) { fputs("Control verified TLS failed\n", stderr); goto done; }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    bool started;
    if (key) started = control_start_key(&config);
    else if (getenv("PLAYDATE_CONTROL_CURRENT_ONLY")) started = control_start_current(&config);
    else started = control_start(&config, stage, (void *)stage_path);
    if (!started) goto done;
    size_t pending_length = 0; ControlState previous = CONTROL_STOPPED;
    while (control_state() != CONTROL_READY && control_state() != CONTROL_KEY_READY && control_state() != CONTROL_REAUTH && control_state() != CONTROL_FAILED) {
        if (previous != control_state()) { previous = control_state(); fprintf(stderr, "Control state %d\n", previous); }
        size_t available; const uint8_t *bytes = control_output(&available);
        if (!pending_length && available) { pending_length = available < sizeof(pending) ? available : sizeof(pending); memcpy(pending, bytes, pending_length); }
        if (pending_length) {
            int n = SSL_write(ssl, pending, (int)pending_length);
            if (n > 0) { control_consume((size_t)n); pending_length = 0; }
            else { int error = SSL_get_error(ssl, n); if (error != SSL_ERROR_WANT_READ && error != SSL_ERROR_WANT_WRITE) { control_fail("Control TLS write failed"); break; } }
        }
        int n = SSL_read(ssl, input, sizeof(input));
        if (n > 0) { if (control_input(input, (size_t)n) != (size_t)n) { control_fail("Host input overflow"); break; } }
        else { int error = SSL_get_error(ssl, n); if (error != SSL_ERROR_WANT_READ && error != SSL_ERROR_WANT_WRITE) { control_fail("Control TLS closed"); break; } }
        if (!key && control_state() == CONTROL_MAPPING && getenv("PLAYDATE_CONTROL_STOP_AFTER_REGISTER")) {
            control_fail("Opt-in interruption after accepted registration"); break;
        }
        control_tick(); struct pollfd pfd = {.fd = fd, .events = POLLIN}; poll(&pfd, 1, 5);
    }
    if (control_result()) { config = *control_result(); ok = true; }
    else {
        const char *auth_file = getenv("PLAYDATE_CONTROL_AUTH_URL_FILE");
        if (auth_file && control_auth_url()[0]) {
            int out = open(auth_file, O_WRONLY | O_CREAT | O_TRUNC, 0600);
            if (out >= 0) { size_t length = strlen(control_auth_url()); if (write(out, control_auth_url(), length) != (ssize_t)length) fputs("Auth URL file failed\n", stderr); close(out); }
        }
        fprintf(stderr, "Control failure state=%d: %s\n", control_state(), control_error());
    }
done:
    SSL_free(ssl); close(fd); control_stop(); return ok;
}
static bool load_configuration(const char *path, const char *stage_path) {
    FILE *file = fopen(path, "rb"); if (!file) return false;
    size_t length = fread(configuration, 1, sizeof(configuration), file); fclose(file);
    if (length == sizeof(configuration) || !control_configure(&config, configuration, length)) { fputs("Invalid native control configuration\n", stderr); return false; }
    memset(configuration, 0, sizeof(configuration));
    if (stage_path && (file = fopen(stage_path, "rb"))) {
        static ControlConfig staged;
        bool ok = fread(&staged, 1, sizeof(staged), file) == sizeof(staged) && fgetc(file) == EOF;
        fclose(file);
        if (!ok || memcmp(staged.machine_private, config.machine_private, 32) || strcmp(staged.node_id, config.node_id)) { fputs("Staged identity does not match configuration\n", stderr); return false; }
        config = staged; memset(&staged, 0, sizeof(staged));
    }
    return true;
}
int main(int argc, char **argv) {
    bool enrolling = argc == 5 && !strcmp(argv[1], "--enroll");
    if (!enrolling && (argc < 2 || argc > 4)) { fputs("Usage: control-host CONFIG.json [RESULT.binary] [STAGED.binary]\n       control-host --enroll PEER PORT STATE.binary\nOnly run while all other owners of this isolated node are stopped.\n", stderr); return 2; }
    const char *saved_path = enrolling ? argv[4] : argc == 4 ? argv[3] : NULL;
    FILE *file;
    if (enrolling) {
        char *end; unsigned long port = strtoul(argv[3], &end, 10);
        if (*end || !port || port > 65535) return 2;
        file = fopen(saved_path, "rb");
        if (file) {
            bool ok = fread(&config, 1, sizeof(config), file) == sizeof(config) && fgetc(file) == EOF;
            fclose(file); if (!ok) { fputs("Invalid saved enrollment\n", stderr); return 1; }
        } else {
            if (errno != ENOENT) return 1;
            control_create_identity(&config, (uint16_t)port);
            snprintf(config.hostname, sizeof(config.hostname), "playdate-pairing-check-%02x%02x%02x", config.tailnet.node_public[0], config.tailnet.node_public[1], config.tailnet.node_public[2]);
            if (!save(&config, saved_path)) return 1;
        }
        control_set_peer_target(argv[2]);
        const char *auth_path = getenv("PLAYDATE_CONTROL_AUTH_URL_FILE");
        if (auth_path && (file = fopen(auth_path, "rb"))) {
            char url[1024]; size_t n = fread(url, 1, sizeof(url) - 1, file);
            bool ok = fgetc(file) == EOF; fclose(file); if (!ok) return 1;
            url[n] = 0; control_set_auth_url(url);
        }
    } else if (!load_configuration(argv[1], saved_path)) return 1;
    if (getenv("PLAYDATE_CONTROL_FORCE_RENEW")) config.node_expiry_ms = tn_unix_ms() + 60000;
    SSL_CTX *ctx = SSL_CTX_new(TLS_client_method()); if (!ctx) return 1;
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL); SSL_CTX_set_default_verify_paths(ctx);
    bool ok = exchange(ctx, true, NULL);
    if (ok && enrolling) ok = save(&config, saved_path);
    if (ok) ok = exchange(ctx, false, saved_path);
    SSL_CTX_free(ctx);
    if (ok && (enrolling || argc >= 3)) ok = save(&config, enrolling ? saved_path : argv[2]);
    const char *json_out = getenv("PLAYDATE_CONTROL_RESULT_JSON");
    if (ok && json_out) ok = save_json(&config, json_out);
    if (ok) printf("PASS authenticated register/map; node %u.%u.%u.%u peer %u.%u.%u.%u DERP %s:%u region %u; node expiry %llu peer expiry %llu\n", config.tailnet.node_ip[0], config.tailnet.node_ip[1], config.tailnet.node_ip[2], config.tailnet.node_ip[3], config.tailnet.peer_ip[0], config.tailnet.peer_ip[1], config.tailnet.peer_ip[2], config.tailnet.peer_ip[3], config.tailnet.derp_host, config.tailnet.derp_port, config.derp_region, (unsigned long long)config.node_expiry_ms, (unsigned long long)config.peer_expiry_ms);
    memset(&config, 0, sizeof(config)); return ok ? 0 : 1;
}

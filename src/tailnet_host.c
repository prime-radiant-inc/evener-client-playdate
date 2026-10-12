/* Opt-in native protocol qualification. Never part of the handheld build. */
#include "tailnet.h"
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
#include <strings.h>

static TailnetConfig config;
static char configuration[16384], response[131072], baseline[65536];
static uint8_t input[4096], write_copy[1024];
static size_t response_length, baseline_length, verified_bytes;
static unsigned received_responses;
void tn_log(const char *message) { puts(message); fflush(stdout); }
uint32_t tn_now_ms(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)((uint64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000);
}
uint64_t tn_unix_ms(void) {
    struct timeval t; gettimeofday(&t, NULL);
    return (uint64_t)t.tv_sec * 1000 + t.tv_usec / 1000;
}
void tn_random(uint8_t *bytes, size_t length) {
    if (length > 2147483647 || RAND_bytes(bytes, (int)length) != 1) abort();
}
static bool check_responses(void) {
    response[response_length] = 0;
    for (;;) {
        char *end = strstr(response, "\r\n\r\n");
        if (!end) return true;
        size_t header = (size_t)(end + 4 - response);
        if (strncmp(response, "HTTP/1.1 200", 12)) { fputs("Hub health status was not 200\n", stderr); return false; }
        size_t body = 0;
        bool content_length = false;
        for (char *p = response; p < end;) {
            char *line = strstr(p, "\r\n");
            if (!line || line > end) break;
            if (!strncasecmp(p, "Content-Length:", 15)) { body = strtoul(p + 15, NULL, 10); content_length = true; }
            p = line + 2;
        }
        if (!content_length || body > sizeof(baseline)) { fputs("Unsupported health response framing\n", stderr); return false; }
        if (response_length < header + body) return true;
        if (!received_responses) { memcpy(baseline, response + header, body); baseline_length = body; }
        else if (body != baseline_length || memcmp(baseline, response + header, body)) {
            fputs("Health response byte integrity changed\n", stderr); return false;
        }
        verified_bytes += body;
        received_responses++;
        memmove(response, response + header + body, response_length - header - body);
        response_length -= header + body;
        response[response_length] = 0;
    }
}
int main(int argc, char **argv) {
    if (argc < 2) { fputs("Usage: tailnet-host ENROLLMENT.json [seconds=140] [path=/api/health] [receive-stall-ms=0]\n", stderr); return 2; }
    unsigned receive_stall = argc > 4 ? (unsigned)strtoul(argv[4], NULL, 10) : 0;
    unsigned seconds = argc > 2 ? (unsigned)strtoul(argv[2], NULL, 10) : 140;
    if (!seconds || seconds > 3600) return 2;
    FILE *file = fopen(argv[1], "rb");
    if (!file) { perror("Enrollment configuration"); return 1; }
    size_t size = fread(configuration, 1, sizeof(configuration), file); fclose(file);
    if (size == sizeof(configuration) || !tn_configure(&config, configuration, size)) {
        fputs("Invalid enrollment configuration\n", stderr); return 1;
    }
    memset(configuration, 0, sizeof(configuration));
    struct addrinfo hints = {.ai_socktype = SOCK_STREAM, .ai_family = AF_INET}, *addresses = NULL;
    char port[6]; snprintf(port, sizeof(port), "%u", config.derp_port);
    if (getaddrinfo(config.derp_host, port, &hints, &addresses)) { fputs("DERP DNS failed\n", stderr); return 1; }
    int fd = -1;
    for (struct addrinfo *a = addresses; a; a = a->ai_next) {
        fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd < 0) continue;
        struct timeval timeout = {.tv_sec = 15};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        if (!connect(fd, a->ai_addr, a->ai_addrlen)) break;
        close(fd); fd = -1;
    }
    freeaddrinfo(addresses);
    if (fd < 0) { perror("DERP connect"); return 1; }
    SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) { close(fd); return 1; }
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL); SSL_CTX_set_default_verify_paths(ctx);
    SSL *ssl = SSL_new(ctx);
    if (!ssl) { SSL_CTX_free(ctx); close(fd); return 1; }
    SSL_set_tlsext_host_name(ssl, config.derp_host); SSL_set1_host(ssl, config.derp_host); SSL_set_fd(ssl, fd);
    if (SSL_connect(ssl) != 1) { fputs("DERP verified TLS failed\n", stderr); SSL_free(ssl); SSL_CTX_free(ctx); close(fd); return 1; }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    SSL_set_mode(ssl, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
    if (!tn_start(&config)) { SSL_free(ssl); SSL_CTX_free(ctx); close(fd); return 1; }
    const char *path = argc > 3 ? argv[3] : "/api/health";
    if (path[0] != '/' || strchr(path, '\r') || strchr(path, '\n') || strlen(path) > 200) return 2;
    char request[512];
    int request_length = snprintf(request, sizeof(request),
        "GET %s HTTP/1.1\r\nHost: %u.%u.%u.%u:%u\r\nConnection: keep-alive\r\n\r\n",
        path, config.peer_ip[0], config.peer_ip[1], config.peer_ip[2], config.peer_ip[3], config.peer_port);
    uint32_t start = tn_now_ms(), opened_at = 0, last_request = 0;
    unsigned sent_requests = 0;
    size_t request_offset = 0, pending_write = 0;
    bool opened = false, ok = false;
    for (;;) {
        size_t available; const uint8_t *out = tn_output(&available);
        if (!pending_write && available) {
            pending_write = available < sizeof(write_copy) ? available : sizeof(write_copy);
            memcpy(write_copy, out, pending_write);
        }
        if (pending_write) {
            int n = SSL_write(ssl, write_copy, (int)pending_write);
            if (n > 0) { tn_consume((size_t)n); pending_write = 0; }
            else {
                int error = SSL_get_error(ssl, n);
                if (error != SSL_ERROR_WANT_READ && error != SSL_ERROR_WANT_WRITE) { tn_fail("DERP TLS write failed"); break; }
            }
        }
        int n = SSL_read(ssl, input, sizeof(input));
        if (n > 0) {
            size_t consumed = tn_input(input, (size_t)n);
            if (consumed != (size_t)n) { tn_fail("Host input backpressure requires retained bytes"); break; }
        } else {
            int error = SSL_get_error(ssl, n);
            if (error != SSL_ERROR_WANT_READ && error != SSL_ERROR_WANT_WRITE) { tn_fail("DERP TLS closed"); break; }
        }
        tn_tick();
        uint32_t now = tn_now_ms();
        if (!opened && tn_state() == TN_READY) { opened = tn_open(); if (!opened) break; }
        if (tn_tcp_state() == TN_TCP_OPEN) {
            if (!opened_at) opened_at = now;
            if (request_offset || (now - last_request >= 100 && sent_requests - received_responses < 16 && now - opened_at < seconds * 1000)) {
                int count = tn_write(request + request_offset, (size_t)request_length - request_offset);
                if (count < 0) break;
                request_offset += (size_t)count;
                if (request_offset == (size_t)request_length) { request_offset = 0; sent_requests++; last_request = now; }
            }
        }
        while (tn_available() && (!opened_at || now - opened_at >= receive_stall)) {
            size_t count = tn_read(response + response_length, sizeof(response) - response_length - 1);
            if (!count) { tn_fail("Host response buffer full"); break; }
            response_length += count;
            if (!check_responses()) { tn_fail("TCP integrity verification failed"); break; }
        }
        if (tn_state() == TN_FAILED || tn_tcp_state() == TN_TCP_FAILED || tn_tcp_state() == TN_TCP_EOF) break;
        if (opened_at && now - opened_at >= seconds * 1000 && received_responses == sent_requests && !request_offset) {
            ok = received_responses > 0 && response_length == 0; break;
        }
        if (now - start > (seconds + 70) * 1000) { tn_fail("Host verification timed out"); break; }
        struct pollfd pfd = {.fd = fd, .events = POLLIN}; poll(&pfd, 1, 5);
    }
    uint8_t digest[32]; unsigned digest_length = 0;
    EVP_Digest(baseline, baseline_length, digest, &digest_length, EVP_sha256(), NULL);
    printf("Body SHA256: ");
    for (unsigned i = 0; i < digest_length; i++) printf("%02x", digest[i]);
    putchar('\n');
    printf("%s: %u/%u responses, %zu verified body bytes, TCP pending=%zu, duration=%u ms\n",
        ok ? "PASS" : "FAIL", received_responses, sent_requests, verified_bytes, tn_pending(), tn_now_ms() - start);
    tn_stop(); SSL_free(ssl); SSL_CTX_free(ctx); close(fd);
    return ok ? 0 : 1;
}

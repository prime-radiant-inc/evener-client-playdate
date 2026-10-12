#include "tailnet.h"
#include "tailnet_wg.h"
#include "json.h"
#include "tweetnacl.h"
#include "lwip/init.h"
#include "lwip/ip.h"
#include "lwip/netif.h"
#include "lwip/tcp.h"
#include "lwip/timeouts.h"
#include <stdio.h>
#include <string.h>

/* Bounded queues live off the firmware's 10 KB game-task stack. */
#define DERP_RX_CAPACITY 65580
#define DERP_TX_CAPACITY 32768
#define TCP_RX_CAPACITY 32768
#define PACKET_CAPACITY 1536
static TailnetConfig config;
static TailnetState state;
static TailnetTCPState tcp_state;
static enum { HTTP_UPGRADE, SERVER_KEY, SERVER_INFO, PEER } phase;
static uint8_t derp_rx[DERP_RX_CAPACITY], derp_tx[DERP_TX_CAPACITY];
static size_t derp_rx_length, derp_tx_length, derp_tx_offset;
static uint8_t tcp_rx[TCP_RX_CAPACITY];
static size_t tcp_rx_head, tcp_rx_length, tcp_pending;
static uint8_t box_plain[2048], box_cipher[2048], frame_buffer[2112];
static uint8_t ip_packet[PACKET_CAPACITY], encrypted_packet[PACKET_CAPACITY + 64];
static uint8_t decrypted_packet[PACKET_CAPACITY];
static struct netif tunnel;
static struct tcp_pcb *connection;
static bool initialized, tunnel_added, handshake_pending, confirmation_pending;
static uint32_t started_at, last_handshake, last_tx, last_rx, last_ping;
static char error_text[160];

static uint32_t read32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static void write32(uint8_t *p, uint32_t n) {
    p[0] = n >> 24; p[1] = n >> 16; p[2] = n >> 8; p[3] = n;
}
uint32_t sys_now(void) { return tn_now_ms(); }
uint32_t tn_wg_now_ms(void) { return tn_now_ms(); }
uint64_t tn_wg_unix_ms(void) { return tn_unix_ms(); }
void tn_wg_random(uint8_t *bytes, size_t length) { tn_random(bytes, length); }
void randombytes(unsigned char *bytes, unsigned long long length) { tn_random(bytes, (size_t)length); }

static bool hex(const Json *j, const char *name, uint8_t out[32]) {
    char text[65];
    if (!json_field(j, 0, name, text, sizeof(text)) || strlen(text) != 64) return false;
    for (unsigned i = 0; i < 32; i++) {
        unsigned v = 0;
        for (unsigned k = 0; k < 2; k++) {
            char c = text[i * 2 + k];
            if (c >= '0' && c <= '9') v = v * 16 + c - '0';
            else if (c >= 'a' && c <= 'f') v = v * 16 + c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') v = v * 16 + c - 'A' + 10;
            else return false;
        }
        out[i] = v;
    }
    return true;
}
static bool address(const Json *j, const char *name, uint8_t out[4]) {
    char text[32], tail;
    unsigned a, b, c, d;
    if (!json_field(j, 0, name, text, sizeof(text)) ||
        sscanf(text, "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) != 4 || a > 255 || b > 255 || c > 255 || d > 255) return false;
    out[0] = a; out[1] = b; out[2] = c; out[3] = d;
    return true;
}
bool tn_configure(TailnetConfig *out, const char *text, size_t length) {
    Json j;
    if (!out || !text) return false;
    memset(out, 0, sizeof(*out));
    if (!json_parse(&j, text, length)) return false;
    bool ok = json_field(&j, 0, "derp_host", out->derp_host, sizeof(out->derp_host)) &&
        hex(&j, "node_private_key", out->node_private) && hex(&j, "node_public_key", out->node_public) &&
        hex(&j, "peer_public_key", out->peer_public) && hex(&j, "derp_public_key", out->derp_public) &&
        hex(&j, "disco_private_key", out->disco_private) && hex(&j, "disco_public_key", out->disco_public) &&
        hex(&j, "peer_disco_key", out->peer_disco) && hex(&j, "seed", out->seed) && address(&j, "node_ip", out->node_ip) && address(&j, "peer_ip", out->peer_ip);
    int port = json_int(&j, json_get(&j, 0, "peer_port"));
    int derp_port = json_int(&j, json_get(&j, 0, "derp_port"));
    int derp_region = json_int(&j, json_get(&j, 0, "derp_region_id"));
    ok = ok && derp_region > 0 && derp_region <= 65535 && port > 0 && port <= 65535 && derp_port > 0 && derp_port <= 65535 && out->derp_host[0];
    for (const char *p = out->derp_host; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '-' || *p == '.')) ok = false;
    out->peer_port = (uint16_t)port;
    out->derp_port = (uint16_t)derp_port;
    out->derp_region = (uint16_t)derp_region;
    json_free(&j);
    return ok;
}


TailnetState tn_state(void) { return state; }
TailnetTCPState tn_tcp_state(void) { return tcp_state; }
const char *tn_error(void) { return error_text; }
size_t tn_available(void) { return tcp_rx_length; }
size_t tn_pending(void) { return tcp_pending; }

void tn_close(void) {
    if (connection) {
        struct tcp_pcb *old = connection;
        connection = NULL;
        tcp_arg(old, NULL);
        tcp_err(old, NULL);
        tcp_recv(old, NULL);
        tcp_sent(old, NULL);
        tcp_abort(old);
    }
    tcp_state = TN_TCP_CLOSED;
    tcp_pending = 0;
}
void tn_fail(const char *reason) {
    if (state == TN_FAILED) return;
    snprintf(error_text, sizeof(error_text), "%s", reason ? reason : "Tailnet transport failed");
    tn_log(error_text);
    state = TN_FAILED;
    tn_close();
    tcp_state = TN_TCP_FAILED;
}
void tn_stop(void) {
    tn_close();
    state = TN_STOPPED;
    if (tunnel_added) { netif_remove(&tunnel); tunnel_added = false; }
    derp_rx_length = derp_tx_length = derp_tx_offset = 0;
    memset(&config, 0, sizeof(config));
}
static size_t output_room(void) { return sizeof(derp_tx) - (derp_tx_length - derp_tx_offset); }
static bool queue(const void *bytes, size_t length) {
    if (length > output_room()) return false;
    if (derp_tx_offset) {
        memmove(derp_tx, derp_tx + derp_tx_offset, derp_tx_length - derp_tx_offset);
        derp_tx_length -= derp_tx_offset;
        derp_tx_offset = 0;
    }
    memcpy(derp_tx + derp_tx_length, bytes, length);
    derp_tx_length += length;
    return true;
}
static bool frame(uint8_t type, const void *bytes, size_t length) {
    if (length + 5 > output_room()) return false;
    uint8_t header[5]; header[0] = type; write32(header + 1, (uint32_t)length);
    queue(header, sizeof(header));
    if (length) queue(bytes, length);
    return true;
}
const uint8_t *tn_output(size_t *length) {
    *length = derp_tx_length - derp_tx_offset;
    return derp_tx + derp_tx_offset;
}
void tn_consume(size_t length) {
    if (length > derp_tx_length - derp_tx_offset) { tn_fail("Invalid DERP socket write result"); return; }
    derp_tx_offset += length;
}
static bool send_encrypted(size_t length) {
    if (length + 32 > sizeof(frame_buffer) || length + 37 > output_room()) return false;
    memcpy(frame_buffer, config.peer_public, 32);
    memcpy(frame_buffer + 32, encrypted_packet, length);
    if (!frame(4, frame_buffer, length + 32)) return false;
    last_tx = tn_now_ms();
    return true;
}
static bool handshake(void) {
    if (output_room() < 256) return false;
    int n = tn_wg_handshake(encrypted_packet, sizeof(encrypted_packet));
    if (n <= 0) { tn_fail("Cannot create WireGuard handshake"); return false; }
    if (!send_encrypted((size_t)n)) return false;
    last_handshake = tn_now_ms();
    handshake_pending = true;
    return true;
}
static bool keepalive(void) {
    if (output_room() < 69 || !tn_wg_ready()) return false;
    int n = tn_wg_encrypt(NULL, 0, encrypted_packet, sizeof(encrypted_packet));
    return n > 0 && send_encrypted((size_t)n);
}
static err_t tunnel_output(struct netif *netif, struct pbuf *packet, const ip4_addr_t *destination) {
    (void)netif;
    if (state != TN_READY || !tn_wg_ready()) return ERR_RTE;
    if (memcmp(&destination->addr, config.peer_ip, 4) || packet->tot_len > PACKET_CAPACITY) return ERR_RTE;
    size_t padded = (packet->tot_len + 15u) & ~(size_t)15;
    if (output_room() < padded + 69) return ERR_MEM;
    if (pbuf_copy_partial(packet, ip_packet, packet->tot_len, 0) != packet->tot_len) return ERR_BUF;
    int n = tn_wg_encrypt(ip_packet, packet->tot_len, encrypted_packet, sizeof(encrypted_packet));
    if (n <= 0) return ERR_RTE;
    return send_encrypted((size_t)n) ? ERR_OK : ERR_MEM;
}
static err_t tunnel_init(struct netif *netif) {
    netif->name[0] = 't'; netif->name[1] = 'n';
    netif->output = tunnel_output;
    netif->mtu = 1280;
    netif->flags = NETIF_FLAG_UP | NETIF_FLAG_LINK_UP;
    return ERR_OK;
}
static void tcp_error(void *arg, err_t error) {
    (void)arg; (void)error;
    connection = NULL;
    tcp_state = TN_TCP_FAILED;
    tcp_pending = 0;
    tn_log("Hub TCP stream failed");
}
static err_t tcp_receive(void *arg, struct tcp_pcb *pcb, struct pbuf *packet, err_t error) {
    (void)arg; (void)pcb;
    if (!packet) { tcp_state = TN_TCP_EOF; return ERR_OK; }
    if (error != ERR_OK) return error;
    if (packet->tot_len > sizeof(tcp_rx) - tcp_rx_length) return ERR_MEM;
    size_t tail = (tcp_rx_head + tcp_rx_length) % sizeof(tcp_rx);
    size_t first = packet->tot_len;
    if (first > sizeof(tcp_rx) - tail) first = sizeof(tcp_rx) - tail;
    pbuf_copy_partial(packet, tcp_rx + tail, (u16_t)first, 0);
    if (first < packet->tot_len) pbuf_copy_partial(packet, tcp_rx, packet->tot_len - (u16_t)first, (u16_t)first);
    tcp_rx_length += packet->tot_len;
    pbuf_free(packet);
    /* Window credit is returned only when the application actually reads. */
    return ERR_OK;
}
static err_t tcp_acknowledged(void *arg, struct tcp_pcb *pcb, u16_t length) {
    (void)arg; (void)pcb;
    tcp_pending = length <= tcp_pending ? tcp_pending - length : 0;
    return ERR_OK;
}
static err_t tcp_connected(void *arg, struct tcp_pcb *pcb, err_t error) {
    (void)arg; (void)pcb;
    tcp_state = error == ERR_OK ? TN_TCP_OPEN : TN_TCP_FAILED;
    if (error == ERR_OK) tn_log("Native Hub TCP connected");
    return error;
}
bool tn_open(void) {
    if (state != TN_READY || !tn_wg_ready() || connection) return false;
    tcp_rx_head = tcp_rx_length = tcp_pending = 0;
    connection = tcp_new_ip_type(IPADDR_TYPE_V4);
    if (!connection) { tcp_state = TN_TCP_FAILED; return false; }
    ip_addr_t local, peer;
    memcpy(&local.addr, config.node_ip, 4);
    memcpy(&peer.addr, config.peer_ip, 4);
    if (tcp_bind(connection, &local, 0) != ERR_OK) { tn_close(); tcp_state = TN_TCP_FAILED; return false; }
    tcp_recv(connection, tcp_receive);
    tcp_err(connection, tcp_error);
    tcp_sent(connection, tcp_acknowledged);
    tcp_nagle_disable(connection);
    connection->so_options |= SOF_KEEPALIVE;
    connection->keep_idle = 30000;
    connection->keep_intvl = 10000;
    connection->keep_cnt = 3;
    tcp_state = TN_TCP_CONNECTING;
    if (tcp_connect(connection, &peer, config.peer_port, tcp_connected) != ERR_OK) {
        tn_close(); tcp_state = TN_TCP_FAILED; return false;
    }
    return true;
}
int tn_write(const void *bytes, size_t length) {
    if (!connection || tcp_state != TN_TCP_OPEN || state != TN_READY) return -1;
    if (!bytes && length) return -1;
    if (length > tcp_sndbuf(connection)) length = tcp_sndbuf(connection);
    if (length > 65535) length = 65535;
    if (!length) return 0;
    err_t error = tcp_write(connection, bytes, (u16_t)length, TCP_WRITE_FLAG_COPY);
    if (error == ERR_MEM) return 0;
    if (error != ERR_OK) return -1;
    tcp_pending += length;
    tcp_output(connection);
    return (int)length;
}
size_t tn_read(void *bytes, size_t capacity) {
    if (!bytes && capacity) return 0;
    size_t count = capacity < tcp_rx_length ? capacity : tcp_rx_length;
    size_t first = count;
    if (first > sizeof(tcp_rx) - tcp_rx_head) first = sizeof(tcp_rx) - tcp_rx_head;
    if (first) memcpy(bytes, tcp_rx + tcp_rx_head, first);
    if (first < count) memcpy((uint8_t *)bytes + first, tcp_rx, count - first);
    tcp_rx_head = (tcp_rx_head + count) % sizeof(tcp_rx);
    tcp_rx_length -= count;
    if (count && connection) tcp_recved(connection, (u16_t)count);
    return count;
}
/* Tailscale disco/disco.go v1.104.1: NaCl box wrapper and v0 Ping/Pong.
 * A DERP source is the relay address 127.3.3.40:region, never a UDP endpoint.
 * Only the selected peer's authenticated discovery identity can elicit a reply.
 */
static const uint8_t disco_magic[6] = {0x54,0x53,0xf0,0x9f,0x92,0xac};
static bool discovery_input(const uint8_t *packet, size_t length) {
    const size_t wrapper = 6 + 32 + 24;
    if (length < wrapper + 16 + 14 || memcmp(packet + 6, config.peer_disco, 32) ||
        length - wrapper + 16 > sizeof(box_cipher)) return true;
    /* Reserve a complete reply before consuming the ping or advancing the RNG. */
    if (output_room() < 147) return false;
    memset(box_cipher, 0, 16);
    memcpy(box_cipher + 16, packet + wrapper, length - wrapper);
    if (crypto_box_open(box_plain, box_cipher, length - wrapper + 16,
        packet + 38, config.peer_disco, config.disco_private)) return true;
    size_t payload = length - wrapper - 16;
    if (box_plain[32] != 1) return true;
    if (payload >= 46) {
        uint8_t any = 0;
        for (unsigned i = 0; i < 32; i++) any |= box_plain[46 + i];
        if (any && memcmp(box_plain + 46, config.peer_public, 32)) return true;
    }
    uint8_t transaction[12], nonce[24];
    memcpy(transaction, box_plain + 34, sizeof(transaction));
    memset(box_plain, 0, 64);
    box_plain[32] = 2;
    memcpy(box_plain + 34, transaction, sizeof(transaction));
    box_plain[56] = box_plain[57] = 0xff;
    box_plain[58] = 127; box_plain[59] = 3; box_plain[60] = 3; box_plain[61] = 40;
    box_plain[62] = config.derp_region >> 8; box_plain[63] = config.derp_region;
    tn_random(nonce, sizeof(nonce));
    if (crypto_box(box_cipher, box_plain, 64, nonce, config.peer_disco, config.disco_private)) {
        tn_fail("Cannot encrypt native discovery response"); return true;
    }
    memcpy(frame_buffer, config.peer_public, 32);
    memcpy(frame_buffer + 32, disco_magic, sizeof(disco_magic));
    memcpy(frame_buffer + 38, config.disco_public, 32);
    memcpy(frame_buffer + 70, nonce, sizeof(nonce));
    memcpy(frame_buffer + 94, box_cipher + 16, 48);
    frame(4, frame_buffer, 142);
    tn_log("Native discovery ping answered");
    return true;
}

static bool handle_frame(uint8_t type, const uint8_t *bytes, size_t length) {
    if (type == 0x12 && length == 8) return frame(0x13, bytes, length);
    if (type == 0x14 && length) { tn_fail("DERP reports unhealthy connection"); return true; }
    if (type == 0x15) { tn_fail("DERP server restarting"); return true; }
    if (phase == SERVER_KEY) {
        static const uint8_t magic[8] = {0x44,0x45,0x52,0x50,0xf0,0x9f,0x94,0x91};
        if (type != 1 || length < 40 || memcmp(bytes, magic, 8) || memcmp(bytes + 8, config.derp_public, 32)) {
            tn_fail("DERP server key does not match enrollment"); return true;
        }
        static const char info[] = "{\"version\":2,\"CanAckPings\":true}";
        if (output_room() < 77 + sizeof(info) - 1) return false;
        uint8_t nonce[24]; tn_random(nonce, sizeof(nonce));
        memset(box_plain, 0, 32); memcpy(box_plain + 32, info, sizeof(info) - 1);
        if (crypto_box(box_cipher, box_plain, 32 + sizeof(info) - 1, nonce, config.derp_public, config.node_private)) {
            tn_fail("DERP client authentication failed"); return true;
        }
        memcpy(frame_buffer, config.node_public, 32); memcpy(frame_buffer + 32, nonce, 24);
        memcpy(frame_buffer + 56, box_cipher + 16, 16 + sizeof(info) - 1);
        frame(2, frame_buffer, 72 + sizeof(info) - 1);
        phase = SERVER_INFO;
    } else if (phase == SERVER_INFO) {
        if (type != 3 || length < 40 || length - 8 > sizeof(box_cipher)) {
            tn_fail("Invalid DERP authentication response"); return true;
        }
        if (output_room() < 6) return false;
        memset(box_cipher, 0, 16); memcpy(box_cipher + 16, bytes + 24, length - 24);
        if (crypto_box_open(box_plain, box_cipher, length - 8, bytes, config.derp_public, config.node_private)) {
            tn_fail("DERP response authentication failed"); return true;
        }
        uint8_t preferred = 1; frame(7, &preferred, 1);
        phase = PEER;
        tn_log("Native DERP authenticated");
        handshake_pending = false;
        handshake();
    } else if (type == 5 && length >= 36 && !memcmp(bytes, config.peer_public, 32)) {
        if (length >= 38 && !memcmp(bytes + 32, disco_magic, sizeof(disco_magic)))
            return discovery_input(bytes + 32, length - 32);
        if (bytes[32] < 1 || bytes[32] > 4) return true;
        int n = tn_wg_receive(bytes + 32, length - 32, decrypted_packet, sizeof(decrypted_packet));
        if (n < 0) return true;
        if (bytes[32] == 2 && tn_wg_ready()) {
            handshake_pending = false;
            confirmation_pending = true;
            state = TN_READY;
            tn_log("Native WireGuard session authenticated");
        }
        if (n > 0) {
            if (n < 20 || (decrypted_packet[0] >> 4) != 4 ||
                memcmp(decrypted_packet + 12, config.peer_ip, 4) ||
                memcmp(decrypted_packet + 16, config.node_ip, 4)) return true;
            size_t total = (size_t)decrypted_packet[2] << 8 | decrypted_packet[3];
            if (total < 20 || total > (size_t)n) return true;
            struct pbuf *packet = pbuf_alloc(PBUF_RAW, (u16_t)total, PBUF_POOL);
            /* Resource exhaustion drops an IP packet, never accepted TCP bytes;
             * TCP retransmission and receive-window ownership recover it. */
            if (!packet) return true;
            if (pbuf_take(packet, decrypted_packet, (u16_t)total) != ERR_OK || ip_input(packet, &tunnel) != ERR_OK)
                pbuf_free(packet);
        }
    }
    return true;
}
static void process_input(void) {
    if (state == TN_STOPPED || state == TN_FAILED) return;
    if (phase == HTTP_UPGRADE) {
        size_t end = 0;
        for (size_t i = 3; i < derp_rx_length; i++)
            if (!memcmp(derp_rx + i - 3, "\r\n\r\n", 4)) { end = i + 1; break; }
        if (!end) { if (derp_rx_length > 8192) tn_fail("DERP upgrade headers too long"); return; }
        if (end < 12 || memcmp(derp_rx, "HTTP/1.", 7) || memcmp(derp_rx + 9, "101", 3)) {
            tn_fail("DERP HTTP upgrade rejected"); return;
        }
        memmove(derp_rx, derp_rx + end, derp_rx_length - end); derp_rx_length -= end;
        phase = SERVER_KEY;
    }
    while (derp_rx_length >= 5 && state != TN_FAILED) {
        size_t body = read32(derp_rx + 1);
        if (body > sizeof(derp_rx) - 5) {
            char detail[96];
            snprintf(detail, sizeof(detail), "Invalid DERP header: type %u, body %lu, buffered %lu",
                derp_rx[0], (unsigned long)body, (unsigned long)derp_rx_length);
            tn_log(detail); tn_fail("DERP frame exceeds transport limit"); return;
        }
        if (derp_rx_length < body + 5 || !handle_frame(derp_rx[0], derp_rx + 5, body)) return;
        memmove(derp_rx, derp_rx + body + 5, derp_rx_length - body - 5);
        derp_rx_length -= body + 5;
    }
}
size_t tn_input(const uint8_t *bytes, size_t length) {
    if (state == TN_STOPPED || state == TN_FAILED || (!bytes && length)) return 0;
    process_input();
    if (state == TN_FAILED) return 0;
    size_t count = length < sizeof(derp_rx) - derp_rx_length ? length : sizeof(derp_rx) - derp_rx_length;
    if (count) memcpy(derp_rx + derp_rx_length, bytes, count);
    derp_rx_length += count;
    if (count) last_rx = tn_now_ms();
    process_input();
    return count;
}
bool tn_start(const TailnetConfig *settings) {
    if (!settings) return false;
    /* settings must be external to the transport; copy before stop clears it. */
    config = *settings;
    tn_close();
    if (tunnel_added) { netif_remove(&tunnel); tunnel_added = false; }
    derp_rx_length = derp_tx_length = derp_tx_offset = 0;
    state = TN_CONNECTING; phase = HTTP_UPGRADE;
    tcp_rx_head = tcp_rx_length = tcp_pending = 0;
    error_text[0] = 0;
    handshake_pending = confirmation_pending = false;
    started_at = last_tx = last_rx = last_ping = tn_now_ms();
    last_handshake = started_at - 5000;
    uint8_t public_key[32];
    if (crypto_scalarmult_base(public_key, config.disco_private) || memcmp(public_key, config.disco_public, 32)) {
        tn_fail("Invalid native discovery keys"); return false;
    }
    if (crypto_scalarmult_base(public_key, config.node_private) || memcmp(public_key, config.node_public, 32) ||
        !tn_wg_initialize(config.node_private, config.peer_public)) {
        tn_fail("Invalid enrolled node keys"); return false;
    }
    if (!initialized) { lwip_init(); initialized = true; }
    ip4_addr_t local, mask, gateway;
    memcpy(&local.addr, config.node_ip, 4); mask.addr = 0; gateway.addr = 0;
    if (!netif_add(&tunnel, &local, &mask, &gateway, NULL, tunnel_init, ip_input)) {
        tn_fail("Cannot create tailnet IP interface"); return false;
    }
    tunnel_added = true;
    netif_set_default(&tunnel); netif_set_up(&tunnel); netif_set_link_up(&tunnel);
    char upgrade[512];
    int n = snprintf(upgrade, sizeof(upgrade), "GET /derp HTTP/1.1\r\nHost: %s\r\nUpgrade: DERP\r\nConnection: Upgrade\r\n\r\n", config.derp_host);
    if (n < 0 || (size_t)n >= sizeof(upgrade) || !queue(upgrade, (size_t)n)) {
        tn_fail("Invalid DERP upgrade request"); return false;
    }
    return true;
}
void tn_tick(void) {
    if (state == TN_STOPPED || state == TN_FAILED) return;
    process_input();
    if (state == TN_FAILED) return;
    uint32_t now = tn_now_ms();
    if (state == TN_CONNECTING && now - started_at > 60000) { tn_fail("Native tailnet connection timed out"); return; }
    if (now - last_rx > 90000) { tn_fail("DERP connection became silent"); return; }
    if (phase == PEER) {
        if (tn_wg_rekey_due() && (!handshake_pending || now - last_handshake >= 5000)) handshake();
        if (confirmation_pending && keepalive()) confirmation_pending = false;
        if (tn_wg_ready() && now - last_tx >= 10000) keepalive();
        if (now - last_ping >= 30000 && output_room() >= 13) {
            uint8_t ping[8]; tn_random(ping, sizeof(ping));
            frame(0x12, ping, sizeof(ping)); last_ping = now;
        }
    }
    sys_check_timeouts();
    if (connection && state == TN_READY) tcp_output(connection);
}

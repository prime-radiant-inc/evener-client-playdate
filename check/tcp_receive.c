/* Packet-level receive-credit check; no sockets or native identity. */
#include "../src/tailnet.c"
#include "lwip/inet_chksum.h"
#include "lwip/prot/ip4.h"
#include "lwip/prot/tcp.h"
/* Transport build flags must not disable the qualification assertions. */
#undef NDEBUG
#include <assert.h>

static unsigned outgoing;
static uint32_t sent_sequence, sent_ack, peer_sequence = 1000;
static uint16_t sent_window;
static ip4_addr_t local_ip, peer_ip;

uint32_t tn_now_ms(void) { return 100; }
uint64_t tn_unix_ms(void) { return 100; }
void tn_random(uint8_t *bytes, size_t length) { memset(bytes, 1, length); }
void tn_log(const char *message) { (void)message; }

static err_t capture_packet(struct netif *netif, struct pbuf *packet, const ip4_addr_t *destination) {
    (void)netif; (void)destination;
    uint8_t header[40];
    assert(pbuf_copy_partial(packet, header, sizeof(header), 0) == sizeof(header));
    const struct tcp_hdr *tcp = (const void *)(header + 20);
    sent_sequence = lwip_ntohl(tcp->seqno);
    sent_ack = lwip_ntohl(tcp->ackno);
    sent_window = lwip_ntohs(tcp->wnd);
    outgoing++;
    return ERR_OK;
}
static err_t init_netif(struct netif *netif) {
    netif->output = capture_packet;
    netif->mtu = 1280;
    netif->flags = NETIF_FLAG_UP | NETIF_FLAG_LINK_UP;
    return ERR_OK;
}
static void peer_packet(uint8_t flags, size_t length) {
    struct pbuf *packet = pbuf_alloc(PBUF_RAW, 40 + (u16_t)length, PBUF_RAM);
    assert(packet && packet->len == 40 + length);
    memset(packet->payload, 0, packet->len);
    struct ip_hdr *ip = packet->payload;
    struct tcp_hdr *tcp = (void *)((uint8_t *)packet->payload + 20);
    tcp->src = lwip_htons(9180);
    tcp->dest = lwip_htons(connection->local_port);
    tcp->seqno = lwip_htonl(peer_sequence);
    tcp->ackno = lwip_htonl(sent_sequence + !!(flags & TCP_SYN));
    tcp->wnd = lwip_htons(16384);
    TCPH_HDRLEN_FLAGS_SET(tcp, 5, flags);
    for (size_t i = 0; i < length; i++) ((uint8_t *)packet->payload)[40 + i] = (uint8_t)(peer_sequence + i);
    assert(pbuf_remove_header(packet, 20) == 0);
    tcp->chksum = inet_chksum_pseudo(packet, IP_PROTO_TCP, packet->tot_len, &peer_ip, &local_ip);
    assert(pbuf_add_header(packet, 20) == 0);
    IPH_VHL_SET(ip, 4, 5);
    IPH_LEN_SET(ip, lwip_htons(packet->tot_len));
    IPH_TTL_SET(ip, 64);
    IPH_PROTO_SET(ip, IP_PROTO_TCP);
    ip->src.addr = peer_ip.addr;
    ip->dest.addr = local_ip.addr;
    IPH_CHKSUM_SET(ip, inet_chksum(ip, 20));
    assert(ip4_input(packet, &tunnel) == ERR_OK);
    peer_sequence += (uint32_t)length + !!(flags & TCP_SYN);
}
static void consume(size_t length, uint16_t expected_window) {
    uint8_t bytes[512];
    unsigned before = outgoing;
    size_t available = tn_available();
    assert(length <= available && length <= sizeof(bytes));
    assert(tn_read(bytes, length) == length);
    for (size_t i = 0; i < length; i++) assert(bytes[i] == (uint8_t)(peer_sequence - available + i));
    printf("read=%zu packets=%u window=%u expected=%u\n", length, outgoing - before, sent_window, expected_window);
    fflush(stdout);
    assert(outgoing > before);
    assert(sent_ack == peer_sequence);
    assert(sent_window == expected_window);
}
int main(void) {
    lwip_init();
    IP4_ADDR(&local_ip, 192, 0, 2, 1);
    IP4_ADDR(&peer_ip, 192, 0, 2, 2);
    ip4_addr_t mask, gateway;
    IP4_ADDR(&mask, 255, 255, 255, 0);
    ip4_addr_set_zero(&gateway);
    assert(netif_add(&tunnel, &local_ip, &mask, &gateway, NULL, init_netif, ip4_input));
    netif_set_default(&tunnel);
    connection = tcp_new();
    assert(connection);
    assert(tcp_bind(connection, &local_ip, 0) == ERR_OK);
    tcp_recv(connection, tcp_receive);
    tcp_err(connection, tcp_error);
    assert(tcp_connect(connection, &peer_ip, 9180, tcp_connected) == ERR_OK);
    peer_packet(TCP_SYN | TCP_ACK, 0);
    assert(tcp_state == TN_TCP_OPEN);
    const size_t sizes[] = {61, 451, 1, 2, 59, 453};
    for (size_t i = 0; i < sizeof(sizes) / sizeof(*sizes); i++) {
        peer_packet(TCP_ACK | TCP_PSH, sizes[i]);
        assert(tn_available() == sizes[i]);
        consume(sizes[i], TCP_WND);
    }
    peer_packet(TCP_ACK | TCP_PSH, 512);
    consume(32, 32);
    consume(480, TCP_WND);
    unsigned before = outgoing;
    uint8_t byte;
    assert(tn_read(&byte, 1) == 0);
    assert(outgoing == before);
    puts("PASS short segments replenish only consumed receive capacity immediately");
    return 0;
}

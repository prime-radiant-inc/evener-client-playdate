#ifndef EVENER_NET_SOCKET_H
#define EVENER_NET_SOCKET_H
#include <stdbool.h>
#include <stddef.h>

typedef struct NetSocket NetSocket;
enum NetState { NS_CLOSED, NS_CONNECTING, NS_OPEN, NS_FAILED };
NetSocket *net_open(const char *host, int port, bool tls);
void net_close(NetSocket *socket);
void net_tick(void);
/* Waiting sockets have no SDK open in flight. Idle means every owned handle
 * completed its worker commands and was released, permitting exclusive use. */
bool net_waiting(const NetSocket *socket);
bool net_idle(void);
enum NetState net_state(const NetSocket *socket);
const char *net_error(const NetSocket *socket);
size_t net_available(const NetSocket *socket);
size_t net_pending(const NetSocket *socket);
int net_read(NetSocket *socket, void *bytes, size_t capacity);
int net_write(NetSocket *socket, const void *bytes, size_t length);
#endif

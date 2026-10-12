#ifndef PLAYDATE_LWIP_CC_H
#define PLAYDATE_LWIP_CC_H
#include <stdint.h>
#include <stdlib.h>
void tn_log(const char *message);
void tn_fail(const char *message);
void tn_random(uint8_t *bytes, size_t length);
static inline uint32_t tn_lwip_random(void) {
    uint32_t value;
    tn_random((uint8_t *)&value, sizeof(value));
    return value;
}
#define LWIP_RAND() tn_lwip_random()
#define LWIP_PLATFORM_DIAG(x) do { } while (0)
#define LWIP_PLATFORM_ASSERT(message) do { tn_fail(message); abort(); } while (0)
#endif

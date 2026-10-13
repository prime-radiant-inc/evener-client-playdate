#ifndef EVENER_TAILNET_DEVICE_H
#define EVENER_TAILNET_DEVICE_H
#include <stdbool.h>
bool tailnet_device_initialize(void);
bool tailnet_device_new_identity(void);
void tailnet_device_start(void);
void tailnet_device_stop(void);
void tailnet_device_update(void);
void tailnet_device_flush(void);
void tailnet_device_set_voice_active(bool active);
const char *tailnet_device_status(void);
const char *tailnet_device_notice(void);
const char *tailnet_device_auth_url(void);
#endif

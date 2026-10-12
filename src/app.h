#ifndef EVENER_APP_H
#define EVENER_APP_H
#include "pd_api.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

extern PlaydateAPI *pd;
#if TARGET_PLAYDATE
#define app_log(format, ...) pd->system->logToConsole("Device: " format, ##__VA_ARGS__)
#else
#define app_log(format, ...) pd->system->logToConsole("Simulator: " format, ##__VA_ARGS__)
#endif
typedef struct {
    char hub_host[256], hub_path[256], hub_token[512], openai_key[512], seed[65];
    int hub_port;
    bool hub_tls, use_tailnet;
} Configuration;
extern Configuration config;
static inline float app_time(void) { return pd->system->getElapsedTime(); }
#endif

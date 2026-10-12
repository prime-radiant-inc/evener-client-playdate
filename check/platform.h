#ifndef PLAYDATE_CHECK_PLATFORM_H
#define PLAYDATE_CHECK_PLATFORM_H
#include "app.h"
#include "pairing_protocol.h"
#include <stdio.h>
#include <stdlib.h>
extern bool fail_write, fail_rename, fail_flush, fail_unlink;
extern RecordCallback *mic_callback;
extern AccessRequestCallback *mic_permission;
extern void *mic_context, *permission_context;
extern float fixture_time;
extern unsigned mic_starts;
PairingSettings destination(const char *host);
void bundle(const PairingSettings *settings);
#define CHECK(condition) do { if (!(condition)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); exit(1); } } while (0)
#endif

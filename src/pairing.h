#ifndef EVENER_PAIRING_H
#define EVENER_PAIRING_H
#include "app.h"
enum PairingResult { PAIRING_RUNNING, PAIRING_CANCELLED, PAIRING_APPLIED };
bool pairing_begin(void);
bool pairing_active(void);
bool pairing_ready_to_save(void);
void pairing_pause(void);
void pairing_resume(void);
enum PairingResult pairing_update(PDButtons pressed);
void pairing_draw(LCDFont *font);
bool pairing_draw_qr(const char *text, int x, int y, int maximum);
#endif

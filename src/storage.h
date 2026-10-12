#ifndef EVENER_STORAGE_H
#define EVENER_STORAGE_H
#include "voice.h"
#define REF_CAPACITY 512
enum DraftState { DRAFT_EMPTY, DRAFT_PARTIAL, DRAFT_READY, DRAFT_SENDING, DRAFT_UNCERTAIN };
typedef struct {
    unsigned version;
    enum DraftState state;
    char ref[REF_CAPACITY], text[DRAFT_CAPACITY];
    char mutation[65], instance[128], method[24];
} Draft;
extern Draft draft;
bool draft_load(const char *ref);
bool draft_save(void);
bool draft_set(const char *text, bool complete);
bool draft_clear(void);
bool storage_replace(const char *from, const char *to);
#endif

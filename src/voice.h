#ifndef EVENER_VOICE_H
#define EVENER_VOICE_H
#include <stdbool.h>
#define DRAFT_CAPACITY 8192
enum VoiceState { VOICE_OFF, VOICE_CONNECTING, VOICE_CONFIGURING, VOICE_READY,
    VOICE_RECORDING, VOICE_FINISHING, VOICE_REVIEW, VOICE_ERROR };
extern enum VoiceState voice_state;
extern char voice_text[DRAFT_CAPACITY], voice_status[256];
void voice_connect(void);
void voice_update(void);
bool voice_start(void);
void voice_finish(void);
void voice_close(void);
// Termination has no later game-loop frames: persist every accepted sample.
bool voice_terminate(void);
void voice_discard(void);
// Restore the selected session's audio before allowing a replacement recording.
bool voice_recover(void);
// The caller has durably persisted final text with draft_set(..., true).
void voice_saved(void);
#endif

#ifndef EVENER_HUB_H
#define EVENER_HUB_H
#include "storage.h"
#include <stddef.h>
#define NAV_PAGE_SIZE 20
#define CHAT_CAPACITY 64
enum BrowseKind { BROWSE_LIVE, BROWSE_PROJECTS, BROWSE_PROJECT };
typedef struct {
    char ref[REF_CAPACITY], title[160], state[32];
} NavigationRow;
typedef struct {
    char key[192], round[128], stream[128];
    char *text;
    int entry, item, sub, version;
    bool user, overlay, clipped;
} ChatMessage;
extern NavigationRow navigation[NAV_PAGE_SIZE];
extern int navigation_count, navigation_offset, navigation_remaining, navigation_revision;
extern enum BrowseKind browse_kind;
extern int browse_tier;
extern char browse_project[REF_CAPACITY], browse_title[160];
extern ChatMessage chat[CHAT_CAPACITY];
extern int chat_count, chat_revision;
extern char selected_ref[REF_CAPACITY], selected_title[160], session_status[32], hub_status[256];
extern bool hub_ready, hub_loading, hub_has_older, hub_hydrated;
void hub_connect(void);
void hub_update(void);
void hub_close(void);
/* Call only after closing/saving the old connection, before opening a new Hub. */
void hub_reset_profile(void);
void hub_browse(enum BrowseKind kind, const char *project, const char *title, int tier, int offset);
bool hub_select(const NavigationRow *row);
/* Keep the visible row when bounded history merges evict an edge. */
void hub_set_reading_anchor(const char *key);
/* Checkpoint the cached window before carrying its snapshot across relaunch. */
bool hub_save_view(char *snapshot, size_t capacity);
bool hub_restore_view(const NavigationRow *row, const char *snapshot);
void hub_older(void);
void hub_latest(void);
void hub_send(void);
bool hub_send_pending(void);
void hub_recheck(void);
#endif

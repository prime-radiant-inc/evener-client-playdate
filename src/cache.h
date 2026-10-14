#ifndef EVENER_CACHE_H
#define EVENER_CACHE_H
#include <stdbool.h>
#include "hub.h"
#define HISTORY_PAGE_ITEMS 10
typedef struct {
    unsigned generation;
    unsigned previous_generation, cleanup_generation;
    int oldest, newest;
    unsigned view_generation;
    int view_oldest, view_newest;
    char incarnation[128], cursor[2048];
    char first_key[192], last_key[192];
    bool complete;
} HistoryCache;
bool cache_load_navigation(void);
bool cache_save_navigation(void);
bool cache_load_chat(void);
bool cache_save_chat(void);
/* History pages own only recorded text; cached data never grants send authority. */
bool cache_load_history(HistoryCache *history);
bool cache_save_history(const HistoryCache *history);
bool cache_load_history_page(const HistoryCache *history, int page, ChatMessage *rows, int *count);
bool cache_save_history_page(const HistoryCache *history, int page, const ChatMessage *rows, int count);
/* Locate by recorded position without reading every page's text. */
int cache_history_position(const HistoryCache *history, const ChatMessage *row);
bool cache_history_edge(const HistoryCache *history, int page, bool newest, ChatMessage *row);
/* Reclaim one obsolete page/file at a time, retaining visible/saved generations. */
void cache_reclaim_history(HistoryCache *history, unsigned visible_generation);
#endif

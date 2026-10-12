#ifndef EVENER_CACHE_H
#define EVENER_CACHE_H
#include <stdbool.h>
bool cache_load_navigation(void);
bool cache_save_navigation(void);
bool cache_load_chat(void);
bool cache_save_chat(void);
#endif

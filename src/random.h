#ifndef EVENER_RANDOM_H
#define EVENER_RANDOM_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
bool random_initialize(const char *seed);
void random_fill(uint8_t *bytes, size_t length);
#endif

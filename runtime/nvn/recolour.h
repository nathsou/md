// No-op stand-in for Link's Awakening's cosmetic recolours (see texpatch.h).
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "nvn.h"

#define TEXPATCH_RECOLOUR_BASE 4096
#define RECOLOUR_TINT_FLOATS 16
static inline int recolour_identify(const NvnTexture *t, uint64_t total) { (void)t, (void)total; return -1; }
static inline const char *recolour_name(int id) { (void)id; return NULL; }
static inline int recolour_class_of(int id) { (void)id; return 0; }
static inline void recolour_add(int cls[2], int c) { (void)cls, (void)c; }
static inline void recolour_tint_draw(const int cls[2], float tint[RECOLOUR_TINT_FLOATS]) { (void)cls, (void)tint; }
static inline void recolour_pack(float tint[8], float shift, float sat, float val, float centre, float width,
                                 float min_sat, float spread) {
    (void)tint, (void)shift, (void)sat, (void)val, (void)centre, (void)width, (void)min_sat, (void)spread;
}

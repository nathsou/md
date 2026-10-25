// Link's Awakening UI texture patches, recolours and screen filters were removed; these no-op stand-ins keep the
// backends' (now dead) call sites compiling until those paths are taken out of metal.m / renderer.m / vulkan/.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "nvn.h"
#include "recolour.h"

#define TEXPATCH_UNKNOWN (-2)
static inline int texpatch_marker(const NvnTexture *t) { (void)t; return -1; }
static inline uint32_t texpatch_generation(void) { return 0; }
static inline void texpatch_apply(int id, uint32_t lv, uint8_t *b, uint32_t w, uint32_t h, uint32_t row) {
    (void)id, (void)lv, (void)b, (void)w, (void)h, (void)row;
}

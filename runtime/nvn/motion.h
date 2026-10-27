// Motion vectors (Metal): where each pixel of the game's 3D scene was in the previous frame, for MetalFX frame
// interpolation (framegen.h). The game draws none, so each program gets a second variant: its vertex shader runs the
// game's code twice - with this frame's constant buffers and with the previous frame's for the same draw (found by its
// mesh, shaders and order) - and its fragment shader writes the difference of the two positions to colour target
// MV_TARGET of the passes that draw with the scene's depth buffer. Draws matched to nothing (new this frame) get no
// motion. renderer.m does the passes and pipelines; this file the shader sources and the per-draw history.
#pragma once
#import <Metal/Metal.h>
#include <stdbool.h>
#include <stdint.h>

#define MV_TARGET 7                         // colour attachment index of the motion texture
#define MV_FORMAT MTLPixelFormatRG16Float  // uv offset to the previous frame's position (x right, y down)

bool mv_enabled(void);  // MetalFX frame generation on (framegen.h), or LARECOMP_MV=1

// The variant's sources (malloc'd; NULL when the program can't have one). The vertex variant takes the previous frame's
// constant buffers from one buffer (arena_slot, bound whole) at the byte offsets in a table of 18 uints (off_slot).
char *mv_vs_msl(const char *msl, uint32_t cbuf_mask, int arena_slot, int off_slot);
char *mv_fs_msl(const char *msl);
// The HUD's (UI layer, MetalFX keeps it still): colour 0 to MV_TARGET as well, by function constant 0 - 0 blended as
// colour 0 (alpha: coverage), 1 opaque, 2 transparent black (clears the layer)
char *mv_fs_ui_msl(const char *msl);
#define UI_FORMAT MTLPixelFormatRGBA16Float  // (the layer: coverage in alpha)

// A draw's vertex constant buffers (hardware slots in mask, k >= 3: the game's uniform buffers): copied to this frame's
// history under its group (shaders, mesh, range), and the buffer and offsets to bind for the previous position - the
// previous frame's copy of the same draw (motion.m: matched by contents), or this one's (no motion) when there was
// none. `ids[k]` names data[k]'s contents within the frame (same id: same bytes, copied once). False: the history is
// full this frame.
bool mv_record(uint64_t frame, uint64_t group, uint32_t mask, const uint8_t *const data[18], const uint32_t size[18],
               const uint64_t ids[18], id<MTLBuffer> *arena, uint32_t offs[18], bool *matched);
void mv_stats(uint64_t *draws, uint64_t *matched, uint64_t *bytes);  // (since the last call)
void mv_reset(void);  // (reboot: no history)
void mv_save(id<MTLTexture> mv, const char *path);  // (debug: an image of the motion target)
void mv_save_coverage(id<MTLTexture> ui, const char *path);  // (debug: the UI layer's coverage)

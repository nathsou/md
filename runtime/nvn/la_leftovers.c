// Stand-ins for Link's Awakening features the shared renderer code still calls (see texpatch.h): no texture is ever
// one of them. To be removed together with their call sites.
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "nvn.h"

int renderer_texpatch_of(NvnTexture *t);
int texpatch_bottle_kind(int id) { (void)id; return 0; }
int texpatch_heart_kind(int id) { (void)id; return 0; }
bool texpatch_is_item_cursor(int id) { (void)id; return false; }
NvnTexture *texpatch_hint_icon(void) { return NULL; }
void texpatch_heart_tint(int kind, float tint[8]) { (void)kind, (void)tint; }
bool pets_hide_item_cursor(void) { return false; }

// Widescreen (Link's Awakening's camera patches): never allowed, so outputs stay 16:9 with bars.
bool widescreen_allowed(void) { return false; }
void widescreen_set_factor(float f) { (void)f; }
float widescreen_effect_stretch(uint64_t fs_hash) { (void)fs_hash; return 1.0f; }
void widescreen_backdrop_seen(uint64_t hash) { (void)hash; }
void widescreen_frame_end(uint64_t frame) { (void)frame; }
void widescreen_scene_ubo(const float *u, uint64_t size) { (void)u, (void)size; }
bool widescreen_ui_pane_partial(const float *u, uint64_t size) { (void)u, (void)size; return false; }
void widescreen_present_crop(uint64_t frame, double shape, float crop[4], double *vx, double *vy, double *vw,
                             double *vh) {
    (void)frame, (void)shape, (void)vx, (void)vy, (void)vw, (void)vh;
    crop[0] = crop[1] = 0, crop[2] = crop[3] = 1;
}
uint64_t game_camera_zone(void) { return 0; }
void scene_size_log(bool composite, uint32_t sw, uint32_t sh, uint32_t w, uint32_t h) {
    (void)composite, (void)sw, (void)sh, (void)w, (void)h;
}

// Mirror mode, pot glow, hint sheet, escape bottle: never active.
bool mirror_draw(bool composite) { (void)composite; return false; }
bool mirror_input(void) { return false; }
bool mirror_scene_size(uint32_t sw, uint32_t sh, uint32_t w, uint32_t h) { (void)sw, (void)sh, (void)w, (void)h; return false; }
bool world_on_screen(void) { return false; }
bool pot_glow_active(void) { return false; }
bool pot_level_active(void) { return false; }
void pot_gold_tint(float tint[8]) { (void)tint; }
bool pot_instance_gold(const float *m) { (void)m; return false; }
void pot_item_draw(float *m) { (void)m; }
void pot_probe(const float *m, bool tintable, uint64_t mesh, uint32_t inst) { (void)m, (void)tintable, (void)mesh, (void)inst; }
bool pot_tint(const float *m, float tint[8], uint64_t mesh) { (void)m, (void)tint, (void)mesh; return false; }
bool hint_sheet_icon_draw(const float *ui, uint32_t ui_size) { (void)ui, (void)ui_size; return false; }
void escape_bottle_tint(int kind, const float *ui, uint32_t ui_size, float tint[8]) { (void)kind, (void)ui, (void)ui_size, (void)tint; }

// Game state probes (Link's Awakening's game manager): unknown for Dread.
int game_in_play(void) { return 0; }
const char *game_level_name(void) { return ""; }  // (never NULL: the renderers compare and hash it)
int cheats_link_health(int *max) { if (max) *max = 0; return -1; }
void warp_frame(uint64_t frame) { (void)frame; }
void save_editor_frame(uint64_t frame) { (void)frame; }
void save_editor_file_read(const char *path, uint64_t addr, int64_t off, int64_t size) { (void)path, (void)addr, (void)off, (void)size; }
void flags_ext_save_read(const char *path, uint64_t addr, int64_t off, int64_t size) { (void)path, (void)addr, (void)off, (void)size; }
bool flags_ext_save_written(const char *path, uint64_t addr, int64_t off, int64_t size) { (void)path, (void)addr, (void)off, (void)size; return false; }
void romfs_file_read_hook(const char *path) { (void)path; }
void run_timer_ending_movie(void) {}
void run_timer_title(void) {}

// Scripted-input state LASRecomp's warp / door-jump tests shared with runtime/hle/input.c
uint64_t g_stick_freeze_until, g_test_buttons, g_test_buttons_until, g_test_stick_until;
int32_t g_stick_lx, g_stick_ly, g_test_lx, g_test_ly;
char g_window_shot_request[256];
void input_note_rstick(int32_t ry) { (void)ry; }

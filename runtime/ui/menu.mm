// In-game overlay menu: none yet for Dread (LASRecomp's ImGui menu was built around Link's Awakening's tabs).
// What the renderers and input call is kept, so a Dread menu can slot in here. F11 toggles fullscreen meanwhile.
#include "menu.h"

#include <SDL3/SDL.h>
#include <atomic>
#include <string.h>

#include "../settings.h"

#define DREADRECOMP_VERSION "0.0.1-dev"

static std::atomic<bool> g_want_fullscreen_apply{true};

extern "C" {

const char *larecomp_version(void) { return DREADRECOMP_VERSION; }

bool menu_is_open(void) { return false; }
bool menu_input_captured(void) { return false; }
void menu_test_open(void) {}

// Name entry (runtime/hle/swkbd.c): no on-screen keyboard yet - the game gets its initial text back.
int menu_keyboard_run(const char *initial, int max_len, char *out, int cap) {
    (void)max_len;
    snprintf(out, cap, "%s", initial ? initial : "");
    return 1;
}

bool menu_handle_event(const SDL_Event *e) {
    if (e->type == SDL_EVENT_KEY_DOWN && e->key.key == SDLK_F11 && !e->key.repeat) {
        g_settings.fullscreen = !g_settings.fullscreen;
        g_want_fullscreen_apply = true;
        return true;
    }
    return false;
}

extern "C" uint32_t g_window_resize_request;  // (runtime/hle/input.c: test scripts' "resize")
void menu_apply_window_requests(void *win) {
    SDL_Window *w = (SDL_Window *)win;
    if (g_want_fullscreen_apply.exchange(false)) SDL_SetWindowFullscreen(w, g_settings.fullscreen);
    if (uint32_t r = __atomic_exchange_n(&g_window_resize_request, 0u, __ATOMIC_RELAXED))
        SDL_SetWindowSize(w, (int)(r >> 16), (int)(r & 0xffff));
}

#if defined(__APPLE__)
void menu_render(id<MTLDevice> dev, id<MTLCommandBuffer> cb, id<MTLTexture> target) { (void)dev, (void)cb, (void)target; }
#else
void menu_render(void *cmd, uint32_t target_w, uint32_t target_h) { (void)cmd, (void)target_w, (void)target_h; }
#endif

}  // extern "C"

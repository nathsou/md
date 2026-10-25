// Hook points: instructions of the game's code that call into the runtime (tools/patches.py HOOKS). The generated
// code at a hook site runs `rt_hook(c, site)` first; what a hook does lives here, so changing it rebuilds the app,
// not the game code.
//
// rt_hook returns 0: run the site's own instruction, 1: the hook did it all, 2..: jump to the site's targets
// (HOOKS[site][1], in order).
#include "../game_addresses.h"
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "../cpu.h"
#include "../rt.h"

// The hook sites (game_addresses.h): which one a site is, by the selected game version's table. None yet for Dread:
// add X(NAME_SITE) here, the address to game_addresses/patches_<version>.h and the site to tools/patches.py HOOKS.
#define HOOK_SITES(X)
enum {
#define X(name) H_##name,
    HOOK_SITES(X)
#undef X
    H_COUNT
};
static const size_t hook_field[H_COUNT + 1] = {
#define X(name) offsetof(GameAddrs, name),
    HOOK_SITES(X)
#undef X
};

static int hook_id(uint64_t site) {
    for (size_t i = 0; i < H_COUNT; i++)
        if (*(const uint64_t *)((const char *)g_ga + hook_field[i]) == site) return (int)i;
    return -1;
}

int rt_hook(Cpu *c, uint64_t site) {
    (void)c;
    switch (hook_id(site)) {
    default:
        break;
    }
    return 0;
}

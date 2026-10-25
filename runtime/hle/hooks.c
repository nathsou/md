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
#include <unistd.h>

#include "../cpu.h"
#include "../rt.h"

// The hook sites (game_addresses.h): which one a site is, by the selected game version's table. None yet for Dread:
// add X(NAME_SITE) here, the address to game_addresses/patches_<version>.h and the site to tools/patches.py HOOKS.
#define HOOK_SITES(X) \
    X(MOVIE_LINK_GATE_SITE) \
    X(MOVIE_UNLINK_GATE_SITE)
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

// The movie player's main thread posts "link / unlink these wait holders" requests into its dispatcher thread's ring,
// then frees the objects and builds the next movie's in the same memory without waiting. On the Switch the dispatcher
// (higher priority, same core) runs as soon as it is signalled, so the work is done first; here every thread runs in
// parallel, and the dispatcher's WaitAny walked freed holders, or a worker read a freed object (about one load in
// four crashed: checkpoints 013, 020, 029...). Wait until the ring is empty (its count, dispatcher +0x370) - with a
// bound, so a dispatcher that never runs again can't hang the game. (nxnative found this on 2.1.0.)
static void movie_ring_wait(uint64_t dispatcher) {
    volatile uint64_t *count = (volatile uint64_t *)(uintptr_t)(dispatcher + 0x370);
    uint64_t t0 = plat_now_ns();
    while (*count && plat_now_ns() - t0 < 200000000ull) usleep(50);
}

int rt_hook(Cpu *c, uint64_t site) {
    switch (hook_id(site)) {
    case H_MOVIE_LINK_GATE_SITE:
    case H_MOVIE_UNLINK_GATE_SITE:
        movie_ring_wait(c->x[19]);
        return 0;
    default:
        break;
    }
    return 0;
}

// Metroid Dread's hook points: instructions of its code that call into the runtime (games/dread/patches.py HOOKS).
// The generated code at a hook site runs `rt_hook(c, site)` first (runtime/game.c hands it to dread_hook); what a
// hook does lives here, so changing it rebuilds the app, not the game code.
//
// dread_hook returns 0: run the site's own instruction, 1: the hook did it all, 2..: jump to the site's targets
// (HOOKS[site][1], in order).
#include "dread.h"
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "cpu.h"
#include "rt.h"

// The hook sites: which one a site is, by the running version's table. A new one: X(NAME_SITE) here, the address in
// patches_<version>.h and the site in patches.py HOOKS.
#define HOOK_SITES(X) \
    X(MOVIE_LINK_GATE_SITE) \
    X(MOVIE_UNLINK_GATE_SITE) \
    X(MOVIE_DISPATCH_STOP_SITE) \
    X(MOVIE_DISPATCH_STOPPED_SITE) \
    X(MOVIE_DISPATCH_RETURN_SITE)
enum {
#define X(name) H_##name,
    HOOK_SITES(X)
#undef X
    H_COUNT
};
static const size_t hook_field[H_COUNT + 1] = {
#define X(name) offsetof(DreadAddrs, name),
    HOOK_SITES(X)
#undef X
};

static int hook_id(uint64_t site) {
    for (size_t i = 0; i < H_COUNT; i++)
        if (*(const uint64_t *)((const char *)game_addrs() + hook_field[i]) == site) return (int)i;
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
    if (*count && getenv("LARECOMP_LOG_MOVIE")) {
        extern void kernel_dump_threads(const char *prefix);
        LOG("[movie] dispatcher %#llx still has %llu request(s) after 200 ms\n", (unsigned long long)dispatcher,
            (unsigned long long)*count);
        kernel_dump_threads("[movie] ");
    }
    // ...and until the dispatcher (more urgent, same core) has finished with it and waits again: the ring count drops
    // when it takes the request, before it is done (with only the count, about one load in five still crashed)
    extern void kernel_yield_to_urgent(uint64_t max_ns);
    kernel_yield_to_urgent(50000000ull);
}

int dread_hook(Cpu *c, uint64_t site) {
    switch (hook_id(site)) {
    case H_MOVIE_LINK_GATE_SITE:
    case H_MOVIE_UNLINK_GATE_SITE:
        if (getenv("LARECOMP_LOG_MOVIE"))  // (the object whose four wait holders are at +0x60 + 0x48 / 0xd0 / 0x158 / 0x1e0)
            LOG("[movie] %s object %#llx (holders %#llx..) dispatcher %#llx t=%llu\n",
                site == GA(MOVIE_LINK_GATE_SITE) ? "link" : "unlink", (unsigned long long)c->x[20],
                (unsigned long long)(c->x[20] + 0x60 + 0x48), (unsigned long long)c->x[19],
                (unsigned long long)(plat_now_ns() / 1000));
        movie_ring_wait(c->x[19]);
        return 0;
    case H_MOVIE_DISPATCH_RETURN_SITE: {
        // The dispatcher job returns when its idle event fires (the player has nothing linked any more) - and the
        // player signals that right after posting its last unlink request. On the Switch the dispatcher (more urgent,
        // same core) preempts at the request's signal and takes it before the idle one exists; here both can be set
        // by the time its WaitAny looks, the idle one wins, and the job left with the unlink pending: the player freed
        // the object, the next movie's link restarted the job, and WaitAny walked the freed holder (about one load in
        // ten crashed). Take what is pending before returning, as the Switch's order would have.
        uint64_t ring = c->x[20];
        if (ring && *(volatile uint64_t *)(uintptr_t)(ring + 0x370)) {
            if (getenv("LARECOMP_LOG_MOVIE"))
                LOG("[movie] dispatcher %#llx returning with %llu request(s) pending: processing them first\n",
                    (unsigned long long)ring, (unsigned long long)*(volatile uint64_t *)(uintptr_t)(ring + 0x370));
            uint64_t saved[19];
            memcpy(saved, c->x, sizeof saved);
            c->x[0] = ring;
            guest_call(c, GA(MOVIE_DISPATCH_PROCESS));
            memcpy(c->x, saved, sizeof saved);
        }
        return 0;
    }
    case H_MOVIE_DISPATCH_STOP_SITE:
    case H_MOVIE_DISPATCH_STOPPED_SITE:
        if (getenv("LARECOMP_LOG_MOVIE")) {
            extern uint32_t kernel_current_thread_handle(void);
            LOG("[movie] dispatcher %#llx %s (thread obj state %u) by handle %#x t=%llu\n", (unsigned long long)c->x[19],
                site == GA(MOVIE_DISPATCH_STOP_SITE) ? "stopping" : "stopped",
                *(volatile uint32_t *)(uintptr_t)(c->x[19] + 0x1e4), kernel_current_thread_handle(),
                (unsigned long long)(plat_now_ns() / 1000));
        }
        return 0;
    default:
        break;
    }
    return 0;
}

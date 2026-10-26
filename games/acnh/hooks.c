// Animal Crossing: New Horizons's hook points (games/acnh/patches.py HOOKS), handed over by runtime/game.c rt_hook.
// acnh_hook returns 0: run the site's own instruction, 1: the hook did it all, 2..: jump to the site's targets.
#include "acnh.h"
#include "rt.h"

int acnh_hook(Cpu *c, uint64_t site) {
    (void)c, (void)site;
    return 0;
}

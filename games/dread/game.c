// Metroid Dread's profile (runtime/game.h): its address tables and hooks (hooks.c).
#include "game.h"
#include "dread.h"

#define GA_ADDR GAME_ADDR_INIT
static const DreadAddrs a_v1_0_0 = {
#include "patches_v1_0_0.h"
#include "runtime_v1_0_0.h"
};
static const DreadAddrs a_v2_1_0 = {
#include "patches_v2_1_0.h"
#include "runtime_v2_1_0.h"
};
#undef GA_ADDR

static const GameVersion versions[] = {
#define GAME_ID(id, name)
#define GA_VERSION(key, label, build_id) {#key, label, build_id, &a_##key},
#include "profile.h"
#undef GAME_ID
#undef GA_VERSION
};

static const GameProfile dread = {
    .id = "dread",
    .name = "Metroid Dread",
    .versions = versions,
    .nversions = sizeof versions / sizeof versions[0],
    .hook = dread_hook,
    .flags = GAME_CLEAR_SCREEN_TARGETS,
};
RT_GAME(dread);

// Animal Crossing: New Horizons's profile (runtime/game.h): its address tables.
#include "acnh.h"

#define GA_ADDR GAME_ADDR_INIT
static const AcnhAddrs a_v1_0_0 = {
#include "patches_v1_0_0.h"
#include "runtime_v1_0_0.h"
};
static const AcnhAddrs a_v2_0_7 = {
#include "patches_v2_0_7.h"
#include "runtime_v2_0_7.h"
};
#undef GA_ADDR

static const GameVersion versions[] = {
#define GAME_ID(id, name)
#define GA_VERSION(key, label, build_id) {#key, label, build_id, &a_##key},
#include "profile.h"
#undef GAME_ID
#undef GA_VERSION
};

static const GameProfile acnh = {
    .id = "acnh",
    .name = "Animal Crossing: New Horizons",
    .versions = versions,
    .nversions = sizeof versions / sizeof versions[0],
    .hook = acnh_hook,
};
RT_GAME(acnh);

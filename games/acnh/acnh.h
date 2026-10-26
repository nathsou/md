// Animal Crossing: New Horizons's profile: its address struct (GA(NAME): the running version's address) and hooks.
#pragma once
#include "game.h"

typedef struct AcnhAddrs {
#define GA_ADDR GAME_ADDR_FIELD
#include "patches_v1_0_0.h"
#include "runtime_v1_0_0.h"
#undef GA_ADDR
} AcnhAddrs;
#define GA(name) (((const AcnhAddrs *)game_addrs())->name)

int acnh_hook(Cpu *c, uint64_t site);  // hooks.c

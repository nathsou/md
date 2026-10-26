// Metroid Dread's profile: its address struct (GA(NAME): the running version's address) and hook handler.
#pragma once
#include "game.h"

typedef struct DreadAddrs {
#define GA_ADDR GAME_ADDR_FIELD
#include "patches_v1_0_0.h"
#include "runtime_v1_0_0.h"
#undef GA_ADDR
} DreadAddrs;
#define GA(name) (((const DreadAddrs *)game_addrs())->name)

int dread_hook(Cpu *c, uint64_t site);  // hooks.c

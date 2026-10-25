// Metroid Dread 1.0.0: addresses used by the patch generator (tools/patches.py), part of the game library's identity.
// An X-macro list (no include guard): included with GA_ADDR(name, value) defined (runtime/game_addresses.h).

// The main program's load address (a table needs at least one entry; also a sanity anchor for the tools)
GA_ADDR(MAIN_TEXT,                        0x7000000000)

// Movie player: after each link / unlink request it posts to its dispatcher thread's request ring (main+0x222838),
// on the `add x22, x22, #8` after the bl (x19 = the dispatcher, ring count at +0x370) - runtime/hle/hooks.c
GA_ADDR(MOVIE_LINK_GATE_SITE,             0x7000224314)
GA_ADDR(MOVIE_UNLINK_GATE_SITE,           0x7000224454)

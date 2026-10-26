// Metroid Dread 1.0.0: addresses used by the patch generator (games/dread/patches.py), part of the game library's identity.
// An X-macro list (no include guard): included with GA_ADDR(name, value) defined (games/dread/dread.h).

// The main program's load address (a table needs at least one entry; also a sanity anchor for the tools)
GA_ADDR(MAIN_TEXT,                        0x7000000000)

// Movie player: after each link / unlink request it posts to its dispatcher thread's request ring (main+0x222838),
// on the `add x22, x22, #8` after the bl (x19 = the dispatcher, ring count at +0x370) - games/dread/hooks.c
GA_ADDR(MOVIE_LINK_GATE_SITE,             0x7000224314)
GA_ADDR(MOVIE_UNLINK_GATE_SITE,           0x7000224454)
// Movie dispatcher teardown (main+0x222720): around its thread stop (bl main+0x667cc: state 3, DestroyThread) - logged
GA_ADDR(MOVIE_DISPATCH_STOP_SITE,         0x700022273c)
GA_ADDR(MOVIE_DISPATCH_THREAD_STOP,       0x70000667cc)
GA_ADDR(MOVIE_DISPATCH_STOPPED_SITE,      0x7000222740)
// Movie dispatcher job (main+0x222624): its return (`ldp x29, x30` at the epilogue; x20 = the ring, main+0x222904
// processes it) - games/dread/hooks.c drains the ring first
GA_ADDR(MOVIE_DISPATCH_RETURN_SITE,       0x700022270c)
GA_ADDR(MOVIE_DISPATCH_PROCESS,           0x7000222904)

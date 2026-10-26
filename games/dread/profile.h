// Metroid Dread: the profile's identity and the versions it has address tables for (patches_<key>.h: addresses the
// generated code depends on, runtime_<key>.h: addresses only the runtime uses). Read by games/dread/game.c and by
// tools/games.py; one GAME_ID line, then one GA_VERSION line per version (key, label, main build ID).
GAME_ID(dread, "Metroid Dread")
GA_VERSION(v1_0_0, "1.0.0", "49161D9CCBC15DF944D0B6278A3C446C006B0BE8")
GA_VERSION(v2_1_0, "2.1.0", "646761F643AFEBB379EDD5E6A5151AF2CEF93DC1")

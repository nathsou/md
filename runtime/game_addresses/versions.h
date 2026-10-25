// The supported game versions, by the main program's build ID (exefs/main, 0x40..0x53). Each one has its own
// address tables: game_addresses/patches_<key>.h and runtime_<key>.h, with the same names. An unknown build ID is
// refused: there is no fallback table. tools/game_addresses.py reads this same file.
// An X-macro list (no include guard): GA_VERSION(key, label, main build ID) and
// GA_RELEASE(key, label, SHA-256 of exefs/main.npdm).

GA_VERSION(v1_0_0, "1.0.0", "49161D9CCBC15DF944D0B6278A3C446C006B0BE8")

// Entry of the game code library (runtime/gamelib.h), compiled into it with the generated code: the module tables
// (gamelib_modules.c, written by tools/gen_game.py), found by name when the app loads the library. Everything else in
// it stays hidden.
#include "gamelib.h"

extern const GameLibModule gamelib_modules[];
extern const uint32_t gamelib_nmodules;

GAMELIB_EXPORT const GameLibTables *gamelib_entry_tables(void) {
    static GameLibTables t;
    t = (GameLibTables){GAMELIB_ABI, GAMELIB_ID, gamelib_nmodules, gamelib_modules};
    return &t;
}

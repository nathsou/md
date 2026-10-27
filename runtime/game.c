// The running game: identified from its exefs, matched against the registered profiles (game.h).
#include "game.h"

#include <stdio.h>
#include <string.h>

#include "rt.h"

GameInfo g_game;
const char *menu_game_name(void) { return g_game.name[0] ? g_game.name : "(no game)"; }  // (runtime/ui/menu.mm)

#ifdef __APPLE__
extern const GameProfile *const __start_games[] __asm__("section$start$__DATA$__rt_games");
extern const GameProfile *const __stop_games[] __asm__("section$end$__DATA$__rt_games");
#elif defined(__linux__)
extern const GameProfile *const __start_rt_games[], *const __stop_rt_games[];  // (ELF: defined by the linker)
#define __start_games __start_rt_games
#define __stop_games __stop_rt_games
#else
__attribute__((section(".gam$a"), used)) static const GameProfile *const __start_games[1];
__attribute__((section(".gam$z"), used)) static const GameProfile *const __stop_games[1];
#endif

static int hexval(char c) { return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10; }

static bool build_id_is(const uint8_t id[20], const char *hex) {
    for (int k = 0; k < 20; k++)
        if (id[k] != (uint8_t)(hexval(hex[2 * k]) << 4 | hexval(hex[2 * k + 1]))) return false;
    return true;
}

// main.npdm: META, whose ACI0 (offset at +0x70) holds the program ID at +0x10. 0 when it can't be read.
static uint64_t npdm_title_id(const char *path) {
    uint8_t buf[0x1000];
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    size_t n = fread(buf, 1, sizeof buf, f);
    fclose(f);
    if (n < 0x80 || memcmp(buf, "META", 4)) return 0;
    uint32_t aci = *(uint32_t *)(buf + 0x70);
    if (aci + 0x18 > n || memcmp(buf + aci, "ACI0", 4)) return 0;
    return *(uint64_t *)(buf + aci + 0x10);
}

bool game_identify(const char *game_dir, GameInfo *g, char *why, size_t why_size) {
    memset(g, 0, sizeof *g);
    char path[1100];
    uint8_t hdr[0x60];
    snprintf(path, sizeof path, "%s/exefs/main", game_dir);
    FILE *f = fopen(path, "rb");
    bool ok = f && fread(hdr, 1, sizeof hdr, f) == sizeof hdr && !memcmp(hdr, "NSO0", 4);
    if (f) fclose(f);
    if (!ok) {
        snprintf(why, why_size, "%s is missing or not a Switch program (extract the game with extract.mts)", path);
        return false;
    }
    memcpy(g->build_id, hdr + 0x40, 20);
    snprintf(path, sizeof path, "%s/exefs/main.npdm", game_dir);
    g->title_id = npdm_title_id(path);
    const GameProfile *const *start = __start_games, *const *stop = __stop_games;
    __asm__("" : "+r"(start), "+r"(stop));  // (separate objects: keep the compiler from reasoning about them)
    for (const GameProfile *const *p = start; p < stop && !g->version; p++) {
        if (!*p) continue;
        for (int i = 0; i < (*p)->nversions; i++)
            if (build_id_is(g->build_id, (*p)->versions[i].build_id)) g->profile = *p, g->version = &(*p)->versions[i];
    }
    if (g->profile) {
        snprintf(g->key, sizeof g->key, "%s", g->profile->id);
        snprintf(g->name, sizeof g->name, "%s", g->profile->name);
    } else {
        snprintf(g->key, sizeof g->key, "title-%016llX", (unsigned long long)g->title_id);
        snprintf(g->name, sizeof g->name, "%s", g->key);
    }
    return true;
}

bool game_select(const char *game_dir, char *why, size_t why_size) {
    if (!game_identify(game_dir, &g_game, why, why_size)) return false;
    const uint8_t *b = g_game.build_id;
    if (g_game.profile)
        LOG("[game] %s %s (title %016llX, main build ID %02X%02X%02X%02X%02X%02X%02X%02X)\n", g_game.name,
            g_game.version->label, (unsigned long long)g_game.title_id, b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]);
    else
        LOG("[game] title %016llX, main build ID %02X%02X%02X%02X%02X%02X%02X%02X: no game profile (games/), running "
            "without game-specific fixes\n", (unsigned long long)g_game.title_id, b[0], b[1], b[2], b[3], b[4], b[5],
            b[6], b[7]);
    return true;
}

// The generated code's hook sites (games/<id>/patches.py HOOKS): the profile decides.
int rt_hook(Cpu *c, uint64_t site) {
    return g_game.profile && g_game.profile->hook ? g_game.profile->hook(c, site) : 0;
}

// The running game's folder name under cache/ (its profile id, or title-<id>)
const char *game_cache_key(void) { return g_game.key[0] ? g_game.key : "unknown"; }

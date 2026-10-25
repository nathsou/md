"""Per-instruction replacements applied by gen.py (guest address -> function(gen, addr) returning C code), and hook
sites (address -> (original instruction word, jump targets, note)) whose runtime side is runtime/hle/hooks.c.

Every site checks the original instruction word, so a wrong address fails loudly instead of silently. Addresses come
from the selected version's table (runtime/game_addresses/patches_<version>.h) through game_addresses.ADDRESS.
"""
from game_addresses import ADDRESS as A  # noqa: F401 (for the patches to come)

PATCHES = {}
HOOKS = {}

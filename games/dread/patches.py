"""Per-instruction replacements applied by gen.py (guest address -> function(gen, addr) returning C code), and hook
sites (address -> (original instruction word, jump targets, note)) whose runtime side is games/dread/hooks.c.

Every site checks the original instruction word, so a wrong address fails loudly instead of silently. Addresses come
from the selected version's table (games/dread/patches_<version>.h) through games.ADDRESS.
"""
from games import ADDRESS as A  # noqa: F401 (for the patches to come)

PATCHES = {}
HOOKS = {}

# Movie player request gates (games/dread/hooks.c): the game waits for its movie dispatcher to take each posted
# link / unlink request, as the dispatcher's preemption makes it on the Switch (nxnative's "observation gates").
HOOKS[A.MOVIE_LINK_GATE_SITE] = (0x910022d6, [], 'add x22, x22, #8 after the link request')
HOOKS[A.MOVIE_UNLINK_GATE_SITE] = (0x910022d6, [], 'add x22, x22, #8 after the unlink request')

# Movie dispatcher teardown, before and after it stops its thread (LARECOMP_LOG_MOVIE: games/dread/hooks.c)
HOOKS[A.MOVIE_DISPATCH_STOP_SITE] = (
    0x94000000 | ((A.MOVIE_DISPATCH_THREAD_STOP - A.MOVIE_DISPATCH_STOP_SITE) >> 2 & 0x3ffffff), [],
    'bl: stop the dispatcher thread')
HOOKS[A.MOVIE_DISPATCH_STOPPED_SITE] = (0x394c6268, [], 'after the stop')

# Movie dispatcher job return (games/dread/hooks.c: pending ring requests are processed before it returns)
HOOKS[A.MOVIE_DISPATCH_RETURN_SITE] = (0xa9437bfd, [], 'ldp x29, x30, [sp, #0x30]: the dispatcher job returns')

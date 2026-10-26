"""Game profiles (games/<id>/), as the generator sees them.

Each profile's profile.h lists its versions by the main program's build ID (GA_VERSION lines); patches_<key>.h holds the
addresses its patches.py uses (GA_ADDR lines; the C runtime includes the same files). A game without a profile is
recompiled without patches or hooks.

select_build_id(build_id) picks the profile and version, fills ADDRESS from that version's table and returns the
profile, or None when no profile lists the build ID. patches(profile) then loads its patches.py, which reads ADDRESS
when it is imported.
"""
import importlib.util
from pathlib import Path
import re
import sys
from types import SimpleNamespace

ROOT = Path(sys._MEIPASS) if hasattr(sys, '_MEIPASS') else Path(__file__).resolve().parent.parent
GAMES_DIR = ROOT / 'games'


def load_table(path):
    values = {}
    for line in Path(path).read_text(encoding='utf-8').splitlines():
        if not line.startswith('GA_ADDR'):
            continue
        match = re.fullmatch(r'GA_ADDR\(([A-Z][A-Z0-9_]*),\s+(0x[0-9a-fA-F]+)\)', line)
        if not match or match[1] in values:
            raise ValueError(f'{path}: invalid or duplicate game-address definition: {line}')
        values[match[1]] = int(match[2], 16)
    return values


class Profile(SimpleNamespace):
    """id, name, dir, versions: {key: (label, build ID as 40 upper-case hex digits)}"""


def load_profile(game_dir):
    text = (game_dir / 'profile.h').read_text(encoding='utf-8')
    m = re.search(r'^GAME_ID\((\w+),\s*"([^"]*)"\)', text, re.M)
    if not m:
        raise ValueError(f'{game_dir}/profile.h: no GAME_ID line')
    versions = {}
    for v in re.finditer(r'^GA_VERSION\((v\w+),\s*"([^"]+)",\s*"([0-9A-F]{40})"\)', text, re.M):
        versions[v[1]] = (v[2], v[3])
    return Profile(id=m[1], name=m[2], dir=game_dir, versions=versions)


def profiles():
    return [load_profile(d) for d in sorted(GAMES_DIR.iterdir()) if (d / 'profile.h').exists()]


ADDRESS = SimpleNamespace()
SELECTED = None  # (profile, version key)


def find_build_id(build_id):
    """(profile, version key) for a main build ID (hex, any case; Nso.build_id strips trailing zero bytes), or None."""
    padded = build_id.upper().ljust(64, '0')  # (the header's 32-byte field: a 20-byte ID, zero-filled)
    if padded[40:].strip('0'):
        return None
    for p in profiles():
        for key, (label, bid) in p.versions.items():
            if padded[:40] == bid:
                return p, key
    return None


def select_build_id(build_id):
    """Fills ADDRESS (in place: modules that imported it see the new values) for the build ID's profile and version;
    returns the profile, or None (no profile: ADDRESS stays empty)."""
    global SELECTED
    ADDRESS.__dict__.clear()
    found = find_build_id(build_id)
    SELECTED = found
    if not found:
        return None
    p, key = found
    for kind in ('patches', 'runtime'):
        path = p.dir / f'{kind}_{key}.h'
        if path.exists():
            ADDRESS.__dict__.update(load_table(path))
    return p


def patches(profile):
    """(HOOKS, PATCHES) from the profile's patches.py ({} when it has none)."""
    path = profile.dir / 'patches.py' if profile else None
    if not path or not path.exists():
        return {}, {}
    spec = importlib.util.spec_from_file_location(f'patches_{profile.id}', path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return getattr(mod, 'HOOKS', {}), getattr(mod, 'PATCHES', {})


def addr_text(code):
    """Expand @NAME@ as hex digits inside generated-C addresses, symbols or labels."""
    return re.sub(r'@([A-Z][A-Z0-9_]*)@', lambda m: f'{getattr(ADDRESS, m[1]):x}', code)

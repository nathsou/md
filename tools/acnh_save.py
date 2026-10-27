"""Animal Crossing: New Horizons saves: decrypt a file, list the residents and the accounts they belong to.

usage: acnh_save.py <save dir>                    the residents (name, island) and the account ID each one is tied to
       acnh_save.py <save dir> --decrypt <name> <out>   decrypts <name>.dat (main, Villager0/personal, ...) to <out>

A resident belongs to one Switch account. To play someone else's save, run with that account:
LARECOMP_USER_ID=<account ID> (otherwise the game starts a new resident on the island). The encryption (the header's
seed table, xorshift128, AES-128-CTR) is the one documented by NHSE (github.com/kwsch/NHSE).
"""
import struct
import subprocess
import sys
from pathlib import Path

M = 0xFFFFFFFF


def xorshift128(seed):
    s = [(0x6C078965 * (seed ^ (seed >> 30)) + 1) & M]
    for i in range(1, 4):
        s.append((0x6C078965 * (s[-1] ^ (s[-1] >> 30)) + i + 1) & M)

    def nxt():
        t = (s[0] ^ (s[0] << 11)) & M
        s[0], s[1], s[2] = s[1], s[2], s[3]
        s[3] = (t ^ (t >> 8) ^ s[3] ^ (s[3] >> 19)) & M
        return s[3]
    return nxt


def param(table, i):
    rand = xorshift128(table[table[i] & 0x7F])
    for _ in range(2 * ((table[table[i + 1] & 0x7F] & 0xF) + 1)):
        rand()
    return bytes(rand() >> 24 for _ in range(16))


def decrypt(base):
    """base: a save file without its extension (<dir>/main): reads base.dat with the key in baseHeader.dat"""
    header = Path(f'{base}Header.dat').read_bytes()
    table = struct.unpack('<128I', header[0x100:0x300])
    key, ctr = param(table, 0), param(table, 2)
    return subprocess.run(['openssl', 'enc', '-d', '-aes-128-ctr', '-K', key.hex(), '-iv', ctr.hex(),
                           '-in', f'{base}.dat'], capture_output=True, check=True).stdout


def name(b):
    s = b.decode('utf-16-le', 'replace').split('\0')[0]
    return s if s and b[2 * len(s):] == bytes(len(b) - 2 * len(s)) and s.isprintable() else None


def residents(main):
    """main.dat holds 8 resident slots of 0x48 bytes: account ID (16), island ID (4) and name (20), a u32, player ID
    (4) and name (20), a u32. Its offset moves between versions: found by its shape (every used slot on the same
    island, the unused ones without an account)."""
    def slot(o):
        uid, island_id = main[o:o + 16], main[o + 0x10:o + 0x14]
        if not uid.strip(b'\0'):
            return 'empty'
        island, player = name(main[o + 0x14:o + 0x28]), name(main[o + 0x30:o + 0x44])
        if not island or not player or not island_id.strip(b'\0') or not main[o + 0x2c:o + 0x30].strip(b'\0'):
            return None
        return uid, island_id, player, island

    for o in range(0, len(main) - 8 * 0x48, 4):
        first = slot(o)
        if not isinstance(first, tuple):
            continue
        slots = [first] + [slot(o + 0x48 * i) for i in range(1, 8)]
        if all(s == 'empty' or (isinstance(s, tuple) and s[1] == first[1]) for s in slots):
            return o, [s for s in slots if s != 'empty']
    return None, []


def main():
    d = sys.argv[1]
    if len(sys.argv) == 5 and sys.argv[2] == '--decrypt':
        Path(sys.argv[4]).write_bytes(decrypt(f'{d}/{sys.argv[3]}'))
        return
    o, found = residents(decrypt(f'{d}/main'))
    if not found:
        sys.exit('no resident table found in main.dat')
    for uid, _, player, island in found:
        print(f'{player} of {island}: LARECOMP_USER_ID={uid.hex()}')
    print(f'(resident table at main.dat {o:#x})')

if __name__ == '__main__':
    main()

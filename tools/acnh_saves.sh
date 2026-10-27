#!/bin/bash
# Animal Crossing check from save files: each save folder (holding main.dat / mainHeader.dat and Villager*/, as a
# Switch save dump or a save/acnh/save_0 copy has them) is booted from a scratch copy as the account its first resident
# belongs to (tools/acnh_save.py), taken through the title and Isabelle's announcement, then walks the island, opens
# the NookPhone and the pockets, and is captured along the way. Once in the day and once at night (the clock shifted
# by NIGHT_OFFSET seconds, default 13 h). A save from an older version is upgraded by the game first (it asks).
# Results: build/acnh_cp/<save>-{day,night}/{frame_*.png,run.log,status}; the saves themselves are not touched.
# usage: tools/acnh_saves.sh <dir of save folders, or one save folder> [jobs]
#        GAME_DIR=data/acnh (default data/acnh-2.0.7) runs another version; EXPLORE=<seconds> (default 90)
set -u
cd "$(dirname "$0")/.."
SRC=${1:?save folder(s)}
JOBS=${2:-2}
GAME_DIR=${GAME_DIR:-data/acnh-2.0.7}
EXPLORE=${EXPLORE:-90}
NIGHT_OFFSET=${NIGHT_OFFSET:-46800}
TIMEOUT=${TIMEOUT:-$((EXPLORE + 150))}
OUT=build/acnh_cp
mkdir -p "$OUT"

# (frames: hidden and uncapped, ~110 presented a second. A through the title, the save upgrade and the announcement;
# then rounds of walking the four ways, the phone and the pockets, with B between them to close whatever opened.)
INPUT=$(python3 - "$EXPLORE" <<'PY'
import sys
start = 7200
end = start + int(sys.argv[1]) * 110
steps = [f"{f}:A:4" for f in range(300, 6600, 60)] + [f"{f}:B:4" for f in range(6600, 7200, 60)]
f, sticks = start, ["STICK_UP", "STICK_LEFT", "STICK_DOWN", "STICK_RIGHT"]
while f < end:
    for stick in sticks:
        steps.append(f"{f}:{stick}:240"); f += 300
        steps.append(f"{f}:B:4"); f += 30
    steps.append(f"{f}:ZL:4"); f += 240
    steps.append(f"{f}:B:4"); f += 150
    steps.append(f"{f}:X:4"); f += 200
    steps.append(f"{f}:B:4"); f += 150
print(",".join(steps))
PY
)
SHOTS=$(python3 - "$EXPLORE" <<'PY'
import sys
start = 7200
shots = [1500, 3000, 4500, 6000, 7100]
f, end = start, start + int(sys.argv[1]) * 110
while f < end:  # (the end of each walk, the phone and the pockets: the same rhythm as the input)
    for _ in range(4):
        shots.append(f + 290); f += 330
    shots.append(f + 230); f += 390
    shots.append(f + 190); f += 350
print(",".join(map(str, shots)))
PY
)

run_one() {
    local dir="$1" when="$2" name
    name=$(basename "$dir" | tr ' ' '_')
    local o="$OUT/$name-$when"
    rm -rf "$o" && mkdir -p "$o/save/save_0"
    rsync -a --exclude .git "$dir/" "$o/save/save_0/"
    local uid
    uid=$(python3 tools/acnh_save.py "$dir" 2>/dev/null | sed -n 's/.*LARECOMP_USER_ID=\([0-9a-f]*\).*/\1/p' | head -1)
    local offset=0
    [ "$when" = night ] && offset=$NIGHT_OFFSET
    local t0=$(date +%s)
    LARECOMP_HIDDEN=1 LARECOMP_DISPLAY_SYNC=0 LARECOMP_UNCAPPED=1 LARECOMP_NO_LAUNCHER=1 LARECOMP_NO_AUDIO=1 \
        LARECOMP_SAVE_DIR="$o/save" LARECOMP_FRAMES_DIR="$o" LARECOMP_USER_ID="$uid" LARECOMP_TIME_OFFSET=$offset \
        LARECOMP_INPUT="$INPUT" LARECOMP_SHOT_AT="$SHOTS" timeout "$TIMEOUT" ./build/nx "$GAME_DIR" > "$o/run.log" 2>&1
    local rc=$?
    local shots=$(ls "$o"/frame_*.png 2>/dev/null | wc -l | tr -d ' ')
    local crash=$(grep -cE '^\*\*\* signal|^FATAL' "$o/run.log")
    echo "$name-$when rc=$rc shots=$shots crash=$crash secs=$(( $(date +%s) - t0 )) user=${uid:-none}" | tee "$o/status"
}

if [ -f "$SRC/mainHeader.dat" ]; then dirs=("$SRC"); else dirs=("$SRC"/*/); fi
i=0
for dir in "${dirs[@]}"; do
    dir=${dir%/}
    [ -f "$dir/mainHeader.dat" ] || continue
    for when in day night; do
        run_one "$dir" "$when" &
        i=$((i + 1))
        if [ $((i % JOBS)) -eq 0 ]; then wait; fi
    done
done
wait

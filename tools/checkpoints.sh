#!/bin/bash
# Whole-game check from a ladder of save files: each checkpoint folder (holding profile0/{samus,common,pkprfl}.bmssv,
# e.g. ZacharyACoon/MetroidDreadSaveFiles' playthrough/) is booted from a scratch copy of its save, continued from the
# title (File 1 -> Continue), and captured at the station it was saved at, after walking left and after walking right.
# Results: build/cp/<NNN>/{frame_*.png,run.log,status}; nothing of the player's own save/ is touched.
# usage: tools/checkpoints.sh <checkpoints dir> [jobs] [first] [last]
set -u
cd "$(dirname "$0")/.."
SRC=${1:?checkpoints dir}
JOBS=${2:-3}
FIRST=${3:-0}
LAST=${4:-999}
OUT=build/cp
mkdir -p "$OUT"
USERDATA=save/save_0/userdata  # (which profiles exist: this machine's, from any earlier boot)

# EXPLORE=<seconds> (default 180): after continuing, alternate runs right and left (about 15 s each) jumping, shooting,
# firing missiles (R + Y) and aiming (L), and capture every 1500 frames; EXPLORE=0: the quick check (a few steps each
# way). TIMEOUT: seconds per checkpoint (default: the exploration plus 150).
EXPLORE=${EXPLORE:-180}
TIMEOUT=${TIMEOUT:-$((EXPLORE + 150))}
if [ "$EXPLORE" -gt 0 ]; then
    INPUT=$(python3 - "$EXPLORE" <<'PY'
import sys
secs = int(sys.argv[1]); start = 7000; end = start + secs * 120  # (frames: ~120 presented a second)
steps = [f"{f}:A:6" for f in (2500, 3000, 3400, 3800, 4200)]
f, d = start, 0
while f < end:
    stick = "STICK_RIGHT" if d % 2 == 0 else "STICK_LEFT"
    steps.append(f"{f}:{stick}:1800")
    for k in range(f, f + 1800, 90): steps.append(f"{k}:B:14")
    for k in range(f + 45, f + 1800, 60): steps.append(f"{k}:Y:4")
    steps.append(f"{f + 900}:R:60"); steps.append(f"{f + 930}:Y:6")
    steps.append(f"{f + 1300}:L:90")
    f += 1800; d += 1
print(",".join(steps))
PY
)
    SHOTS=$(python3 -c "print(','.join(str(f) for f in range(6500, 7000 + $EXPLORE * 120, 1500)))")
else
    INPUT="2500:A:6,3000:A:6,3400:A:6,3800:A:6,4200:A:6,7200:STICK_LEFT:600,8000:STICK_RIGHT:1200"
    SHOTS="6500,7100,7800,9200,9800"
fi

run_one() {
    local dir="$1" n
    n=$(basename "$dir" | cut -d' ' -f1)  # ("NNN - name": NNN; a stress copy "NNNa": NNNa)
    local o="$OUT/$n"
    rm -rf "$o" && mkdir -p "$o/save/save_0"
    cp -R "$dir/profile0" "$o/save/save_0/"
    [ -d "$dir/userdata" ] && cp -R "$dir/userdata" "$o/save/save_0/" || cp -R "$USERDATA" "$o/save/save_0/"
    local t0=$(date +%s)
    LARECOMP_NO_LAUNCHER=1 LARECOMP_NO_AUDIO=1 LARECOMP_SAVE_DIR="$o/save" LARECOMP_FRAMES_DIR="$o" \
        LARECOMP_INPUT="$INPUT" LARECOMP_SHOT_AT="$SHOTS" timeout "$TIMEOUT" ./build/dreadrecomp . > "$o/run.log" 2>&1
    local rc=$?
    local shots=$(ls "$o"/frame_*.png 2>/dev/null | wc -l | tr -d ' ')
    local crash=$(grep -cE '^\*\*\* signal|^FATAL' "$o/run.log")
    echo "$n rc=$rc shots=$shots crash=$crash secs=$(( $(date +%s) - t0 )) $(basename "$dir")" | tee "$o/status"
}

i=0
for dir in "$SRC"/*/; do
    dir=${dir%/}
    n=$(basename "$dir" | cut -c1-3)
    [ "$((10#$n))" -lt "$FIRST" ] && continue 2>/dev/null
    [ "$((10#$n))" -gt "$LAST" ] && continue 2>/dev/null
    [ -d "$dir/profile0" ] || continue
    run_one "$dir" &
    i=$((i + 1))
    if [ $((i % JOBS)) -eq 0 ]; then wait; fi
done
wait

#!/bin/bash
# One comparison image per checkpoint run by tools/checkpoints.sh: the checkpoint's own screenshot (real hardware) then
# this build's frames, in build/cp/<NNN>/sheet.png.
# usage: tools/checkpoint_sheet.sh <checkpoints dir> [NNN...]
cd "$(dirname "$0")/.."
SRC=$1; shift
for o in ${@:-$(ls build/cp)}; do
    o=build/cp/${o##*/}
    n=$(basename "$o")
    ref=$(ls "$SRC"/"$n"*/*.png 2>/dev/null | head -1)
    imgs=()
    [ -n "$ref" ] && imgs+=("$ref")
    imgs+=($(ls "$o"/frame_*.png 2>/dev/null))
    [ ${#imgs[@]} -gt 0 ] && montage "${imgs[@]}" -resize 480x270 -tile 6x -geometry +2+2 -background '#222' "$o/sheet.png" 2>/dev/null
done

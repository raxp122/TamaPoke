#!/bin/bash
# Builds the screen simulator and plays scene scripts.
#   tools/sim/run.sh                 all scenes in tools/sim/scenes/
#   tools/sim/run.sh scenes/main.txt one scene (path relative to tools/sim or the repo)
# Output: tools/sim/out/*.png and tools/sim/out/layout-report.md
set -euo pipefail
cd "$(dirname "$0")/../.."
GFX="${ARDUINO_GFX:-}"
if [[ -z "$GFX" ]]; then  # folder name from git clone or from the Library Manager
  for d in "$HOME/Arduino/libraries/Arduino_GFX" "$HOME/Arduino/libraries/GFX_Library_for_Arduino"; do
    [[ -d "$d" ]] && GFX="$d" && break
  done
fi
if [[ ! -f "$GFX/src/font/glcdfont.h" ]]; then
  echo "Arduino_GFX not found (set ARDUINO_GFX to the library folder)" >&2
  exit 1
fi
BUILD=tools/sim/build
mkdir -p "$BUILD" tools/sim/out
rm -f tools/sim/out/*.png
python3 tools/sim/ino2cpp.py TamaPoke.ino "$BUILD/sim_sketch.cpp"
CXX="${CXX:-g++}"
FLAGS=(-std=gnu++17 -O1 -g -w -DTAMAPOKE_GEN3_RELEASE -DSIM_HOST
       -Itools/sim/stubs -I"$BUILD" -I. -I"$GFX/src")
SRCS=(tools/sim/sim_main.cpp tools/sim/sim_hw.cpp pet.cpp battle.cpp i18n.cpp
      species_chirp.cpp thumb_format.cpp netlogic.cpp sdmon.cpp)
"$CXX" "${FLAGS[@]}" "${SRCS[@]}" -lz -o "$BUILD/tamapoke-sim"
scenes=()
for a in "$@"; do
  if [[ -f "$a" ]]; then scenes+=("$a"); else scenes+=("tools/sim/$a"); fi
done
if [[ ${#scenes[@]} -eq 0 ]]; then scenes=(tools/sim/scenes/*.txt); fi
"$BUILD/tamapoke-sim" -o tools/sim/out -sd tools/sdcard "${scenes[@]}"

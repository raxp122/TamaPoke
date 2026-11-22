#!/bin/bash
# Builds the screen simulator and plays scene scripts.
#   tools/sim/run.sh                 all scenes in tools/sim/scenes/
#   tools/sim/run.sh scenes/main.txt one scene (path relative to tools/sim or the repo)
# Output: tools/sim/out/*.png and tools/sim/out/layout-report.md
# SIM_SANITIZE=1 builds with AddressSanitizer/UBSan (memory errors fail the run).
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
# SIM_SANITIZE=1: AddressSanitizer + UBSan, any memory error stops the run (used by CI)
if [[ "${SIM_SANITIZE:-0}" == 1 ]]; then
  FLAGS+=(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
  export ASAN_OPTIONS="detect_leaks=0:abort_on_error=1" UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=1"
fi
"$CXX" "${FLAGS[@]}" "${SRCS[@]}" -lz -o "$BUILD/tamapoke-sim"
scenes=()
for a in "$@"; do
  if [[ -f "$a" ]]; then scenes+=("$a"); else scenes+=("tools/sim/$a"); fi
done
if [[ ${#scenes[@]} -eq 0 ]]; then scenes=(tools/sim/scenes/*.txt); fi
# every scene starts from a fresh device (new save, empty NVS): one process each
report=tools/sim/out/layout-report.md
echo "# Layout report" > "$report"
echo >> "$report"
echo "OUTSIDE: text or box drawn beyond the round panel. SMALL: button (outlined rounded box) under 44 px, hard to tap." >> "$report"
echo >> "$report"
outside=0; small=0
for sc in "${scenes[@]}"; do
  mkdir -p "$BUILD/run"
  "$BUILD/tamapoke-sim" -o "$BUILD/run" -sd tools/sdcard "$sc" | grep -v '^total'
  cp "$BUILD"/run/*.png tools/sim/out/ 2>/dev/null || true
  rm -f "$BUILD"/run/*.png
  sed -n '/^## /,/^Total:/p' "$BUILD/run/layout-report.md" | grep -v '^Total:' >> "$report"
  o=$(grep -o 'Total: [0-9]* outside' "$BUILD/run/layout-report.md" | grep -o '[0-9]*'); s=$(grep -o ', [0-9]* small' "$BUILD/run/layout-report.md" | grep -o '[0-9]*')
  outside=$((outside + ${o:-0})); small=$((small + ${s:-0}))
done
echo "Total: $outside outside, $small small" >> "$report"
echo "total outside=$outside small=$small (report: $report)"

#!/bin/bash
# Builds and runs the core tests. Binaries and captured screens go to
# tests/out/ (ignored by git); build/ holds only the app.
#   scripts/run_tests.sh          quick tests (slots/mappers, PSG, VDP, BASIC if ROMs are present)
#   scripts/run_tests.sh --zexall also the Z80 exerciser (about 45 s)
set -euo pipefail
cd "$(dirname "$0")/.."

OUT=tests/out
CB=Sources/KorMSX/Resources/cbios
INC="-ISources/MSXCore/include -ISources/MSXCore"
CORE="Sources/MSXCore/z80.c Sources/MSXCore/vdp.c Sources/MSXCore/psg.c Sources/MSXCore/v9938.c Sources/MSXCore/rtc.c"
mkdir -p "$OUT"

# shellcheck disable=SC2086
clang -O1 $INC -o "$OUT/mappertest" tests/mappertest.c $CORE -lm
# shellcheck disable=SC2086
clang -O2 $INC -o "$OUT/boottest" tests/boottest.c Sources/MSXCore/*.c -lm
clang -O2 -o "$OUT/zextest" tests/zextest.c Sources/MSXCore/z80.c

echo "== slots, sub-slots, mappers, SCC"
"$OUT/mappertest"

echo "== PSG tone (expect 873.9 Hz)"
"$OUT/boottest" 240 "$OUT/psgtone.ppm" cbios $CB/cbios_main_msx1.rom $CB/cbios_logo_msx1.rom tests/data/psgtone.rom

echo "== V9938 command engine (screen in $OUT/vdpcmd.ppm)"
KORMSX_SUB=$CB/cbios_sub.rom "$OUT/boottest" 600 "$OUT/vdpcmd.ppm" cbios2 \
    $CB/cbios_main_msx2.rom $CB/cbios_logo_msx2.rom tests/data/vdpcmd.rom

echo "== V9938 80-column text (screen in $OUT/text80.ppm)"
KORMSX_SUB=$CB/cbios_sub.rom "$OUT/boottest" 600 "$OUT/text80.ppm" cbios2 \
    $CB/cbios_main_msx2.rom $CB/cbios_logo_msx2.rom tests/data/text80.rom

if [[ -f roms/MSX.ROM ]]; then
    echo "== MSX1 BASIC: print 6*7 (screen in $OUT/basic1.ppm)"
    KORMSX_TYPE=$'print 6*7\n' "$OUT/boottest" 600 "$OUT/basic1.ppm" cbios roms/MSX.ROM
fi
if [[ -f roms/MSX2.ROM && -f roms/MSX2EXT.ROM ]]; then
    echo "== MSX2 BASIC: print fre(0) (screen in $OUT/basic2.ppm)"
    KORMSX_TYPE=$'print fre(0)\n' "$OUT/boottest" 900 "$OUT/basic2.ppm" msx2 roms/MSX2.ROM roms/MSX2EXT.ROM
fi

if [[ "${1:-}" == "--zexall" ]]; then
    echo "== ZEXALL"
    "$OUT/zextest" tests/data/zexall.com | tr '\r' '\n' | grep -v '^$'
fi

#!/bin/bash
# Exercise the proxy + widescreen plugin in isolation (no game/Steam needed)
# under the CrossOver bottle's wine. Expects "RESULT: OK" and a windowed
# device; with Enabled=0 in the ini the same request FAILS (which is what the
# stock game hits in exclusive fullscreen).
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
WINE="${WINE:-/Applications/CrossOver.app/Contents/SharedSupport/CrossOver/bin/wine}"
export CX_BOTTLE="${CX_BOTTLE:-Steam}"
export WINEDEBUG=-all

i686-w64-mingw32-gcc -O2 -o "$HERE/harness.exe" "$HERE/harness.c" \
    -ld3d8 -luser32 -lgdi32

STAGE=/tmp/h2test
rm -rf "$STAGE"; mkdir -p "$STAGE/scripts"
cp "$ROOT/dist/d3d8.dll" "$STAGE/"
cp "$ROOT/dist/H2SAWidescreen.asi" "$STAGE/scripts/"
printf '[Widescreen]\nEnabled=1\nBorderless=1\nFOVCorrect=1\nFOVFactor=1.0\n' \
    > "$STAGE/scripts/H2SAWidescreen.ini"
cp "$HERE/harness.exe" "$STAGE/"

"$WINE" "$STAGE/harness.exe" "Z:\\tmp\\h2test\\d3d8.dll"
echo "--- loader log ---"
cat "$STAGE/scripts/H2SAAsiLoader.log"

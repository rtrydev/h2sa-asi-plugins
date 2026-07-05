#!/bin/bash
# Differential tester for the RenderD3D.dll x87 -> SSE2 translation.
#
# Builds difftest.exe, stages it plus the .x87 blob and the leaf-function
# manifest into the game directory, and runs it inside the CrossOver Steam
# bottle. difftest loads the real RenderD3D.dll (with DONT_RESOLVE_DLL_
# REFERENCES — no DllMain, no imports), maps the translated blob with fixups
# applied, then calls every leaf function both ways with identical randomized
# register/stack/memory contexts and compares eax/edx/st0 and touched memory.
#
# Prereqs: (cd runtime && make) and python3 tools/{translate,gen_manifest}.py
# have produced dist/RenderD3D.dll.x87 and dist/RenderD3D.dll.leaf.txt.
#
# Args: [maxfuncs] [rich]  — forwarded to difftest.exe (default: all funcs).
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
WINE="${WINE:-/Applications/CrossOver.app/Contents/SharedSupport/CrossOver/bin/wine}"
export CX_BOTTLE="${CX_BOTTLE:-Steam}"
export WINEDEBUG=-all

GAME="${H2SA_GAME_DIR:-/Users/rtry/Library/Application Support/CrossOver/Bottles/Steam/drive_c/Program Files (x86)/Steam/steamapps/common/Hitman 2 Silent Assassin}"
GAME_WIN='C:\Program Files (x86)\Steam\steamapps\common\Hitman 2 Silent Assassin'

[ -f "$ROOT/dist/RenderD3D.dll.x87" ] || { echo "run tools/translate.py first"; exit 1; }
[ -f "$ROOT/dist/RenderD3D.dll.leaf.txt" ] || { echo "run tools/gen_manifest.py first"; exit 1; }

i686-w64-mingw32-gcc -O1 -Wall -msse2 -static -static-libgcc \
    -o "$HERE/difftest.exe" "$HERE/difftest.c" "$HERE/probe.S" \
    "$ROOT/runtime/helpers.S"

cp "$HERE/difftest.exe" "$ROOT/dist/RenderD3D.dll.x87" \
   "$ROOT/dist/RenderD3D.dll.leaf.txt" "$GAME/"
trap 'rm -f "$GAME/difftest.exe" "$GAME/RenderD3D.dll.x87" "$GAME/RenderD3D.dll.leaf.txt"' EXIT

"$WINE" --bottle "$CX_BOTTLE" --workdir "$GAME_WIN" \
    --cx-app "$GAME_WIN\\difftest.exe" \
    RenderD3D.dll RenderD3D.dll.x87 RenderD3D.dll.leaf.txt "$@" 2>&1 \
    | grep -vE 'fixme|err:|warn:|wine:' || true

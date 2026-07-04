#!/bin/bash
# Install (or uninstall with -u) the H2SA plugins into the game.
# Works on mac (CrossOver bottle) and on Windows under Git Bash / MSYS2.
set -e
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*)
        DEFAULT_GAME="/c/Program Files (x86)/Steam/steamapps/common/Hitman 2 Silent Assassin";;
    *)
        DEFAULT_GAME="/Users/rtry/Library/Application Support/CrossOver/Bottles/Steam/drive_c/Program Files (x86)/Steam/steamapps/common/Hitman 2 Silent Assassin";;
esac
GAME="${H2SA_GAME_DIR:-$DEFAULT_GAME}"
HERE="$(cd "$(dirname "$0")" && pwd)"

if [ "$1" = "-u" ]; then
    rm -f "$GAME/d3d8.dll"
    rm -f "$GAME/scripts/H2SAAsiLoader.log"
    rm -f "$GAME/scripts/H2SAWidescreen.asi"
    rm -f "$GAME/scripts/H2SAWidescreen.log"
    # the plugin .ini is user config; left in place on purpose.
    # Hitman2.ini is not touched on uninstall; restore Hitman2.ini.bak by hand
    # if you want the original resolution back.
    echo "uninstalled (d3d8.dll + plugin removed; Hitman2.ini left as-is)"
    exit 0
fi

[ -f "$HERE/dist/d3d8.dll" ] || { echo "build first: (cd runtime && make)"; exit 1; }
[ -f "$HERE/dist/H2SAWidescreen.asi" ] || { echo "build first: (cd runtime && make)"; exit 1; }
[ -d "$GAME" ] || { echo "game dir not found: $GAME (set H2SA_GAME_DIR)"; exit 1; }

mkdir -p "$GAME/scripts"

# ASI loader: d3d8.dll proxy in the game root (also carries the D3D8 hooks)
cp "$HERE/dist/d3d8.dll" "$GAME/d3d8.dll"

# Widescreen + startup fix plugin
cp "$HERE/dist/H2SAWidescreen.asi" "$GAME/scripts/"
if [ ! -f "$GAME/scripts/H2SAWidescreen.ini" ]; then
    printf '[Widescreen]\nEnabled=1\nFullscreen=0\nBorderless=-1\nFOVCorrect=1\nFOVFactor=1.0\nCursorFix=-1\n' \
        > "$GAME/scripts/H2SAWidescreen.ini"
fi

# Give widescreen out of the box: if Hitman2.ini still has the placeholder
# 800x600 (4:3), bump it to a 16:9 resolution. Set H2SA_RESOLUTION=WxH to
# override, or to your display resolution for borderless-fullscreen. The
# original is backed up to Hitman2.ini.bak.
RES="${H2SA_RESOLUTION:-1920x1080}"
INI="$GAME/Hitman2.ini"
if [ -f "$INI" ] && grep -qE '^Resolution[[:space:]]+800x600' "$INI"; then
    cp "$INI" "$INI.bak"
    # portable in-place edit (BSD/GNU sed differ on -i)
    sed "s/^Resolution[[:space:]].*/Resolution ${RES}/" "$INI" > "$INI.tmp" \
        && mv "$INI.tmp" "$INI"
    echo "Hitman2.ini: Resolution 800x600 -> ${RES} (backup: Hitman2.ini.bak)"
fi

# Wine/CrossOver: the game-directory d3d8.dll only loads with the DLL
# override d3d8=native,builtin. Add it to the bottle if we can find CrossOver.
if [ "$(uname -s)" != "Darwin" ]; then
    :  # real Windows: app-dir DLLs win automatically, no override needed
elif [ -n "$WINE" ] || [ -x "/Applications/CrossOver.app/Contents/SharedSupport/CrossOver/bin/wine" ]; then
    CXWINE="${WINE:-/Applications/CrossOver.app/Contents/SharedSupport/CrossOver/bin/wine}"
    # bottle = the dir named ...Bottles/<name>/drive_c/... in the game path
    BOTTLE_NAME="$(printf '%s' "$GAME" | sed -n 's#.*/Bottles/\([^/]*\)/drive_c/.*#\1#p')"
    if [ -n "$BOTTLE_NAME" ]; then
        if CX_BOTTLE="$BOTTLE_NAME" WINEDEBUG=-all "$CXWINE" reg add \
             "HKCU\\Software\\Wine\\DllOverrides" /v d3d8 /d native,builtin /f \
             >/dev/null 2>&1; then
            echo "bottle '$BOTTLE_NAME': set DLL override d3d8=native,builtin"
        else
            echo "WARNING: could not set d3d8=native,builtin automatically."
            echo "  Add it in CrossOver: bottle '$BOTTLE_NAME' > Wine Configuration"
            echo "  > Libraries > new override for 'd3d8' (native, builtin)."
        fi
        # Borderless-fullscreen only hides the macOS menu bar and suppresses
        # the host cursor (near the Dock) when winemac captures the display,
        # which it does for a fullscreen-sized window while the app is active.
        # This bottle-wide Mac Driver option enables that capture without a
        # display mode switch (mode switches misrender under D3DMetal Retina).
        if CX_BOTTLE="$BOTTLE_NAME" WINEDEBUG=-all "$CXWINE" reg add \
             "HKCU\\Software\\Wine\\Mac Driver" /v CaptureDisplaysForFullscreen \
             /d y /f >/dev/null 2>&1; then
            echo "bottle '$BOTTLE_NAME': set Mac Driver CaptureDisplaysForFullscreen=y"
        else
            echo "WARNING: could not set CaptureDisplaysForFullscreen=y automatically."
            echo "  The menu bar / stray cursor fix needs it. Set it by hand:"
            echo "  CX_BOTTLE='$BOTTLE_NAME' wine reg add 'HKCU\\Software\\Wine\\Mac Driver'"
            echo "    /v CaptureDisplaysForFullscreen /d y /f"
        fi
    fi
fi

echo "installed to $GAME"
echo "  loader:     $GAME/d3d8.dll"
echo "  plugin:     $GAME/scripts/H2SAWidescreen.asi"
echo "  config:     $GAME/scripts/H2SAWidescreen.ini"
echo "logs after launch: $GAME/scripts/H2SAAsiLoader.log, H2SAWidescreen.log"
echo "Launch through Steam so the game's Steam check passes."

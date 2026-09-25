#!/bin/bash
# Install (or uninstall with -u) the H2SA plugins into the game.
# Works on mac (CrossOver bottle), Linux (Steam Proton prefix) and on Windows
# under Git Bash / MSYS2.
set -e
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*)
        DEFAULT_GAME="/c/Program Files (x86)/Steam/steamapps/common/Hitman 2 Silent Assassin";;
    Linux)
        DEFAULT_GAME="$HOME/.local/share/Steam/steamapps/common/Hitman 2 Silent Assassin";;
    *)
        DEFAULT_GAME="$HOME/Library/Application Support/CrossOver/Bottles/Steam/drive_c/Program Files (x86)/Steam/steamapps/common/Hitman 2 Silent Assassin";;
esac
GAME="${H2SA_GAME_DIR:-$DEFAULT_GAME}"
HERE="$(cd "$(dirname "$0")" && pwd)"

# KDE Plasma (X11): a KWin window rule that blocks compositing while the game
# window is open. KWin composites even a fullscreen Wine window, and on this
# stack (NVIDIA, 240Hz) that turns an even 60fps into visible judder; with
# compositing blocked the game flips straight to the display. KWin resumes
# compositing when the window closes. Matched on Steam's per-app window class.
KWIN_RULE=h2sa-hitman2-block-compositing
kwin_reconfigure() {
    for q in qdbus6 qdbus; do
        command -v "$q" >/dev/null 2>&1 &&
            "$q" org.kde.KWin /KWin reconfigure >/dev/null 2>&1 && return
    done
}
kwin_rule_install() {
    command -v kwriteconfig6 >/dev/null 2>&1 || return 0
    local rules
    rules="$(kreadconfig6 --file kwinrulesrc --group General --key rules)"
    case ",$rules," in
        *",$KWIN_RULE,"*) ;;
        *) rules="${rules:+$rules,}$KWIN_RULE" ;;
    esac
    local w="kwriteconfig6 --file kwinrulesrc --group $KWIN_RULE"
    $w --key Description "Hitman 2 (h2sa): block compositing"
    $w --key wmclass steam_app_6850
    $w --key wmclassmatch 1
    $w --key wmclasscomplete false
    $w --key blockcompositing true
    $w --key blockcompositingrule 2
    kwriteconfig6 --file kwinrulesrc --group General --key rules "$rules"
    kwriteconfig6 --file kwinrulesrc --group General --key count \
        "$(printf '%s' "$rules" | tr ',' '\n' | grep -c .)"
    kwin_reconfigure
    echo "KWin: window rule '$KWIN_RULE' blocks compositing while the game runs"
}
kwin_rule_remove() {
    command -v kwriteconfig6 >/dev/null 2>&1 || return 0
    local rules
    rules="$(kreadconfig6 --file kwinrulesrc --group General --key rules)"
    case ",$rules," in *",$KWIN_RULE,"*) ;; *) return 0 ;; esac
    rules="$(printf '%s' "$rules" | tr ',' '\n' | grep -vx "$KWIN_RULE" |
             paste -sd, -)"
    local k
    for k in Description wmclass wmclassmatch wmclasscomplete \
             blockcompositing blockcompositingrule; do
        kwriteconfig6 --file kwinrulesrc --group "$KWIN_RULE" --key "$k" --delete
    done
    kwriteconfig6 --file kwinrulesrc --group General --key rules "$rules"
    kwriteconfig6 --file kwinrulesrc --group General --key count \
        "$(printf '%s' "$rules" | tr ',' '\n' | grep -c .)"
    kwin_reconfigure
    echo "KWin: removed window rule '$KWIN_RULE'"
}

# Files from before the snake_case rename / core merge (widescreen + profiler
# are now one h2sa_core.asi). Removed on install and uninstall; the old inis
# are only removed after their settings were migrated into h2sa_core.ini.
remove_legacy() {
    rm -f "$GAME/scripts/H2SAAsiLoader.log"
    rm -f "$GAME/scripts/H2SAWidescreen.asi"
    rm -f "$GAME/scripts/H2SAWidescreen.ini"
    rm -f "$GAME/scripts/H2SAWidescreen.log"
    rm -f "$GAME/scripts/H2SAProfiler.asi"
    rm -f "$GAME/scripts/H2SAProfiler.ini"
    rm -f "$GAME/scripts/H2SAProfiler.log"
    rm -f "$GAME/scripts/H2SAReducedX87.asi"
    rm -f "$GAME/scripts/H2SAReducedX87-diag.asi"
    rm -f "$GAME/scripts/H2SAReducedX87.log"
    rm -rf "$GAME/scripts/H2SAReducedX87"
    rm -f "$GAME/scripts/H2SADump.asi"
    rm -f "$GAME/scripts/H2SADump.log"
}

if [ "$1" = "-u" ]; then
    rm -f "$GAME/d3d8.dll"
    rm -f "$GAME/scripts/h2sa_asi_loader.log"
    rm -f "$GAME/scripts/h2sa_core.asi"
    rm -f "$GAME/scripts/h2sa_core.log"
    rm -f "$GAME/scripts/h2sa_texpack.asi"
    rm -f "$GAME/scripts/h2sa_texpack.ini"
    rm -f "$GAME/scripts/h2sa_texpack.log"
    rm -f "$GAME/scripts/h2sa_reduced_x87.asi"
    rm -f "$GAME/scripts/h2sa_reduced_x87_diag.asi"
    rm -f "$GAME/scripts/h2sa_reduced_x87.log"
    rm -rf "$GAME/scripts/h2sa_reduced_x87"
    rm -f "$GAME/scripts/h2sa_dump.asi"
    rm -f "$GAME/scripts/h2sa_dump.log"
    remove_legacy
    [ "$(uname -s)" = "Linux" ] && kwin_rule_remove
    # the plugin .ini is user config; left in place on purpose.
    # Hitman2.ini is not touched on uninstall; restore Hitman2.ini.bak by hand
    # if you want the original resolution back.
    echo "uninstalled (d3d8.dll + plugins removed; h2sa_core.ini and Hitman2.ini left as-is)"
    exit 0
fi

[ -f "$HERE/dist/d3d8.dll" ] || { echo "build first: (cd runtime && make)"; exit 1; }
[ -f "$HERE/dist/h2sa_core.asi" ] || { echo "build first: (cd runtime && make)"; exit 1; }
[ -d "$GAME" ] || { echo "game dir not found: $GAME (set H2SA_GAME_DIR)"; exit 1; }

mkdir -p "$GAME/scripts"

# Migrate a pre-rename install: the old per-plugin configs become the
# [Widescreen] / [Profiler] sections of the merged h2sa_core.ini.
OLD_WS_INI="$GAME/scripts/H2SAWidescreen.ini"
OLD_PF_INI="$GAME/scripts/H2SAProfiler.ini"
if [ ! -f "$GAME/scripts/h2sa_core.ini" ] &&
   { [ -f "$OLD_WS_INI" ] || [ -f "$OLD_PF_INI" ]; }; then
    {
        [ -f "$OLD_WS_INI" ] && { cat "$OLD_WS_INI"; echo; }
        [ -f "$OLD_PF_INI" ] && cat "$OLD_PF_INI"
    } > "$GAME/scripts/h2sa_core.ini"
    echo "migrated old H2SAWidescreen.ini/H2SAProfiler.ini settings -> scripts/h2sa_core.ini"
fi
remove_legacy

# ASI loader: d3d8.dll proxy in the game root (also carries the D3D8 hooks)
cp "$HERE/dist/d3d8.dll" "$GAME/d3d8.dll"

# Core plugin: widescreen + startup fix + mouse fixes + frame cap, plus the
# performance profiler overlay (top-right; [Profiler] section of the ini).
cp "$HERE/dist/h2sa_core.asi" "$GAME/scripts/"
if [ ! -f "$GAME/scripts/h2sa_core.ini" ]; then
    printf '[Widescreen]\nEnabled=1\nFullscreen=0\nBorderless=-1\nPreserveAspect=1\nFOVCorrect=1\nFOVFactor=1.0\nCursorFix=0\nFpsCap=60\nVSync=-1\nMouseClipFix=-1\nMouseMotionFix=-1\nUIScale=-1\n\n[Camera]\nAutoZoomOut=1\n\n[DeathScreen]\nEnabled=1\nBackCol=101010\nHidePlane=1\n\n[Profiler]\nEnabled=1\nScale=1.0\nShowCPU=1\nOffsetX=8\nOffsetY=8\n' \
        > "$GAME/scripts/h2sa_core.ini"
elif ! grep -q '^[[:space:]]*UIScale' "$GAME/scripts/h2sa_core.ini"; then
    # migrate an existing config: enable the UI scale feature (UIScale=-1
    # renders at native pixels; Hitman2.ini Resolution becomes the UI size)
    awk '{ print } /^\[Widescreen\]/ { print "UIScale=-1" }' \
        "$GAME/scripts/h2sa_core.ini" > "$GAME/scripts/h2sa_core.ini.tmp" \
        && mv "$GAME/scripts/h2sa_core.ini.tmp" "$GAME/scripts/h2sa_core.ini"
    echo "h2sa_core.ini: added UIScale=-1 to [Widescreen]"
fi
# migrate an existing config: add the camera auto zoom-out section
if ! grep -q '^\[Camera\]' "$GAME/scripts/h2sa_core.ini"; then
    printf '\n[Camera]\nAutoZoomOut=1\n' >> "$GAME/scripts/h2sa_core.ini"
    echo "h2sa_core.ini: added [Camera] AutoZoomOut=1"
fi
# migrate an existing config: add the death-screen recolor section
if ! grep -q '^\[DeathScreen\]' "$GAME/scripts/h2sa_core.ini"; then
    printf '\n[DeathScreen]\nEnabled=1\nBackCol=101010\nHidePlane=1\n' \
        >> "$GAME/scripts/h2sa_core.ini"
    echo "h2sa_core.ini: added [DeathScreen] Enabled=1 BackCol=101010 HidePlane=1"
elif ! grep -q '^[[:space:]]*HidePlane' "$GAME/scripts/h2sa_core.ini"; then
    awk '{ print } /^\[DeathScreen\]/ { print "HidePlane=1" }' \
        "$GAME/scripts/h2sa_core.ini" > "$GAME/scripts/h2sa_core.ini.tmp" \
        && mv "$GAME/scripts/h2sa_core.ini.tmp" "$GAME/scripts/h2sa_core.ini"
    echo "h2sa_core.ini: added HidePlane=1 to [DeathScreen]"
fi
# drop the retired UISharpen key from earlier builds (harmless but stale)
if grep -q '^[[:space:]]*UISharpen' "$GAME/scripts/h2sa_core.ini" 2>/dev/null; then
    grep -v '^[[:space:]]*UISharpen' "$GAME/scripts/h2sa_core.ini" \
        > "$GAME/scripts/h2sa_core.ini.tmp" \
        && mv "$GAME/scripts/h2sa_core.ini.tmp" "$GAME/scripts/h2sa_core.ini"
fi

# The HD texture pack experiment (h2sa_texpack) was removed — clean up a
# previous install of it. Its data dir (dumps/pack) is user data and is
# left alone if present.
rm -f "$GAME/scripts/h2sa_texpack.asi" "$GAME/scripts/h2sa_texpack.ini" \
      "$GAME/scripts/h2sa_texpack.log"

# Reduced-precision x87 plugin: SSE2-translated RenderD3D.dll for CrossOver
# performance. The .asi loads any <module>.x87 blob from scripts/h2sa_reduced_x87/.
if [ -f "$HERE/dist/h2sa_reduced_x87.asi" ] && [ -f "$HERE/dist/RenderD3D.dll.x87" ]; then
    cp "$HERE/dist/h2sa_reduced_x87.asi" "$GAME/scripts/"
    # a stray diag build would also hook the game with helper-call counting
    # overhead; never leave one behind
    rm -f "$GAME/scripts/h2sa_reduced_x87_diag.asi"
    mkdir -p "$GAME/scripts/h2sa_reduced_x87"
    cp "$HERE/dist/RenderD3D.dll.x87" "$GAME/scripts/h2sa_reduced_x87/"
    msg="RenderD3D.dll.x87"
    # hitman2.exe.x87 (game-logic x87) is optional: generated by dumping the
    # packed exe once (see tools/undump.py / h2sa_dump.asi). Install if present.
    if [ -f "$HERE/dist/hitman2.exe.x87" ]; then
        cp "$HERE/dist/hitman2.exe.x87" "$GAME/scripts/h2sa_reduced_x87/"
        msg="$msg + hitman2.exe.x87"
    fi
    echo "x87 plugin: h2sa_reduced_x87.asi + $msg installed"
else
    echo "NOTE: x87 plugin not installed (build runtime + run tools/translate.py)"
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
if [ "$(uname -s)" = "Linux" ]; then
    # Steam Proton: the override goes into the game's own prefix, which lives
    # in the same Steam library as the game (steamapps/compatdata/6850). It is
    # set per-app (AppDefaults\hitman2.exe) with the prefix's own Proton wine;
    # config_info records which Proton build that is (its fonts dir, line 2).
    COMPAT="$(cd "$GAME/../.." && pwd)/compatdata/6850"
    PFX="$COMPAT/pfx"
    PROTON_FILES="$(sed -n '2s#/share/fonts/*$##p' "$COMPAT/config_info" 2>/dev/null)"
    KEY='HKCU\Software\Wine\AppDefaults\hitman2.exe\DllOverrides'
    if grep -q '^\[Software\\\\Wine\\\\AppDefaults\\\\hitman2.exe\\\\DllOverrides\]' "$PFX/user.reg" 2>/dev/null &&
       grep -A3 '^\[Software\\\\Wine\\\\AppDefaults\\\\hitman2.exe\\\\DllOverrides\]' "$PFX/user.reg" |
       grep -q '^"d3d8"="native,builtin"'; then
        echo "Proton prefix: d3d8=native,builtin already set for hitman2.exe"
    elif pgrep -x hitman2.exe >/dev/null 2>&1; then
        echo "WARNING: the game is running; quit it and re-run ./install.sh to"
        echo "  set the d3d8=native,builtin DLL override in its Proton prefix."
    elif [ -d "$PFX" ] && [ -x "$PROTON_FILES/bin/wine" ] &&
         WINEPREFIX="$PFX" WINEDEBUG=-all "$PROTON_FILES/bin/wine" reg add \
             "$KEY" /v d3d8 /d native,builtin /f >/dev/null 2>&1 &&
         WINEPREFIX="$PFX" "$PROTON_FILES/bin/wineserver" -w; then
        echo "Proton prefix: set DLL override d3d8=native,builtin for hitman2.exe"
    else
        echo "WARNING: could not set the d3d8 DLL override in the Proton prefix"
        echo "  ($PFX — launch the game once through Steam to create it, then"
        echo "  re-run ./install.sh). Or set the game's Steam launch options to:"
        echo "    WINEDLLOVERRIDES=\"d3d8=n,b\" %command%"
    fi
    # Proton runs d3d8 on wined3d (GL) by default; DXVK's d3d8 (Vulkan) is
    # opt-in per game, so it has to be a launch option.
    echo "Recommended Steam launch options (Properties > General):"
    echo "    PROTON_DXVK_D3D8=1 %command%"
    kwin_rule_install
elif [ "$(uname -s)" != "Darwin" ]; then
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
echo "  core:       $GAME/scripts/h2sa_core.asi (widescreen/startup/mouse/fps + profiler overlay)"
echo "  config:     $GAME/scripts/h2sa_core.ini"
echo "  x87 plugin: $GAME/scripts/h2sa_reduced_x87.asi (+ h2sa_reduced_x87/RenderD3D.dll.x87)"
echo "logs after launch: h2sa_asi_loader.log, h2sa_core.log, h2sa_reduced_x87.log"
echo "Launch through Steam so the game's Steam check passes."

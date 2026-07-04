# H2SA ASI Plugins

ASI plugins and a bundled ASI loader for **Hitman 2: Silent Assassin** on
modern systems, especially CrossOver/Rosetta 2 on Apple Silicon Macs. It is
the sibling of the [Hitman: Codename 47 build](../hc47-asi-plugins); the two
games are close relatives but Hitman 2 is a **Direct3D 8** title, which
changes how the loader attaches and how the widescreen/startup fix is done.

The repo builds two artifacts:

- `d3d8.dll` — a Direct3D 8 proxy that doubles as the ASI loader. It loads
  every `*.asi` from `scripts/`, forwards the real d3d8 exports to the
  system d3d8, and wraps the D3D8 COM interface so plugins can influence
  device creation and rendering without patching game code.
- `H2SAWidescreen.asi` — makes the game **start** under CrossOver (the
  stock exclusive-fullscreen device fails there) and renders it in correct
  widescreen at any resolution.

Everything hooks the fixed Direct3D 8 COM ABI, so there are **no
game-build byte offsets** to break — unlike the C47 build, which byte-patches
its DirectDraw/D3D7 renderer.

## Why d3d8.dll (and not dsound.dll)

`hitman2.exe` imports only `kernel32` and loads its subsystems at run time.
The renderer, `RenderD3D.dll`, is loaded when the game starts and imports
exactly one symbol — `Direct3DCreate8` — from `d3d8.dll`. A `d3d8.dll` proxy
in the game directory is therefore pulled in the instant the renderer
initializes, right before it creates its device: the ideal attach point. (The
C47 build proxies `dsound.dll` for the same reason; Hitman 2 never loads
dsound.)

## Build & Install

### mac

```sh
brew install mingw-w64
(cd runtime && make)          # writes dist/d3d8.dll and dist/H2SAWidescreen.asi
./install.sh                  # copies them into the game + configures the bottle
./install.sh -u               # uninstall (leaves your .ini and Hitman2.ini)
```

By default `install.sh` targets:

```text
mac:     /Users/rtry/Library/Application Support/CrossOver/Bottles/Steam/drive_c/Program Files (x86)/Steam/steamapps/common/Hitman 2 Silent Assassin
Windows: C:\Program Files (x86)\Steam\steamapps\common\Hitman 2 Silent Assassin
```

Override with `H2SA_GAME_DIR=/path/to/game ./install.sh`.

On mac the installer also configures the bottle:

- `d3d8=native,builtin` DLL override, so the game-directory proxy wins over
  the builtin d3d8.
- `Mac Driver\CaptureDisplaysForFullscreen=y`, so winemac captures the
  display for a borderless-fullscreen window — which is what hides the macOS
  menu bar and stops the host (Mac) cursor showing through near the Dock.
  See *Cursor & menu bar* below.
- if `Hitman2.ini` still has the placeholder `Resolution 800x600`, bumps it
  to `1920x1080` (backup: `Hitman2.ini.bak`; override with
  `H2SA_RESOLUTION=WxH`).

### Windows

The same sources build with MSYS2's 32-bit mingw-w64 gcc:

```powershell
winget install MSYS2.MSYS2
C:\msys64\usr\bin\pacman.exe -S --noconfirm mingw-w64-i686-gcc make
.\build.ps1 -Install
```

On real Windows the app-directory `d3d8.dll` is used automatically, so no
DLL override is needed.

### Launch through Steam

Hitman 2 checks for Steam at startup and exits immediately if it is not
present, so launch it the normal way (through the Steam client, appid
`6850`), not by running `hitman2.exe` directly.

## Plugin: Widescreen + startup fix (`H2SAWidescreen.asi`)

Two problems on a modern Mac, both handled at the D3D8 boundary:

### Startup — "Unable to create device"

In its default configuration the renderer asks D3D8 for an **exclusive
fullscreen** device at the configured resolution and colour depth. Under the
CrossOver D3DMetal/wined3d stack that `CreateDevice` fails
(`D3DERR_NOTAVAILABLE`), and `RenderD3D.dll` then shows its own fatal box —
*"Unable to create device. Try changing resolution or color depth"* /
*"This program requires that the display settings are set to high color or
true color."* This is the Direct3D 8 counterpart of the DirectDraw
exclusive-fullscreen problem the C47 build works around, and the fix is the
same in spirit: **don't use exclusive fullscreen.** The plugin rewrites the
presentation parameters to windowed (which uses the desktop format and always
creates) and, by default under Wine, strips the window to a borderless popup
at 0,0 so it still looks fullscreen.

*Verified:* with the plugin off, `CreateDevice` for the stock fullscreen
request returns `0x8876086a`; with it on, the same request returns `S_OK` and
the game runs.

### Widescreen — resolution and aspect

The resolution comes from the `Resolution WxH` line in `Hitman2.ini`, exactly
as the engine reads it. RenderD3D's fullscreen mode-selection mangles any
resolution not in its built-in ladder (a resolution like `1920x1080` reaches
`CreateDevice` as `1920x`*garbage*), so the plugin **pins the backbuffer to
the ini resolution** — the same value the engine lays its viewport, HUD and
mouse mapping out against — bypassing the broken mode-selection entirely.
Since the device is windowed there is no fullscreen mode to match, so any
resolution works.

The projection is authored for 4:3; at a wider backbuffer the image would be
Vert- (zoomed/squashed). The plugin intercepts
`IDirect3DDevice8::SetTransform(D3DTS_PROJECTION)` and applies the standard
**Hor+** correction — scale the matrix x term by `(4/3) / aspect` — widening
the horizontal field of view to the real aspect while keeping the vertical
FOV. Orthographic (2D/HUD) projections are detected (`_34==0`) and left
untouched, so the HUD is not distorted.

### Fullscreen vs. borderless

By default the game runs **borderless** — a frameless window filling the
screen, centred and aspect-preserving (letterboxed where the game aspect does
not match the display, so nothing is stretched). This is the robust default:
it always creates, survives alt-tab, and needs no display mode-switch.

**Exclusive fullscreen** (`Fullscreen=1`) is also supported, but there is a
catch worth understanding. Exclusive fullscreen only works at a resolution the
display actually **enumerates**, and under CrossOver that list is the Mac's
Retina-scaled **16:10** modes — `1280x800`, `1920x1200`, `2560x1600`, … —
*none* of the classic game resolutions (`800x600`, `1280x1024`, `1920x1080`)
are in it. That is precisely why the stock game's fullscreen device fails on a
Mac. So:

- `Fullscreen=1` **and** `Resolution` set to an enumerated mode
  (e.g. `1920x1200`) → real exclusive fullscreen. *Verified:* the game creates
  a fullscreen device and runs.
- `Fullscreen=1` at any other resolution → automatically falls back to
  borderless (logged), so it still starts.

To see your display's modes, run the game's own `config.exe`, or pick from the
16:10 list above. Borderless (the default) works at any resolution and, for a
16:9 pick like `1920x1080`, letterboxes cleanly on a 16:10 screen.

### Cursor & menu bar (CrossOver)

Two Mac-only annoyances are handled by the plugin so borderless looks and
behaves like true fullscreen:

- **Stray host cursor.** The game never sets an OS cursor (its menu pointer
  is a drawn sprite; gameplay is DirectInput), so CrossOver would otherwise
  show the macOS arrow on top of the game. The plugin keeps the Windows
  cursor NULL and hidden on the game's own threads (Wine tracks cursor state
  per thread queue, so this must run on the game threads, not a helper), and
  it forces winemac to (re)assert the hide by briefly cycling a transparent
  cursor. `CursorFix=-1` (auto) enables this under Wine only.
- **macOS menu bar at startup.** winemac hides the menu bar and captures the
  display only while the macOS *app* is active, and a Steam-launched process
  is not activated for free (so the menu bar lingered until you clicked in
  the window). The plugin reproduces that first click programmatically at
  startup, so the game comes up truly full-screen. This relies on the
  bottle's `CaptureDisplaysForFullscreen=y` (set by `install.sh`).

If you prefer to keep the macOS cursor/menu bar, set `CursorFix=0`.

Config: `scripts/H2SAWidescreen.ini`

```ini
[Widescreen]
Enabled=1
Fullscreen=0    ; 1 = exclusive fullscreen when Resolution is a real display
                ; mode, else borderless; 0 = always windowed/borderless
Borderless=-1   ; when not fullscreen: -1 auto = borderless fills desktop
                ; (all platforms), 0 plain window, 1 always borderless
FOVCorrect=1    ; Hor+ projection correction on/off
FOVFactor=1.0   ; extra horizontal FOV multiplier (>1 = wider)
CursorFix=-1    ; hide the stray macOS cursor + auto-activate at startup:
                ; -1 auto (on under Wine), 0 off, 1 on
FpsCap=60       ; frame-rate cap; 0 = uncapped (see below)
```

### Frame-rate cap

Hitman 2's engine advances its simulation from the measured frame time, so on
modern hardware — where the game can run at hundreds of FPS, especially
windowed with no fullscreen vsync — the physics, camera and scripted timing
misbehave (objects fling around, the camera oversteers, things run too fast).
The loader wraps `IDirect3DDevice8::Present` and the plugin paces each frame to
hold `FpsCap` (default **60**), which restores stock-like behaviour. Set
`FpsCap=0` to disable the cap. This applies on every platform.

Install output:

- Loader: `d3d8.dll` (game root)
- ASI: `scripts/H2SAWidescreen.asi`
- Config: `scripts/H2SAWidescreen.ini`
- Logs: `scripts/H2SAAsiLoader.log`, `scripts/H2SAWidescreen.log`

## ASI Loader (`d3d8.dll`)

A minimal d3d8.dll proxy (`runtime/asiloader.c`). On attach it loads every
`*.asi` from `scripts/` and logs to `scripts/H2SAAsiLoader.log`. It exports
all five real d3d8 entry points at their real ordinals (see
`runtime/d3d8.def`); four are forwarded to the system d3d8, resolved lazily,
and `Direct3DCreate8` is wrapped so the returned `IDirect3D8` — and the
`IDirect3DDevice8` it creates — have their `CreateDevice`, `Reset` and
`SetTransform` vtable slots redirected here. Plugins opt in via
`H2SA_RegisterD3D8Hooks` (see `runtime/h2sa_d3d8.h`); the vtable layout is a
fixed COM ABI, so no game offsets are involved.

Under Wine/CrossOver the bottle needs `d3d8=native,builtin` so the
game-directory proxy is chosen over the builtin d3d8 (which is what the proxy
forwards to). `install.sh` sets this automatically for CrossOver bottles.

Additional plugins can be dropped into `scripts/` and will be loaded; a new
plugin that needs the D3D8 hooks just registers through the same API.

## Verification

`tests/harness.c` reproduces what the renderer does — create a window,
`Direct3DCreate8`, then `CreateDevice` with exclusive-fullscreen parameters —
against the game-directory proxy, so the whole proxy → plugin → device path
can be exercised without launching the game:

```sh
i686-w64-mingw32-gcc -O2 -o tests/harness.exe tests/harness.c -ld3d8 -luser32 -lgdi32
# stage dist/d3d8.dll + dist/H2SAWidescreen.asi next to it under scripts/, then
# run harness.exe under the bottle's wine; expect "RESULT: OK" and a windowed device.
```

For the real game, launch through Steam and inspect
`scripts/H2SAAsiLoader.log` and `scripts/H2SAWidescreen.log`: they record the
requested vs. applied presentation parameters and the `CreateDevice` result.

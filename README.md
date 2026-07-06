# H2SA ASI Plugins

ASI plugins and a bundled ASI loader for **Hitman 2: Silent Assassin** on
modern systems, especially CrossOver/Rosetta 2 on Apple Silicon Macs. It is
the sibling of the [Hitman: Codename 47 build](../hc47-asi-plugins); the two
games are close relatives but Hitman 2 is a **Direct3D 8** title, which
changes how the loader attaches and how the widescreen/startup fix is done.

The repo builds four artifacts:

- `d3d8.dll` — a Direct3D 8 proxy that doubles as the ASI loader. It loads
  every `*.asi` from `scripts/`, forwards the real d3d8 exports to the
  system d3d8, and wraps the D3D8 COM interface so plugins can influence
  device creation and rendering without patching game code.
- `H2SAWidescreen.asi` — makes the game **start** under CrossOver (the
  stock exclusive-fullscreen device fails there) and renders it in correct
  widescreen at any resolution.
- `H2SAReducedX87.asi` — translates the renderer's x87 float code to SSE2
  at runtime for a large CrossOver/Rosetta 2 performance win (see below).
- `H2SAProfiler.asi` — a top-right on-screen overlay showing FPS, frame
  time, and an EIP-sampled CPU-time breakdown (see below).

The widescreen/startup fix hooks the fixed Direct3D 8 COM ABI, so it has
**no game-build byte offsets** to break — unlike the C47 build, which
byte-patches its DirectDraw/D3D7 renderer. The x87 plugin does patch
renderer code, but every site is byte-checked against the retail build
(PE timestamp + image size) and it declines to patch a module it does not
recognize.

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
pip3 install capstone pefile  # only needed for the x87 plugin's blob
python3 tools/translate.py    # writes dist/RenderD3D.dll.x87 (x87 plugin)
(cd runtime && make)          # writes dist/d3d8.dll + the .asi plugins
./install.sh                  # copies them into the game + configures the bottle
./install.sh -u               # uninstall (leaves your .ini and Hitman2.ini)
```

The `translate.py` step is only for `H2SAReducedX87.asi`; the loader and
widescreen plugin build without it. `install.sh` installs the x87 plugin
only if both `dist/H2SAReducedX87.asi` and `dist/RenderD3D.dll.x87` exist.

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

**Exclusive fullscreen** (`Fullscreen=1`) is **real-Windows only**:

- On **Windows**, `Fullscreen=1` with `Resolution` set to an enumerated mode
  (e.g. `1920x1200`) creates a true exclusive-fullscreen device; any other
  resolution falls back to borderless (logged), so it still starts.
- On **Wine/CrossOver (Mac)**, exclusive fullscreen is *broken* and the plugin
  never uses it: winemac drives the captured display at a **2x Retina backing
  scale**, so a `1920x1200` fullscreen surface becomes `3840x2400` pixels —
  larger than the physical panel — and the picture spills off-screen with the
  HUD clipped (this is the "fullscreen bigger than the screen" symptom). There
  is no per-app fix from inside the game, and the mode change also breaks
  alt-tab and the cursor. So on the Mac, `Fullscreen=1` is treated exactly like
  borderless-fullscreen (a log line explains this), which fills the screen
  correctly because macOS composites and scales the window properly. There is
  nothing to gain from exclusive fullscreen on this stack anyway — the present
  cost under D3DMetal is the same.

In short: on the Mac, use the default **borderless**. It fills the screen at
any resolution and, for a 16:9 pick like `1920x1080`, letterboxes cleanly on a
16:10 display.

### Cursor & menu bar (CrossOver)

`CursorFix` (optional, **off by default**) was an attempt to tame two Mac-only
annoyances: the stray macOS arrow drawn over the game, and the menu bar
lingering at startup. It hid the OS cursor on the game's own threads (cycling a
transparent cursor to make winemac re-assert the hide) and reproduced a
startup "click" so the macOS app activates (menu bar hides / display captures).

In practice, on the current borderless-fullscreen path it **does more harm than
good** and is disabled by default:

- The host cursor no longer leaks in borderless/fullscreen here, so the hiding
  is unnecessary.
- The per-frame cursor cycling and `SetCursorPos` edge-nudging made camera
  movement feel heavy — turning it off makes the game noticeably smoother.

The menu bar is handled by the bottle's `CaptureDisplaysForFullscreen=y` (set
by `install.sh`) once the window is active. If you specifically want the old
cursor-hide / startup-activation behaviour back, set `CursorFix=1` (or `-1`
for auto-on under Wine).

### Mouse-look under winemac (CrossOver)

Camera mouse-look needs **two** winemac fixes (both **auto-on under Wine**), the
same pair the sibling *Hitman: Contracts* port needed — H2 turns out to read its
mouse identically. Symptoms without them: the camera pins against an invisible
wall looking right/down, and slow turns stall until you shove the mouse harder.

**`MouseClipFix` — the edge wall.** `RenderD3D` captures the pointer for
camera-look by clipping the OS cursor to its full client rect (`GetClientRect` →
`ClientToScreen` → `ClipCursor`). Because the borderless window covers the whole
desktop, that clip equals the **entire display** — which winemac treats as *not
clipping*, leaving the Mac pointer in **absolute** mode where the reported
position clamps at the screen edges. Push right or down and the position pins at
the edge and the camera stops dead. winemac only switches the pointer to
**relative** mode (warps honoured, motion unbounded) when the clip is a *strict
subset* of the display, so the fix intercepts `ClipCursor` on `RenderD3D.dll` and
insets any full-screen clip by 2px. A `NULL` (release) clip — menus — is passed
straight through.

**`MouseMotionFix` — the slow-move stall.** The mission camera (and the menu
pointer) read the mouse through **DirectInput 8** in immediate mode
(`GetDeviceState` → `DIMOUSESTATE2`). DirectInput's *relative* axis is lossy on
winemac: it gets an integer per-event delta from the accelerated macOS `CGEvent`
stream, so a slow move rounds to **zero every event** and the camera stalls until
you cross a whole pixel in one event. The win32 `GetCursorPos` position does *not*
lose this — winemac accumulates the fractional motion into the absolute cursor
and rounds the **sum** — so the fix keeps the DirectInput device (buttons/firing
untouched) but, while camera-look is active, **replaces the device's lX/lY with
motion derived from `GetCursorPos`**, recentring each read. It pairs with
`MouseClipFix`: the clip inset puts winemac in relative mode so `GetCursorPos` is
smooth and the recentre warp is honoured.

Because the packed `hitman2.exe` creates its DirectInput device before the plugin
loads (and resolves DirectInput at runtime, so there is no static import to
hook), the fix reaches the device's methods via the **shared COM vtable**: it
creates its own throwaway SysMouse device — DirectInput objects of one class
share a single vtable — patches `GetDeviceState`/`GetDeviceData` there, and
releases it; the game's existing device then routes through the hooks. No game
byte-offsets. (H2's `p5dll.dll` HID import drives the exotic **P5 data glove**,
not the mouse — a red herring.) Set either to `0` to disable (both are off by
default on real Windows, which needs neither).

Config: `scripts/H2SAWidescreen.ini`

```ini
[Widescreen]
Enabled=1
Fullscreen=0      ; 1 = exclusive fullscreen (real Windows only, at an
                  ; enumerated mode); on Wine/Mac -> borderless-fullscreen
Borderless=-1     ; when not fullscreen: -1 auto = borderless fills desktop
                  ; (all platforms), 0 plain window, 1 always borderless
FOVCorrect=1      ; Hor+ projection correction on/off
FOVFactor=1.0     ; extra horizontal FOV multiplier (>1 = wider)
CursorFix=0       ; hide stray macOS cursor + startup activation: 0 off
                  ; (default), 1 on, -1 auto (on under Wine)
FpsCap=60         ; frame-rate cap; 0 = uncapped (see below)
MouseClipFix=-1   ; fix the mouse-look edge wall (inset the renderer's full-
                  ; window cursor clip): -1 auto (on under Wine), 0 off, 1 on
MouseMotionFix=-1 ; fix the slow-move stall (feed the DirectInput camera from
                  ; GetCursorPos): -1 auto (on under Wine), 0 off, 1 on
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

## Plugin: Reduced x87 (`H2SAReducedX87.asi`)

Improves performance under CrossOver/Rosetta 2, where x87 instructions are
emulated in software (80-bit) while SSE2 runs on hardware. An offline
translator rewrites provably-safe x87 functions into SSE2 double-precision
code; the runtime loader installs 5-byte entry hooks when the matching game
module loads. Same design as the [C47 build](../hc47-asi-plugins)'s
`HC47ReducedX87.asi` — the translator toolchain is shared (copied into
`tools/x87/`).

The target is **`RenderD3D.dll`**, the active renderer (`DrawDll` in
`Hitman2.ini`). Its software vertex-transform/lighting path is dense x87 —
matrix and vector math whose cost scales with on-screen geometry, which is
what drags the frame rate down in crowded areas. Coverage is high:

```text
translatable: 198 functions
translated 198 functions, 13267 x87 insns  (94% of the module's x87)
```

`hitman2.exe` is also translated, which matters because the profiler shows
crowded-area frames are dominated by game logic (AI/animation/physics), not
the renderer — and that logic is itself x87-heavy. The exe ships **packed**
(a single max-entropy `.text` unpacked by a stub at startup), so its real
code cannot be read from the on-disk image; it has to be dumped from memory
first:

```sh
(cd runtime && make dump)                 # builds dist/H2SADump.asi
cp dist/H2SADump.asi "$GAME/scripts/"     # launch once; writes hitman2_dump.bin
python3 tools/undump.py "$GAME/scripts/hitman2_dump.bin" -o staging/hitman2.exe
python3 tools/translate.py hitman2.exe --game staging   # dist/hitman2.exe.x87
# remove H2SADump.asi afterwards; install.sh picks up hitman2.exe.x87 if present
```

`H2SADump.asi` waits until the game is fully up (unpacker long finished),
then writes `hitman2.exe`'s mapped image; `tools/undump.py` rewrites the
section headers into a memory-aligned PE the analyzer can read. Coverage on
the unpacked image is ~1,040 functions / ~46,500 x87 instructions (about
63% of the module's x87).

Because a packed module's `.text` is garbage until the unpacker runs — and
the running exe is "loaded" from the very start — the loader must not hook
it too early. The `.x87` blob records each function's original entry bytes;
the loader polls and only installs hooks once (nearly) every entry matches,
so it hooks `hitman2.exe` after unpacking and `RenderD3D.dll` immediately.

Precision: translated code computes in 64-bit doubles instead of 80-bit
extended — more than enough for a renderer whose results land in 32-bit
float vertex buffers. Anything the translator cannot prove safe (unknown
x87 stack depth, `fprem`, jump tables, EFLAGS hazards, unbalanced
call/return state) stays original.

Generate the patch blob and build the plugin:

```sh
pip3 install capstone pefile
python3 tools/translate.py            # writes dist/RenderD3D.dll.x87
(cd runtime && make)                  # builds dist/H2SAReducedX87.asi
./install.sh                          # installs the .asi + the .x87 blob
```

The plugin is byte-checked: if `RenderD3D.dll`'s PE timestamp or image size
does not match the blob, it logs a mismatch and applies nothing. It also
coordinates with the widescreen plugin — the one function that mod patches
mid-body (the resolution-snap `je`) is on the translator's exclusion list,
though it is a no-FP function the translator would never select anyway.

Install output:

- ASI: `scripts/H2SAReducedX87.asi`
- Patch blob: `scripts/H2SAReducedX87/RenderD3D.dll.x87`
- Log: `scripts/H2SAReducedX87.log`

Expected log after launch:

```text
[renderd3d.dll] applied: 198/198 hooks, blob 199 KB at ... (delta ...)
```

A diagnostic build (`H2SAReducedX87-diag.asi`, also built by `make`) adds a
NaN/Inf tripwire at float-returning translated functions and logs the FPU
control word and helper-call counts — useful if a translated function is
suspected of misbehaving. If a specific function does, exclude it by RVA:
`python3 tools/translate.py --exclude 0x1234`.

## Plugin: Profiler (`H2SAProfiler.asi`)

A small performance overlay in the top-right corner, drawn each frame
through the loader's `on_frame` hook (a built-in 5x7 pixel font rendered as
pre-transformed colored triangles via `DrawPrimitiveUP`, wrapped in a state
block save/restore — the same technique as the
[h2-stats-overlay](../h2-stats-overlay) reference, so it needs no game
offsets). It shows:

```text
FPS 60
MS 16.6/22.0        ; average / peak frame time this window
X87 41%             ; CPU in the SSE2-translated RenderD3D blob
REND 18%            ; CPU in RenderD3D.dll NOT translated (leftover x87)
GAME 24%            ; CPU in hitman2.exe
REST 17%            ; everything else (D3D/Metal, wine, system)
```

The CPU split comes from a background EIP-sampling thread that reads each
game thread's instruction pointer and buckets it by module. It follows the
`H2SAReducedX87` entry hooks to find the translated blob, so blob samples
are attributed to **X87** and untranslated RenderD3D samples to **REND** —
making the x87 translation's effect directly visible (time that was
emulated x87 inside RenderD3D now shows up as native SSE2 under X87).

The sampler only suspends threads that consumed CPU since the last refresh.
Under CrossOver a thread parked in a D3DMetal/GPU syscall never reaches a
suspend safe-point, so blindly suspending every thread hangs; the idle
filter skips those blocked service threads (the same approach HC47's
`eipprof` uses on this bottle). With `ShowCPU=0` the sampler still runs but
only FPS/frame time is drawn.

Config: `scripts/H2SAProfiler.ini`

```ini
[Profiler]
Enabled=1      ; 0 hides the overlay (sampler still idle-cheap)
Scale=1.0      ; text size; 2.0 doubles it
ShowCPU=1      ; 0 = FPS + frame time only
OffsetX=8      ; inset from the right edge, in pixels
OffsetY=8      ; inset from the top edge, in pixels
```

The overlay is outlined text with no background panel, at the same glyph
size as the stats-overlay `SA`/`AZ` labels (`Scale=1.0`); lower `Scale` for
a smaller readout.

Install output:

- ASI: `scripts/H2SAProfiler.asi`
- Config: `scripts/H2SAProfiler.ini`
- Log: `scripts/H2SAProfiler.log` — a periodic text snapshot of the same
  stats (useful when a screenshot is not available), plus a `REST-top:`
  breakdown naming the modules the "REST" time actually lands in (e.g.
  `wined3d.dll`, `d3d8.dll`, `ntdll.dll`), which tells CPU draw-submission
  cost apart from GPU/idle waiting.

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

### Differential tester (x87 translation)

`tests/difftest.c` validates the SSE2 translation against the original x87
code. It loads the real `RenderD3D.dll` (with `DONT_RESOLVE_DLL_REFERENCES`
— no DllMain, no imports), maps the translated blob with fixups applied,
then calls every leaf function (from `dist/RenderD3D.dll.leaf.txt`, produced
by `tools/gen_manifest.py`) both ways with identical randomized register,
stack and memory contexts and compares `eax`/`edx`/`st0` and all touched
scratch memory (float-tolerant, since translated code is 64-bit double vs
80-bit extended).

```sh
python3 tools/translate.py            # dist/RenderD3D.dll.x87
python3 tools/gen_manifest.py         # dist/RenderD3D.dll.leaf.txt
tests/difftest.sh                     # build + run in the CrossOver bottle
tests/difftest.sh 20 rich             # first 20 funcs, pointer-rich contexts
```

A clean run reports `0 mismatched`. Functions that dereference pointers
supplied as random arguments fault identically in both versions and are
reported as `all-fault` (the tester cannot fabricate a valid context for
them) — that is not a translation defect. The known-good baseline is
`58 tested: 44 passed, 0 mismatched, 14 all-fault`.

The same tester validates `hitman2.exe.x87` against the **unpacked** image:
give `difftest.exe` the undumped exe (staged under a name that does not
collide with the running `hitman2.exe`, e.g. `h2unp.exe`) plus
`hitman2.exe.x87` and a leaf manifest generated with
`tools/gen_manifest.py hitman2.exe --game staging`. The strongest evidence,
though, is that the game runs stably with all ~1,040 translated game-logic
functions executing every frame.

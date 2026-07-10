/* H2SA Widescreen — native widescreen + a working startup path for
 * Hitman 2: Silent Assassin under Wine/CrossOver (32-bit ASI).
 *
 * The game is a Direct3D 8 title. Two things break it or spoil it on a
 * modern Mac:
 *
 *  1. Startup. In its default exclusive-fullscreen configuration the
 *     renderer asks D3D8 for a fullscreen device at the hitman2.ini
 *     resolution and colour depth. Under the CrossOver D3DMetal/wined3d
 *     stack that CreateDevice frequently fails, and RenderD3D.dll then
 *     pops its own fatal error — "Unable to create device. Try changing
 *     resolution or color depth" / "This program requires that the
 *     display settings are set to high color or true color." This is the
 *     Direct3D 8 analogue of the DirectDraw exclusive-fullscreen problem
 *     the Codename 47 build worked around, and the fix is the same in
 *     spirit: don't use exclusive fullscreen. We force the presentation
 *     parameters to windowed, which uses the desktop format and always
 *     creates, and (by default under Wine) size the window to a borderless
 *     popup covering the desktop so it still looks fullscreen.
 *
 *  2. Aspect ratio. The projection is authored for 4:3; at a wider
 *     backbuffer the image is Vert- (zoomed, squashed). We intercept
 *     IDirect3DDevice8::SetTransform(D3DTS_PROJECTION) and apply the
 *     standard Hor+ correction — widen the horizontal field of view to the
 *     real aspect while keeping the vertical FOV — by scaling the matrix's
 *     x term. Orthographic (2D/HUD) projections are left untouched.
 *
 * All of this happens at the fixed Direct3D 8 COM ABI via the loader's
 * H2SA_RegisterD3D8Hooks, so there are no game-build byte offsets to break.
 *
 * Exclusive fullscreen (Fullscreen=1) is REAL WINDOWS ONLY, and only at a
 * resolution the display actually enumerates. Under CrossOver on a Retina Mac
 * it is broken — winemac drives the captured display at a 2x backing scale so
 * the fullscreen surface renders larger than the panel and the picture spills
 * off-screen with the HUD clipped — so on Wine Fullscreen=1 is treated as
 * borderless-fullscreen, which fills the screen correctly. Borderless is the
 * right "fullscreen" on this stack; there is nothing to gain from the
 * exclusive path here (the present cost is the same under D3DMetal).
 *
 * Config: scripts/H2SAWidescreen.ini
 *   [Widescreen]
 *   Enabled=1
 *   Fullscreen=0    ; 1 = exclusive fullscreen (real Windows only, at an
 *                   ; enumerated mode); on Wine/Mac -> borderless-fullscreen
 *   Borderless=-1   ; when not fullscreen: -1 auto (borderless, filling the
 *                   ; desktop, under Wine), 0 plain window, 1 always borderless
 *   PreserveAspect=1 ; borderless: keep the Hitman2.ini resolution's aspect
 *                   ; ratio — scale to the largest centred fit and fill the
 *                   ; rest of the screen with black bars; 0 = stretch to fill
 *   FOVCorrect=1    ; Hor+ projection correction on/off
 *   FOVFactor=1.0   ; extra horizontal FOV multiplier (>1 = wider)
 *   CursorFix=0     ; hide the host (Mac) cursor over the game: 0 off
 *                   ; (default — see note), 1 on, -1 auto (on under Wine)
 *   MouseClipFix=-1 ; fix the mouse-look "edge wall" under winemac by insetting
 *                   ; the renderer's full-window cursor clip so winemac switches
 *                   ; to relative mouse motion: -1 auto (Wine), 0 off, 1 on
 *   MouseMotionFix=-1 ; fix the slow-move camera stall by feeding the camera's
 *                   ; DirectInput mouse motion from the absolute OS cursor
 *                   ; (GetCursorPos) instead of winemac's lossy relative axis:
 *                   ; -1 auto (Wine), 0 off, 1 on
 *   FpsCap=60       ; frame-rate cap (the engine is frame-time bound and
 *                   ; misbehaves uncapped); 0 = uncapped
 *   VSync=-1        ; -1 auto: in exclusive fullscreen, let the display pace
 *                   ; the cap (present every Nth vblank when refresh = N*cap),
 *                   ; else vsync every frame; 0 off (software cap only, may
 *                   ; tear); 1 force vsync-every-frame
 */
#include <d3d8.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <mmsystem.h>
#include <immintrin.h>          /* _mm_pause for the limiter spin */
#include "h2sa_d3d8.h"

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002  /* Win10 1803+ */
#endif

static FILE *g_log;
static char g_dir[MAX_PATH];
static int g_enabled = 1;
static int g_borderless = -1;      /* -1 auto (Wine), 0 never, 1 always */
static int g_fullscreen = 0;       /* 1 = exclusive fullscreen (see below) */
static int g_fovcorrect = 1;
static float g_fovfactor = 1.0f;
static int g_fpscap = 60;           /* frame-rate cap; 0 = uncapped */
static int g_vsync = -1;            /* -1 auto, 0 off, 1 force vsync-every-frame */
static DWORD g_present_intervals;   /* D3DCAPS8.PresentationIntervals, cached */
static D3DFORMAT g_desktop_fmt = D3DFMT_X8R8G8B8; /* desktop backbuffer format */
static int g_caps_done;             /* caps queried yet? */
static volatile int g_hw_paced;     /* display (vsync divisor) paces the rate */
static int g_borderless_active;    /* a fullscreen request was converted */
static int g_preserveaspect = 1;   /* borderless: keep the backbuffer aspect
                                    * (largest centred fit + black bars) instead
                                    * of stretching to the screen; 0 = stretch */
static int g_ini_w, g_ini_h;       /* Resolution WxH parsed from Hitman2.ini */
static int g_cursorfix = 0;        /* 0 off (default), 1 on, -1 auto (Wine).
                                    * Off by default: in borderless/fullscreen
                                    * on this Mac stack the host cursor no
                                    * longer leaks, and the per-frame hide/
                                    * re-prime + SetCursorPos churn made camera
                                    * movement feel heavy — net negative. */
static int g_mouseclipfix = -1;    /* inset the renderer's full-window cursor
                                    * clip so winemac uses relative mouse motion,
                                    * fixing the mouse-look edge wall: -1 auto
                                    * (Wine), 0 off, 1 on */
static int g_mousemotionfix = -1;  /* feed the DirectInput camera motion from the
                                    * absolute OS cursor (GetCursorPos) instead of
                                    * winemac's lossy relative axis, fixing the
                                    * slow-move stall: -1 auto (Wine), 0 off, 1 on */
static HWND g_game_hwnd;           /* the game window, learned at device init */
static volatile DWORD g_fg_deadline; /* startup-activation window still open */
static volatile DWORD g_next_kick;   /* earliest tick for the next kick */
static volatile int g_kicks_left;    /* remaining startup activation kicks */

static void logf_(const char *fmt, ...)
{
    if (!g_log) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

static void read_config(void)
{
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s\\H2SAWidescreen.ini", g_dir);
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[128];
    int b;
    float v;
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, " Enabled = %d", &b) == 1 ||
            sscanf(line, " Enabled=%d", &b) == 1)
            g_enabled = b;
        else if (sscanf(line, " Borderless = %d", &b) == 1 ||
                 sscanf(line, " Borderless=%d", &b) == 1)
            g_borderless = b < 0 ? -1 : (b != 0);
        else if (sscanf(line, " Fullscreen = %d", &b) == 1 ||
                 sscanf(line, " Fullscreen=%d", &b) == 1)
            g_fullscreen = b;
        else if (sscanf(line, " CursorFix = %d", &b) == 1 ||
                 sscanf(line, " CursorFix=%d", &b) == 1)
            g_cursorfix = b < 0 ? -1 : (b != 0);
        else if (sscanf(line, " MouseClipFix = %d", &b) == 1 ||
                 sscanf(line, " MouseClipFix=%d", &b) == 1)
            g_mouseclipfix = b < 0 ? -1 : (b != 0);
        else if (sscanf(line, " MouseMotionFix = %d", &b) == 1 ||
                 sscanf(line, " MouseMotionFix=%d", &b) == 1)
            g_mousemotionfix = b < 0 ? -1 : (b != 0);
        else if (sscanf(line, " PreserveAspect = %d", &b) == 1 ||
                 sscanf(line, " PreserveAspect=%d", &b) == 1)
            g_preserveaspect = (b != 0);
        else if (sscanf(line, " FOVCorrect = %d", &b) == 1 ||
                 sscanf(line, " FOVCorrect=%d", &b) == 1)
            g_fovcorrect = b;
        else if (sscanf(line, " FOVFactor = %f", &v) == 1 ||
                 sscanf(line, " FOVFactor=%f", &v) == 1)
            g_fovfactor = v;
        else if (sscanf(line, " FpsCap = %d", &b) == 1 ||
                 sscanf(line, " FpsCap=%d", &b) == 1)
            g_fpscap = b;
        else if (sscanf(line, " VSync = %d", &b) == 1 ||
                 sscanf(line, " VSync=%d", &b) == 1)
            g_vsync = b < 0 ? -1 : (b != 0);
    }
    fclose(f);
    if (!(g_fovfactor >= 0.5f && g_fovfactor <= 2.0f)) g_fovfactor = 1.0f;
    if (g_fpscap < 0 || g_fpscap > 1000) g_fpscap = 60;
}

/* Parse "Resolution WxH" from Hitman2.ini in the game root (the parent of
 * this scripts directory). This is the resolution the engine lays out
 * against internally; we hand the same value to the device so the two
 * always agree — and, crucially, it bypasses RenderD3D's fullscreen
 * mode-selection, which snaps unknown widths to a stale display mode (or
 * loads an uninitialised height: 1920x1080 comes through as 1920x<garbage>)
 * before it ever reaches CreateDevice. */
static void read_game_resolution(void)
{
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s\\..\\Hitman2.ini", g_dir);
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (_strnicmp(p, "Resolution", 10) != 0) continue;
        int w = 0, h = 0;
        if (sscanf(p + 10, " %dx%d", &w, &h) == 2 &&
            w >= 320 && h >= 200 && w <= 16384 && h <= 16384) {
            g_ini_w = w; g_ini_h = h;
        }
        break;
    }
    fclose(f);
}

static int is_wine(void)
{
    return GetProcAddress(GetModuleHandleA("ntdll.dll"),
                          "wine_get_version") != NULL;
}

/* The renderer's mode-set path snaps the requested resolution to a fixed 4:3
 * ladder (512x384 .. 1600x1200). For a width past the top of the ladder
 * (e.g. 1920) it falls through and loads the HEIGHT from an uninitialised
 * stack slot — garbage — which then drives the viewport, the projection and
 * the whole 2D layout, so the picture collapses and the cursor lands
 * off-screen. The ladder is guarded by `je <skip>` on a flag; making that
 * jump unconditional bypasses the ladder entirely, so the resolution the
 * engine already parsed (Hitman2.ini Width x Height) passes straight through.
 * This is the source-level fix nemesis2000's widescreen patch uses, and the
 * same idea as the Codename 47 build. Found by pattern scan (identical bytes
 * in RenderD3D.dll and RenderOpenGL.dll), so no fixed offsets. */
static const uint8_t SNAP_SIG[16] = {
    0x0f, 0x84, 0x05, 0x01, 0x00, 0x00, 0x8b, 0x42,
    0x64, 0x3d, 0x00, 0x02, 0x00, 0x00, 0x7d, 0x0f
};
/* `je rel32` (target = site+6+0x105) -> `jmp rel32; nop` to the same target
 * (rel32 = 0x105 + 1 = 0x106 from the 5-byte jmp). */
static const uint8_t SNAP_PATCH[6] = { 0xe9, 0x06, 0x01, 0x00, 0x00, 0x90 };
static int g_snap_done;

static uint8_t *find_sig(uint8_t *base, const uint8_t *sig, size_t n)
{
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    IMAGE_NT_HEADERS32 *nt = (IMAGE_NT_HEADERS32 *)(base + dos->e_lfanew);
    size_t span = nt->OptionalHeader.SizeOfImage;
    for (size_t i = 0; i + n <= span; i++)
        if (memcmp(base + i, sig, n) == 0)
            return base + i;
    return NULL;
}

static int patch_one_renderer(const char *name)
{
    HMODULE m = GetModuleHandleA(name);
    if (!m) return 0;
    uint8_t *site = find_sig((uint8_t *)m, SNAP_SIG, sizeof(SNAP_SIG));
    if (!site) {
        logf_("%s: resolution-snap signature not found — skipped", name);
        return 1;   /* loaded but no match: give up on this one */
    }
    DWORD old;
    if (VirtualProtect(site, sizeof(SNAP_PATCH), PAGE_EXECUTE_READWRITE, &old)) {
        memcpy(site, SNAP_PATCH, sizeof(SNAP_PATCH));
        VirtualProtect(site, sizeof(SNAP_PATCH), old, &old);
        FlushInstructionCache(GetCurrentProcess(), site, sizeof(SNAP_PATCH));
        logf_("%s: resolution snap disabled at +0x%tx — Hitman2.ini "
              "resolution passes through", name, site - (uint8_t *)m);
    }
    return 1;
}

/* Returns 1 once whichever renderer is present has been handled. */
static int patch_renderer_snap(void)
{
    int did = 0;
    did |= patch_one_renderer("RenderD3D.dll");
    did |= patch_one_renderer("RenderOpenGL.dll");
    return did;
}

static DWORD WINAPI snap_watch(LPVOID arg)
{
    (void)arg;
    for (int i = 0; i < 800 && !g_snap_done; i++) {
        if (patch_renderer_snap()) { g_snap_done = 1; break; }
        Sleep(5);
    }
    return 0;
}

static int cursorfix_wanted(void)
{
    return g_cursorfix == 1 || (g_cursorfix == -1 && is_wine());
}

static int mouseclipfix_wanted(void)
{
    return g_mouseclipfix == 1 || (g_mouseclipfix == -1 && is_wine());
}

static int mousemotionfix_wanted(void)
{
    return g_mousemotionfix == 1 || (g_mousemotionfix == -1 && is_wine());
}

/* Redirect one IAT entry (module imports dll!fn) to hook; saves the original
 * through *orig. Used to notice when the game recenters the pointer for
 * mouse-look. */
static int iat_hook(HMODULE mod, const char *dll, const char *fn,
                    void *hook, void **orig)
{
    uint8_t *base = (uint8_t *)mod;
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    IMAGE_NT_HEADERS32 *nt = (IMAGE_NT_HEADERS32 *)(base + dos->e_lfanew);
    IMAGE_DATA_DIRECTORY dir =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return 0;
    IMAGE_IMPORT_DESCRIPTOR *imp =
        (IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress);
    DWORD span = nt->OptionalHeader.SizeOfImage;
    for (; imp->Name; imp++) {
        if (imp->Name >= span) continue;
        if (_stricmp((const char *)(base + imp->Name), dll) != 0) continue;
        /* OriginalFirstThunk can be 0 (old linkers / bound imports); the
         * name table is gone then, so skip rather than walk garbage. */
        if (!imp->OriginalFirstThunk || !imp->FirstThunk ||
            imp->OriginalFirstThunk >= span || imp->FirstThunk >= span)
            continue;
        IMAGE_THUNK_DATA32 *oft =
            (IMAGE_THUNK_DATA32 *)(base + imp->OriginalFirstThunk);
        IMAGE_THUNK_DATA32 *ft =
            (IMAGE_THUNK_DATA32 *)(base + imp->FirstThunk);
        for (; ft->u1.Function; oft++, ft++) {
            if (oft->u1.Ordinal & IMAGE_ORDINAL_FLAG32) continue;
            if (oft->u1.AddressOfData >= span) continue;
            IMAGE_IMPORT_BY_NAME *ibn =
                (IMAGE_IMPORT_BY_NAME *)(base + oft->u1.AddressOfData);
            if (strcmp((char *)ibn->Name, fn) != 0) continue;
            DWORD old;
            if (!VirtualProtect(&ft->u1.Function, 4, PAGE_READWRITE, &old))
                return 0;
            if (orig) *orig = (void *)(uintptr_t)ft->u1.Function;
            ft->u1.Function = (DWORD)(uintptr_t)hook;
            VirtualProtect(&ft->u1.Function, 4, old, &old);
            return 1;
        }
    }
    return 0;
}

static BOOL (WINAPI *g_real_setcursorpos)(int, int);

/* Double-cursor fix. The game never sets an OS cursor of its own — its menu
 * pointer is a drawn sprite and gameplay uses DirectInput — so whatever
 * Windows cursor state the process ends up with is an accident, and under
 * CrossOver the macOS arrow shows through on top of the game.
 *
 * Two Wine facts drive the design (verified in wineserver/queue.c and
 * winemac.drv):
 *
 *  1. Cursor state is per thread queue. ShowCursor's count lives on the
 *     calling thread's queue; the effective count for a window is the sum
 *     over the queues attached to its thread input, and assign_thread_input
 *     subtracts a queue's contribution back out when it detaches. So hiding
 *     from a helper thread (AttachThreadInput -> ShowCursor(FALSE) ->
 *     detach) undoes itself; SetCursor/ShowCursor must run ON the threads
 *     that own the game's windows.
 *
 *  2. winemac only stays hidden for a NULL effective cursor (count < 0 or
 *     no cursor set). Any REAL cursor handle reaching the driver — even a
 *     fully transparent one — makes it unhide and clears its
 *     clientWantsCursorHidden flag, after which its "pointer not over a
 *     Wine window" fallback paints the macOS ARROW (seen when the pointer
 *     pins at the bottom of the screen, and during transient moments where
 *     the window drops off the screen list). An earlier version of this fix
 *     asserted a transparent cursor; that transparent-but-real cursor was
 *     itself what armed the arrow. So: never present ANY real cursor. Keep
 *     the current cursor NULL, the class cursor NULL (so DefWindowProc's
 *     WM_SETCURSOR can't set one), and the show count negative.
 *
 * The assertion runs from every game-owned thread we can reach:
 *   - the SetCursorPos IAT hook (mouse-look recentering);
 *   - fix_present (CreateDevice/Reset, covers mission loads);
 *   - WH_GETMESSAGE hooks on EVERY window-owning thread of the process
 *     (menus and DirectInput may pump input on other threads than the
 *     device window's). */
static HHOOK g_msg_hook;

/* Hide the cursor for the CALLING thread's input queue: current cursor to
 * NULL, show count driven negative. Only effective on a thread that owns
 * game windows — every call site is one. Cheap once hidden. Logs state
 * transitions (count sign / cursor handle) so a run shows who flips what. */
static void assert_cursor_hidden(const char *site)
{
    if (!g_enabled || !cursorfix_wanted()) return;

    HCURSOR cur = GetCursor();
    CURSORINFO ci;
    ci.cbSize = sizeof(ci);
    int showing = GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING);

    /* diagnostic: log whenever this thread's view of the state changes */
    static volatile LONG last_state = -1;
    LONG state = (showing ? 1 : 0) | (cur ? 2 : 0);
    if (state != last_state) {
        last_state = state;
        logf_("cursor state change at %s: showing=%d cursor=%p tid=%lu "
              "(tick %lu)", site, showing, (void *)cur,
              (unsigned long)GetCurrentThreadId(),
              (unsigned long)GetTickCount());
    }

    /* keep the cursor identity NULL; the show COUNT is managed by the
     * re-prime (transitions only reach the Mac driver at count >= 0, so
     * driving it negative here would hide them) */
    if (cur)
        SetCursor(NULL);
}

/* Keeping macOS's cursor-hide engaged needs more than correct win32 state:
 *
 *  - win32u only notifies the display driver when the EFFECTIVE cursor
 *    CHANGES, so a cursor that is NULL from before the window exists never
 *    produces a hide event at all;
 *  - macOS force-reveals the cursor at the WindowServer level when the
 *    pointer enters the Dock strip, and that reveal DESYNCS winemac's
 *    bookkeeping: its internal cursorHidden flag still says hidden, so it
 *    never re-issues [NSCursor hide] and the arrow sticks forever.
 *
 * Both are cured by the same maneuver: set a REAL cursor, then take it
 * away. The real cursor makes winemac run its unhide path (rebalancing the
 * stale flag), and the NULL transition then issues a FRESH [NSCursor hide].
 * Using a fully transparent cursor as the "real" one makes the whole
 * resync invisible. Re-run it every 250 ms on a game thread while the game
 * is foreground, so a WindowServer reveal is undone within a beat. */
static volatile LONG g_prime_needed = 1;
static HCURSOR g_blank_cursor;

static HCURSOR blank_cursor(void)
{
    if (!g_blank_cursor) {
        BYTE andmask[128], xormask[128];
        memset(andmask, 0xFF, sizeof(andmask));   /* AND=1,XOR=0: transparent */
        memset(xormask, 0x00, sizeof(xormask));
        HCURSOR c = CreateCursor(GetModuleHandleA(NULL), 0, 0, 32, 32,
                                 andmask, xormask);
        if (c && InterlockedCompareExchangePointer(
                     (PVOID volatile *)&g_blank_cursor, (PVOID)c, NULL))
            DestroyCursor(c);          /* lost the race; keep the winner */
    }
    return g_blank_cursor;
}

static void prime_driver_hide(void)
{
    if (!cursorfix_wanted()) return;
    HWND w = g_game_hwnd;
    if (!w || GetForegroundWindow() != w) return;
    static DWORD last_prime;
    DWORD now = GetTickCount();
    if (!InterlockedExchange(&g_prime_needed, 0) &&
        now - last_prime < 250)
        return;
    last_prime = now;
    int c = ShowCursor(TRUE);                     /* transitions only reach */
    for (int k = 0; c < 0 && k < 8; k++)          /* the driver at count>=0 */
        c = ShowCursor(TRUE);
    HCURSOR blank = blank_cursor();
    SetCursor(blank ? blank : LoadCursorA(NULL, (LPCSTR)IDC_ARROW));
    SetCursor(NULL);
    for (int k = 0; c > 0 && k < 8; k++)
        c = ShowCursor(FALSE);
    static LONG logged;
    if (!logged) {
        logged = 1;
        logf_("driver hide re-prime active (blank->NULL every 250ms, tid %lu)",
              (unsigned long)GetCurrentThreadId());
    }
}

static BOOL WINAPI my_setcursorpos(int x, int y)
{
    /* A recenter means mouse-look is active, and we are on the game thread —
     * the one place where hiding the cursor actually sticks. */
    assert_cursor_hidden("SetCursorPos recenter");
    return g_real_setcursorpos ? g_real_setcursorpos(x, y) : TRUE;
}

/* Diagnostic pass-through hooks: the game is not expected to touch the OS
 * cursor, so log any ShowCursor/SetCursor it does make — those are exactly
 * the calls that could re-arm the host arrow. */
static int (WINAPI *g_real_showcursor)(BOOL);
static HCURSOR (WINAPI *g_real_setcursor)(HCURSOR);

static int WINAPI my_showcursor(BOOL show)
{
    int n = g_real_showcursor ? g_real_showcursor(show) : (show ? 0 : -1);
    static LONG logs;
    if (logs < 40) {
        InterlockedIncrement(&logs);
        logf_("game ShowCursor(%d) -> %d (tid %lu)", show, n,
              (unsigned long)GetCurrentThreadId());
    }
    return n;
}

static HCURSOR WINAPI my_setcursor(HCURSOR c)
{
    static LONG logs;
    if (logs < 40) {
        InterlockedIncrement(&logs);
        logf_("game SetCursor(%p) (tid %lu)", (void *)c,
              (unsigned long)GetCurrentThreadId());
    }
    /* the game never needs an OS cursor; keep it NULL */
    return g_real_setcursor ? g_real_setcursor(NULL) : NULL;
}

static void hook_game_imports(void)
{
    static int done_pos, done_diag;
    if (!cursorfix_wanted() || (done_pos && done_diag)) return;
    HMODULE r = GetModuleHandleA("RenderD3D.dll");
    if (!r) r = GetModuleHandleA("RenderOpenGL.dll");
    if (!done_pos && r &&
        iat_hook(r, "user32.dll", "SetCursorPos", (void *)my_setcursorpos,
                 (void **)&g_real_setcursorpos)) {
        done_pos = 1;
        logf_("SetCursorPos hooked — cursor is hidden on the game thread "
              "during mouse-look");
    }
    if (!done_diag) {
        HMODULE mods[3];
        mods[0] = GetModuleHandleA(NULL);
        mods[1] = r;
        mods[2] = GetModuleHandleA("SDL_Engine.dll");
        int n = 0;
        for (int i = 0; i < 3; i++) {
            if (!mods[i]) continue;
            n += iat_hook(mods[i], "user32.dll", "ShowCursor",
                          (void *)my_showcursor, (void **)&g_real_showcursor);
            n += iat_hook(mods[i], "user32.dll", "SetCursor",
                          (void *)my_setcursor, (void **)&g_real_setcursor);
        }
        if (n) {
            done_diag = 1;
            logf_("ShowCursor/SetCursor pass-through hooks installed (%d)", n);
        } else if (r) {
            done_diag = 1;   /* renderer is up and nothing imports them */
            logf_("no ShowCursor/SetCursor imports found to hook");
        }
    }
}

/* Fix the mouse-look "edge wall" under winemac.
 *
 * RenderD3D captures the pointer for camera-look exactly the way the Contracts
 * engine does: GetClientRect -> ClientToScreen -> ClipCursor(full client rect),
 * then reads the cursor position (delivered as WM_MOUSEMOVE client coords) each
 * frame, applies the delta from the window centre to the camera, and recentres
 * with SetCursorPos. Because the borderless game window covers the whole
 * desktop, that full-client clip equals the ENTIRE display — which winemac
 * treats as "not clipping", leaving the Mac pointer in ABSOLUTE mode where the
 * reported position clamps at the screen edges: push right or down and the
 * position pins at the far edge, the per-frame delta collapses to zero, and the
 * recentre can't rescue it — the camera stops dead against an invisible wall
 * until you drag back (shoving harder past the edge is what eventually registers
 * motion). winemac only switches the pointer to relative / associated-off mode —
 * where warps are honoured and motion is unbounded — when the clip rect is a
 * STRICT SUBSET of the display.
 *
 * Fix: intercept ClipCursor and, whenever the game clips to (essentially) the
 * whole display, inset the rect a couple of pixels so it is a strict subset.
 * winemac then engages relative mode and the wall is gone, with the confined
 * area shrunk by an imperceptible 2px. A NULL (release) clip — menus, where the
 * pointer roams freely — is passed straight through untouched.
 *
 * Unlike Contracts (which read the mouse through a DirectInput RELATIVE axis
 * that winemac quantises per event, losing slow motion, and so needed a second
 * fix feeding the camera from GetCursorPos) H2 reads its mouse from the ABSOLUTE
 * OS cursor via WM_MOUSEMOVE — the same smoothly-accumulating source winemac
 * keeps sub-pixel-accurate — so once relative mode engages, slow moves are
 * already smooth and no separate motion-source fix is needed. There is no
 * DirectInput mouse and no HID mouse read here (p5dll's HID import drives the
 * exotic P5 data glove, not the mouse), so ClipCursor is the whole fix.
 *
 * The hook goes on RenderD3D.dll (the module that owns the window and clips the
 * cursor), not the packed hitman2.exe launcher. Wine-only by default; real
 * Windows needs no change and is left alone. */
static BOOL (WINAPI *g_real_clipcursor)(const RECT *);

/* Camera-look state, learned from the engine's own ClipCursor calls: the
 * renderer clips the cursor to its window while camera-look is active and
 * releases it (ClipCursor(NULL)) in menus. The DirectInput motion fix below
 * only rewrites motion while look is active, so menus keep their normal free
 * pointer. Tracked regardless of whether the clip-inset itself is wanted, so
 * the motion fix works even with MouseClipFix off. */
static volatile LONG g_look_active;
static volatile LONG g_look_primed;    /* reset on each look-enable to swallow the
                                        * first frame's stale delta (no jolt) */

static BOOL WINAPI my_clipcursor(const RECT *rc)
{
    LONG active = rc ? 1 : 0;
    if (active != g_look_active) {
        g_look_active = active;
        if (active) g_look_primed = 0;
    }

    if (!g_enabled || !mouseclipfix_wanted() || !rc)
        return g_real_clipcursor ? g_real_clipcursor(rc) : TRUE;

    /* The game window is a borderless popup at (0,0) covering the desktop and
     * clips to its own client rect, so the "whole display" it can reach is
     * 0,0 .. screen w,h. */
    int dw = GetSystemMetrics(SM_CXSCREEN);
    int dh = GetSystemMetrics(SM_CYSCREEN);
    if (dw < 40 || dh < 40)
        return g_real_clipcursor ? g_real_clipcursor(rc) : TRUE;

    /* Inset every edge that reaches (or passes) the display bound, so the clip
     * can never equal the full desktop — which winemac reads as "unclipped". */
    const int INSET = 2;
    RECT out = *rc;
    if (out.left   <= 0)   out.left   = INSET;
    if (out.top    <= 0)   out.top    = INSET;
    if (out.right  >= dw)  out.right  = dw - INSET;
    if (out.bottom >= dh)  out.bottom = dh - INSET;
    if (out.right  <= out.left)  out.right  = out.left + 1;
    if (out.bottom <= out.top)   out.bottom = out.top + 1;

    static LONG logged;
    if (!logged) {
        logged = 1;
        logf_("ClipCursor inset for relative mouse: (%ld,%ld,%ld,%ld) -> "
              "(%ld,%ld,%ld,%ld) on %dx%d — winemac now delivers relative "
              "motion, fixing the mouse-look edge wall",
              rc->left, rc->top, rc->right, rc->bottom,
              out.left, out.top, out.right, out.bottom, dw, dh);
    }
    return g_real_clipcursor ? g_real_clipcursor(&out) : TRUE;
}

/* Install the ClipCursor IAT hook on the renderer. Independent of CursorFix:
 * the edge-wall fix is wanted even with the host-cursor hider off. RenderD3D is
 * already mapped by the time our watcher runs, so this resolves immediately;
 * cursor_watch also retries it as a backstop until it lands. */
static void hook_clipcursor(void)
{
    static int done;
    if (done || !g_enabled || !mouseclipfix_wanted()) return;
    HMODULE r = GetModuleHandleA("RenderD3D.dll");
    if (!r) r = GetModuleHandleA("RenderOpenGL.dll");
    if (r && iat_hook(r, "user32.dll", "ClipCursor",
                      (void *)my_clipcursor, (void **)&g_real_clipcursor)) {
        done = 1;
        logf_("ClipCursor hooked on the renderer — full-window cursor clips are "
              "inset so winemac uses relative mouse motion (fixes the mouse-look "
              "edge wall)");
    }
}

/* ------------------------------------------------------------------------
 * DirectInput camera-motion fix (the slow-move stall).
 *
 * H2's mission camera (and its menu pointer) reads the mouse through
 * DirectInput8 in immediate mode — GetDeviceState into a DIMOUSESTATE2, whose
 * first two LONGs are the relative lX/lY (confirmed empirically; the RenderD3D
 * WM_MOUSEMOVE path is a menu/one-shot path, not the per-frame camera).
 * DirectInput's RELATIVE axis is lossy on winemac: it hands DI an integer
 * per-event delta from the accelerated macOS CGEvent stream, and a SLOW move
 * rounds to zero every event, so the camera stalls until you move fast enough
 * to cross a whole pixel in one event (the "invisible border" that frees when
 * you shove). The win32 GetCursorPos position does NOT lose this — winemac
 * accumulates the fractional motion into the absolute cursor and rounds the
 * SUM, so it creeps smoothly at any speed.
 *
 * This keeps the DirectInput device (so buttons keep working) but REPLACES the
 * device's X/Y axis data with motion derived from GetCursorPos while camera-look
 * is active: each read we sample the cursor's delta from the window centre and
 * recentre. The immediate path (GetDeviceState) is what the game uses; the
 * buffered path (GetDeviceData) is handled too for robustness. Menus with the
 * pointer released (look inactive) are left alone. Wine-only by default. It
 * pairs with MouseClipFix: the clip inset puts winemac in relative mode (so
 * GetCursorPos is smooth and the recentre warp is honoured), and this feeds the
 * camera from that smooth source.
 *
 * Installation patches the SysMouse device's COM vtable (GetDeviceState +
 * GetDeviceData). Because DirectInput device objects of one class share a
 * single vtable, we reach it via our OWN throwaway SysMouse device rather than
 * catching the game's CreateDevice — the packed hitman2.exe creates its device
 * before we load and resolves DirectInput at runtime (no static IAT to hook as
 * in Contracts), so a creation hook would be too late. No game byte-offsets. */

/* Minimal DirectInput ABI (avoid pulling dinput.h). */
#ifndef DIGDD_PEEK
#define DIGDD_PEEK 0x00000001
#endif
#define H2_DIMOFS_X 0
#define H2_DIMOFS_Y 4
/* GUID_SysMouse = {6F1D2B60-D5A0-11CF-BFC7-444553540000} */
static const GUID H2_GUID_SysMouse =
    { 0x6F1D2B60, 0xD5A0, 0x11CF, { 0xBF,0xC7,0x44,0x45,0x53,0x54,0x00,0x00 } };

typedef HRESULT (WINAPI *h2_di8create_t)(HINSTANCE, DWORD, REFIID, void **, void *);
typedef HRESULT (WINAPI *h2_createdevice_t)(void *, REFGUID, void **, void *);
typedef HRESULT (WINAPI *h2_getdevicedata_t)(void *, DWORD, void *, DWORD *, DWORD);
typedef HRESULT (WINAPI *h2_getdevicestate_t)(void *, DWORD, void *);

static h2_getdevicedata_t  g_real_getdevicedata;
static h2_getdevicestate_t g_real_getdevicestate;
static void *g_di_mouse;                          /* the SysMouse device object */
static BOOL (WINAPI *g_setcursorpos_p)(int, int); /* real SetCursorPos, unhooked */

/* Client-area centre of the game window, in screen coordinates — the point we
 * recentre to each read so the cursor delta stays bounded. */
static int client_center_screen(HWND w, POINT *out)
{
    RECT rc;
    if (!w || !IsWindow(w) || !GetClientRect(w, &rc)) return 0;
    out->x = rc.right / 2;
    out->y = rc.bottom / 2;
    return ClientToScreen(w, out);
}

/* This frame's camera motion from the OS cursor (smooth at any speed), then
 * recentre. Returns 0 if the cursor/centre can't be read. */
static int look_motion_delta(int *dx, int *dy)
{
    POINT c, p;
    if (!client_center_screen(g_game_hwnd, &c) || !GetCursorPos(&p)) return 0;
    *dx = p.x - c.x; *dy = p.y - c.y;
    if (g_setcursorpos_p) g_setcursorpos_p(c.x, c.y);
    if (!g_look_primed) { g_look_primed = 1; *dx = *dy = 0; }
    return 1;
}

/* Immediate-mode read (IDirectInputDevice8::GetDeviceState): fills a
 * DIMOUSESTATE(2) whose first two LONGs are the relative lX/lY. While camera-
 * look is active we overwrite those two with our cursor-derived delta; the
 * buttons and wheel after them are left exactly as DirectInput filled them. */
static HRESULT WINAPI my_getdevicestate(void *dev, DWORD cb, void *ptr)
{
    HRESULT hr = g_real_getdevicestate(dev, cb, ptr);
    /* The shared-vtable patch means we see every device's reads (mouse AND
     * keyboard), so identify the mouse by its small state size (DIMOUSESTATE=16,
     * DIMOUSESTATE2=20; a keyboard is 256) and only rewrite during camera-look. */
    int mouse_sized = (cb >= 8 && cb <= 32);
    if (SUCCEEDED(hr) && g_look_active && mousemotionfix_wanted() && ptr &&
        mouse_sized) {
        if (!g_di_mouse) g_di_mouse = dev;   /* remember for the buffered path */
        int dx, dy;
        if (look_motion_delta(&dx, &dy)) {
            ((LONG *)ptr)[0] = dx;   /* lX */
            ((LONG *)ptr)[1] = dy;   /* lY */
        }
    }
    return hr;
}

/* Buffered read (IDirectInputDevice8::GetDeviceData): rewrite motion, keep
 * buttons. Only consuming reads of the mouse during camera-look are rewritten;
 * PEEK reads, other devices and menus pass through untouched. */
static HRESULT WINAPI my_getdevicedata(void *dev, DWORD cb, void *rgdod,
                                       DWORD *pdwInOut, DWORD flags)
{
    DWORD cap = pdwInOut ? *pdwInOut : 0;
    HRESULT hr = g_real_getdevicedata(dev, cb, rgdod, pdwInOut, flags);

    if (FAILED(hr) || dev != g_di_mouse || !g_look_active ||
        !mousemotionfix_wanted() || (flags & DIGDD_PEEK) ||
        !rgdod || !pdwInOut || cb < sizeof(DWORD) * 4)
        return hr;

    int dx, dy;
    if (!look_motion_delta(&dx, &dy))
        return hr;

    BYTE *buf = (BYTE *)rgdod;
    DWORD n = *pdwInOut;

    /* Drop the device's own (lossy) X/Y relative events; keep buttons/wheel. */
    DWORD w = 0;
    for (DWORD r = 0; r < n; r++) {
        DWORD ofs = *(DWORD *)(buf + (size_t)r * cb);
        if (ofs == H2_DIMOFS_X || ofs == H2_DIMOFS_Y) continue;
        if (w != r) memcpy(buf + (size_t)w * cb, buf + (size_t)r * cb, cb);
        w++;
    }

    /* Inject our motion as fresh X/Y events (if the game's buffer has room). */
    static DWORD seq;
    DWORD now = GetTickCount();
    if (dx && w < cap) {
        BYTE *e = buf + (size_t)w * cb;
        *(DWORD *)(e + 0) = H2_DIMOFS_X;
        *(DWORD *)(e + 4) = (DWORD)dx;
        *(DWORD *)(e + 8) = now;
        *(DWORD *)(e + 12) = ++seq;
        w++;
    }
    if (dy && w < cap) {
        BYTE *e = buf + (size_t)w * cb;
        *(DWORD *)(e + 0) = H2_DIMOFS_Y;
        *(DWORD *)(e + 4) = (DWORD)dy;
        *(DWORD *)(e + 8) = now;
        *(DWORD *)(e + 12) = ++seq;
        w++;
    }

    *pdwInOut = w;
    return hr;
}

/* Patch one function pointer in a COM object's vtable. */
static int patch_vtable_slot(void *obj, int index, void *hook, void **orig)
{
    void **vtbl = *(void ***)obj;
    DWORD old;
    if (!VirtualProtect(&vtbl[index], sizeof(void *), PAGE_READWRITE, &old))
        return 0;
    if (orig) *orig = vtbl[index];
    vtbl[index] = hook;
    VirtualProtect(&vtbl[index], sizeof(void *), old, &old);
    FlushInstructionCache(GetCurrentProcess(), &vtbl[index], sizeof(void *));
    return 1;
}

/* IID_IDirectInput8A = {BF798030-483A-4DA2-AA99-5D64ED369700} */
static const GUID H2_IID_IDirectInput8A =
    { 0xBF798030, 0x483A, 0x4DA2, { 0xAA,0x99,0x5D,0x64,0xED,0x36,0x97,0x00 } };
#ifndef DIRECTINPUT_VERSION
#define DIRECTINPUT_VERSION 0x0800
#endif
typedef ULONG (WINAPI *h2_release_t)(void *);

/* Patch the SysMouse device's shared vtable. DirectInput device objects of one
 * class share a single vtable in dinput8's memory, so we create our OWN
 * throwaway SysMouse device just to reach that vtable, patch its GetDeviceState
 * (9) / GetDeviceData (10) slots, then release it. The game's mouse device —
 * whenever it was created, even before we loaded — dispatches through the same
 * vtable and so routes through our hooks. This is timing-proof, unlike catching
 * CreateDevice (which we would miss for an already-created device). */
static int install_di_shared_vtable(void)
{
    HMODULE di = GetModuleHandleA("dinput8.dll");
    if (!di) return 0;
    h2_di8create_t create = (h2_di8create_t)(uintptr_t)
        GetProcAddress(di, "DirectInput8Create");
    if (!create) return 0;
    void *pdi = NULL;
    HRESULT hr = create(GetModuleHandleA(NULL), DIRECTINPUT_VERSION,
                        (REFIID)&H2_IID_IDirectInput8A, &pdi, NULL);
    if (FAILED(hr) || !pdi) {
        logf_("DI shared-vtable: DirectInput8Create failed %#lx",
              (unsigned long)hr);
        return 0;
    }
    void **divt = *(void ***)pdi;
    h2_createdevice_t cdev = (h2_createdevice_t)divt[3];   /* CreateDevice */
    h2_release_t direl = (h2_release_t)divt[2];            /* Release */
    void *dev = NULL;
    hr = cdev(pdi, (REFGUID)&H2_GUID_SysMouse, &dev, NULL);
    int a = 0, b = 0;
    if (SUCCEEDED(hr) && dev) {
        a = patch_vtable_slot(dev, 9, (void *)my_getdevicestate,
                              (void **)&g_real_getdevicestate);
        b = patch_vtable_slot(dev, 10, (void *)my_getdevicedata,
                              (void **)&g_real_getdevicedata);
        ((h2_release_t)(*(void ***)dev)[2])(dev);         /* release our device */
    } else {
        logf_("DI shared-vtable: CreateDevice(SysMouse) failed %#lx",
              (unsigned long)hr);
    }
    direl(pdi);                                            /* release our IDI8 */
    logf_("DI shared-vtable patch: SysMouse GetDeviceState=%d GetDeviceData=%d "
          "(mouse motion fix %s)", a, b, (a || b) ? "armed" : "FAILED");
    return a || b;
}

/* Install the motion fix once dinput8 is loaded, via the shared-vtable patch. */
static void hook_dinput(void)
{
    static int done;
    if (done || !g_enabled || !mousemotionfix_wanted()) return;
    if (!g_setcursorpos_p) {
        HMODULE u = GetModuleHandleA("user32.dll");
        g_setcursorpos_p = u ? (BOOL (WINAPI *)(int, int))(uintptr_t)
            GetProcAddress(u, "SetCursorPos") : NULL;
    }
    if (!GetModuleHandleA("dinput8.dll")) return;      /* not loaded yet; retry */
    install_di_shared_vtable();
    done = 1;                                           /* one shot */
}

/* Keep the OS pointer out of the screen's bottom/top edge strips. macOS
 * force-shows the cursor when the pointer dwells in the auto-hidden Dock /
 * menu-bar reveal zones at the screen edges — a WindowServer behaviour that
 * no app-side cursor state can override (observed: pointer pinned at the
 * bottom edge with a NULL win32 cursor and a negative show count still gets
 * the host arrow). Nudge the pointer back just off the edge while the game
 * is foreground. Missions are unaffected (mouse-look recenters the pointer);
 * menus merely cannot park the pointer on the outermost two pixel rows. */
static void nudge_cursor_off_edges(void)
{
    if (!g_enabled || !cursorfix_wanted()) return;
    HWND w = g_game_hwnd;
    if (!w || GetForegroundWindow() != w) return;
    int dh = GetSystemMetrics(SM_CYSCREEN);
    POINT pt;
    if (dh < 40 || !GetCursorPos(&pt)) return;
    int ny = pt.y < 2 ? 2 : pt.y > dh - 3 ? dh - 3 : pt.y;
    if (ny != pt.y)
        SetCursorPos(pt.x, ny);
}

/* Force the macOS app to activate so winemac hides the menu bar and captures
 * the display (both gated on [NSApp isActive]). A Steam-launched process does
 * not get macOS activation for free, and a plain SetForegroundWindow does not
 * grant it: winemac's set_focus only asks Cocoa to activate the app when
 * activate_on_focus_time was armed within the last 2s — which normally only a
 * user click does (it posts WM_MACDRV_ACTIVATE_ON_FOLLOWING_FOCUS, then the
 * ensuing focus event activates). We reproduce a click's effect: arm that
 * driver message on the game thread, then force a real focus transition by
 * bouncing foreground through a throwaway window, so set_focus runs again
 * while armed and activates the app. Idempotent once active. */
#ifndef WM_MACDRV_ACTIVATE_ON_FOLLOWING_FOCUS
#define WM_MACDRV_ACTIVATE_ON_FOLLOWING_FOCUS 0x80001001  /* macdrv.h */
#endif

static void kick_app_activation(HWND hwnd)
{
    if (!hwnd || !IsWindow(hwnd)) return;

    /* A throwaway top-level window to steal foreground for an instant. */
    HWND tmp = CreateWindowExA(WS_EX_TOOLWINDOW, "STATIC", "", WS_POPUP,
                               0, 0, 1, 1, NULL, NULL,
                               GetModuleHandleA(NULL), NULL);
    if (tmp) {
        ShowWindow(tmp, SW_SHOWNA);
        SetForegroundWindow(tmp);      /* game loses foreground */
    }
    /* Arm activation on the game window's thread, then hand foreground back:
     * the arm message is queued to that thread before the focus event, so it
     * is consumed by the set_focus that follows and the app activates. */
    PostMessageA(hwnd, WM_MACDRV_ACTIVATE_ON_FOLLOWING_FOCUS, 0, 0);
    SetForegroundWindow(hwnd);
    SetFocus(hwnd);
    if (tmp) {
        ShowWindow(tmp, SW_HIDE);
        DestroyWindow(tmp);
    }
    logf_("app-activation kick sent (arm + foreground bounce)");
}

static LRESULT CALLBACK getmsg_hook(int code, WPARAM wp, LPARAM lp)
{
    if (code >= 0) {
        prime_driver_hide();
        assert_cursor_hidden("message hook");
        nudge_cursor_off_edges();
    }
    return CallNextHookEx(g_msg_hook, code, wp, lp);
}

/* Thread-targeted WH_GETMESSAGE hooks: the hook proc runs on the hooked
 * thread whenever it pumps a message, keeping the hidden state asserted per
 * thread queue. Menus / DirectInput can pump input on threads other than
 * the device window's, so hook every window-owning thread in the process. */
#define MAX_HOOKED_TIDS 8
static DWORD g_hooked_tids[MAX_HOOKED_TIDS];
static int g_nhooked;

static BOOL CALLBACK hook_thread_of_window(HWND w, LPARAM lp)
{
    (void)lp;
    DWORD pid = 0;
    DWORD tid = GetWindowThreadProcessId(w, &pid);
    if (!tid || pid != GetCurrentProcessId()) return TRUE;
    for (int i = 0; i < g_nhooked; i++)
        if (g_hooked_tids[i] == tid) return TRUE;
    if (g_nhooked < MAX_HOOKED_TIDS) {
        HHOOK h = SetWindowsHookExA(WH_GETMESSAGE, getmsg_hook, NULL, tid);
        if (h) {
            if (!g_msg_hook) g_msg_hook = h;
            g_hooked_tids[g_nhooked++] = tid;
            logf_("WH_GETMESSAGE cursor hook installed on thread %lu",
                  (unsigned long)tid);
        }
    }
    return TRUE;
}

#ifndef GCL_HCURSOR
#define GCL_HCURSOR (-12)
#endif

static void backdrop_maintain(void);   /* letterbox upkeep, defined below */

/* Supervisor thread: installs the in-thread hooks as the game's windows and
 * threads appear, and keeps the game window's CLASS cursor NULL so
 * DefWindowProc's WM_SETCURSOR handling cannot install a real cursor. It
 * must NOT touch ShowCursor/SetCursor itself — from a foreign thread that
 * state does not stick (see the comment above). */
static DWORD WINAPI cursor_watch(LPVOID arg)
{
    (void)arg;
    for (int tick = 0;; tick++) {
        Sleep(20);
        hook_game_imports();
        hook_clipcursor();   /* edge-wall fix; wanted even with CursorFix off */
        hook_dinput();       /* slow-move fix: feed DI mouse from GetCursorPos */
        if ((tick % 25) == 0)          /* every ~500ms */
            backdrop_maintain();       /* keep letterbox bars black + beneath */
        if (!cursorfix_wanted())
            continue;
        if ((tick % 10) == 0)          /* rescan for new threads at 5 Hz */
            EnumWindows(hook_thread_of_window, 0);
        nudge_cursor_off_edges();      /* backstop between input messages */
        HWND w = g_game_hwnd;
        /* Startup activation: reproduce a user click a few times over the
         * first couple of seconds so the macOS app activates (menu bar hides
         * / display captures) without needing a real click. A handful of
         * spaced kicks covers the case where the first fires before the
         * Cocoa window is ready; macOS then keeps us active. */
        if (g_fg_deadline && w && IsWindow(w)) {
            DWORD now = GetTickCount();
            if (now >= g_next_kick && g_kicks_left > 0) {
                kick_app_activation(w);
                g_kicks_left--;
                g_next_kick = now + 500;
                if (g_kicks_left == 0) {
                    g_fg_deadline = 0;
                    logf_("startup activation kicks done");
                }
            }
        }
        if (w && IsWindow(w) &&
            (HCURSOR)(uintptr_t)GetClassLongA(w, GCL_HCURSOR) != NULL) {
            SetClassLongA(w, GCL_HCURSOR, 0);
            logf_("class cursor cleared to NULL");
        }
    }
    return 0;
}

/* Borderless-fullscreen is the default on every platform (-1 auto = on).
 * On Windows the alternative — a forced *plain* window at the backbuffer
 * size — is both ugly and unstable (the stock engine does not expect to be
 * windowed and could misbehave/crash), so auto means "borderless" there too;
 * exclusive fullscreen (Fullscreen=1, valid mode) is handled separately. Set
 * Borderless=0 to force a plain window on purpose. */
static int borderless_wanted(void)
{
    return g_borderless != 0;   /* -1 auto and 1 -> borderless; 0 -> plain */
}

/* Strip the game window to a borderless popup with a client area of w x h
 * at x,y (no frame, so window rect == client). */
static void set_borderless_window(HWND hwnd, int x, int y, int w, int h)
{
    if (!hwnd) return;
    LONG style = GetWindowLongA(hwnd, GWL_STYLE);
    style &= ~(WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX |
               WS_SYSMENU | WS_BORDER | WS_DLGFRAME);
    style |= WS_POPUP;
    SetWindowLongA(hwnd, GWL_STYLE, style);
    SetWindowPos(hwnd, HWND_TOP, x, y, w, h,
                 SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    /* winemac only treats a fullscreen-sized window as fullscreen (raised
     * level, hidden menu bar, captured display when CaptureDisplaysForFull-
     * screen is set) while the macOS app is ACTIVE. A Steam-launched process
     * is not activated for free, so arm the watchdog to kick activation over
     * the next couple of seconds (see kick_app_activation). */
    SetForegroundWindow(hwnd);
    SetFocus(hwnd);
    g_kicks_left = 5;
    g_next_kick = GetTickCount() + 300;
    g_fg_deadline = GetTickCount() + 20000;
    logf_("window %p -> borderless popup %dx%d at %d,%d (activation queued)",
          (void *)hwnd, w, h, x, y);
}

/* Letterbox backdrop: preserving the backbuffer aspect makes the game window
 * SMALLER than the screen, and the leftover strips must be black. A separate
 * full-screen black window behind the game window supplies them. Design
 * constraints on this stack:
 *
 *  - It is created on the game's own thread (fix_present runs there on
 *    CreateDevice/Reset), and painted directly with GetDC+FillRect so the
 *    bars never depend on anyone dispatching WM_PAINT for it (macOS keeps
 *    the painted backing store).
 *  - It is sized 1px past the screen's right/bottom edges — with the game
 *    window letterboxed, the BACKDROP is now the window at the screen edges,
 *    so it inherits the winemac NSMouseInRect edge case (a pointer pinned on
 *    the exact bottom/right boundary counts as "outside", flashing the host
 *    arrow) that the full-fill window worked around the same way.
 *  - Keeping a fullscreen-SIZED window in the app also keeps winemac's
 *    fullscreen treatment engaged (hidden menu bar, raised window level)
 *    now that the game window itself no longer covers the screen; winemac's
 *    level adjustment keeps windows stacked above it at least as high, so
 *    the game window in front is not swallowed. */
static HINSTANCE g_inst;
static HWND g_backdrop;

static void backdrop_paint(HWND w)
{
    RECT rc;
    HDC dc = w ? GetDC(w) : NULL;
    if (!dc) return;
    if (GetClientRect(w, &rc))
        FillRect(dc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
    ReleaseDC(w, dc);
    /* Clear the update region: nothing else paints this window, so a pending
     * WM_PAINT would otherwise stay pending forever and keep the "needs
     * repainting?" check in backdrop_maintain firing every tick. */
    ValidateRect(w, NULL);
}

/* Create (once) / size / show the backdrop and slot it directly beneath the
 * game window. Runs on the game thread from fix_present. */
static void backdrop_show(HWND game, int dw, int dh)
{
    if (!g_backdrop) {
        static int cls_done;
        if (!cls_done) {
            WNDCLASSA wc;
            memset(&wc, 0, sizeof(wc));
            wc.lpfnWndProc = DefWindowProcA;
            wc.hInstance = g_inst;
            wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
            /* hCursor stays NULL so hovering the bars cannot re-arm a cursor */
            wc.lpszClassName = "H2SALetterbox";
            RegisterClassA(&wc);
            cls_done = 1;
        }
        g_backdrop = CreateWindowExA(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                                     "H2SALetterbox", "", WS_POPUP,
                                     0, 0, dw + 1, dh + 1,
                                     NULL, NULL, g_inst, NULL);
        if (!g_backdrop) {
            logf_("letterbox backdrop CreateWindow FAILED (%lu)",
                  (unsigned long)GetLastError());
            return;
        }
        logf_("letterbox backdrop %p created (%dx%d)", (void *)g_backdrop,
              dw + 1, dh + 1);
    }
    SetWindowPos(g_backdrop, game, 0, 0, dw + 1, dh + 1,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    backdrop_paint(g_backdrop);
}

static void backdrop_hide(void)
{
    if (g_backdrop && IsWindowVisible(g_backdrop))
        ShowWindow(g_backdrop, SW_HIDE);
}

/* Periodic upkeep from the watcher thread (~2Hz). In the steady state this
 * must be QUERIES ONLY: an earlier revision unconditionally refilled the
 * full-screen backdrop (a multi-MB winemac surface flush on the Cocoa main
 * thread, where presents also run) and re-slotted it with SetWindowPos on
 * every tick (the "directly beneath?" check compared against GW_HWNDNEXT,
 * which any INVISIBLE window between the two defeats, so it never settled
 * and winemac reshuffled its window order at 2Hz) — together a rhythmic
 * in-game stutter. Now: repaint only when something actually invalidated
 * the backdrop, and re-slot only when it is not below the game window at
 * all (invisible in-between windows cover nothing and are fine). */
static void backdrop_maintain(void)
{
    HWND b = g_backdrop, g = g_game_hwnd;
    if (!b || !g || !IsWindowVisible(b) || !IsWindow(g)) return;

    int below = 0;
    HWND w = g;
    for (int i = 0; i < 64 && (w = GetWindow(w, GW_HWNDNEXT)) != NULL; i++)
        if (w == b) { below = 1; break; }
    if (!below) {
        SetWindowPos(b, g, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE |
                     SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
        static LONG logs;
        if (logs < 8) {
            InterlockedIncrement(&logs);
            logf_("letterbox backdrop re-slotted below the game window");
        }
    }

    RECT upd;
    if (GetUpdateRect(b, &upd, FALSE))
        backdrop_paint(b);       /* validates, so it doesn't refire */
}

/* Is w x h an exact 32-bit display mode the driver enumerates? Exclusive
 * fullscreen only works at an enumerated mode; under CrossOver that list is
 * the Mac display's (Retina-scaled, 16:10) modes, and none of the classic
 * game resolutions (800x600, 1280x1024, 1920x1080) are in it — which is why
 * the stock game's exclusive-fullscreen CreateDevice fails. */
static int is_display_mode(int w, int h)
{
    DEVMODEA dm;
    memset(&dm, 0, sizeof(dm));
    dm.dmSize = sizeof(dm);
    for (DWORD i = 0; EnumDisplaySettingsA(NULL, i, &dm); i++) {
        if (dm.dmBitsPerPel >= 32 &&
            (int)dm.dmPelsWidth == w && (int)dm.dmPelsHeight == h)
            return 1;
    }
    return 0;
}

/* Cache the HAL's supported presentation intervals once. We create our own
 * throwaway IDirect3D8 (via the loaded d3d8.dll proxy export) and query
 * D3DCAPS8 — GetDeviceCaps needs no device, so there is no mode switch. This
 * runs lazily from the first CreateDevice fixup (game thread, outside the
 * loader lock, after the game's own Direct3DCreate8 has already mapped the
 * system d3d8), so it is safe. */
static void read_caps_once(void)
{
    if (g_caps_done) return;
    g_caps_done = 1;
    HMODULE ld = GetModuleHandleA("d3d8.dll");
    if (!ld) return;
    IDirect3D8 *(WINAPI *create)(UINT) =
        (IDirect3D8 *(WINAPI *)(UINT))(uintptr_t)
        GetProcAddress(ld, "Direct3DCreate8");
    if (!create) return;
    IDirect3D8 *d = create(D3D_SDK_VERSION);
    if (!d) return;
    D3DCAPS8 caps;
    if (SUCCEEDED(d->lpVtbl->GetDeviceCaps(d, D3DADAPTER_DEFAULT,
                                           D3DDEVTYPE_HAL, &caps)))
        g_present_intervals = caps.PresentationIntervals;
    /* Remember the actual desktop backbuffer format. A windowed device needs
     * a concrete format: native D3D8 rejects D3DFMT_UNKNOWN for a windowed
     * backbuffer with D3DERR_INVALIDCALL (wined3d tolerates it, which is why
     * the borderless path worked under CrossOver but not on real Windows). */
    D3DDISPLAYMODE mode;
    if (SUCCEEDED(d->lpVtbl->GetAdapterDisplayMode(d, D3DADAPTER_DEFAULT,
                                                   &mode)))
        g_desktop_fmt = mode.Format;
    d->lpVtbl->Release(d);
    logf_("device present-interval caps = 0x%08lx, desktop fmt = %d",
          (unsigned long)g_present_intervals, g_desktop_fmt);
}

/* Is w x h @ refresh Hz an enumerated 32-bit display mode? */
static int is_refresh_mode(int w, int h, int hz)
{
    DEVMODEA dm;
    memset(&dm, 0, sizeof(dm));
    dm.dmSize = sizeof(dm);
    for (DWORD i = 0; EnumDisplaySettingsA(NULL, i, &dm); i++) {
        if (dm.dmBitsPerPel >= 32 &&
            (int)dm.dmPelsWidth == w && (int)dm.dmPelsHeight == h &&
            (int)dm.dmDisplayFrequency == hz)
            return 1;
    }
    return 0;
}

/* Choose an exclusive-fullscreen presentation interval that lets the DISPLAY
 * pace the frame rate, so the cap is jitter-free and the software limiter can
 * stand down (two independent clocks — the vsync grid and a QPC accumulator —
 * beat against each other and cause the "60fps but choppy" judder).
 *
 * When the refresh is ~an integer multiple N of the target (e.g. 240Hz / 60 =
 * 4) and INTERVAL_N is supported, present every Nth vblank: the hardware then
 * scans out exactly `cap` unique frames per second, perfectly spaced. If the
 * refresh is not a clean multiple, try pinning an enumerated cap-Hz (or
 * 2*cap-Hz) mode; failing that, vsync every frame (tear-free) and let the
 * software limiter do the cap. NOTE: non-DEFAULT intervals are fullscreen-only
 * in D3D8 — the windowed/borderless path must use DEFAULT + software limiter. */
static const DWORD k_intervals[5] = {
    0, D3DPRESENT_INTERVAL_ONE, D3DPRESENT_INTERVAL_TWO,
    D3DPRESENT_INTERVAL_THREE, D3DPRESENT_INTERVAL_FOUR
};

static void choose_fs_interval(D3DPRESENT_PARAMETERS *pp)
{
    g_hw_paced = 0;
    read_caps_once();

    if (g_vsync == 0) {                     /* vsync off: software cap only */
        pp->FullScreen_PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
        logf_("fullscreen: VSync=0 -> immediate present, software limiter caps");
        return;
    }
    /* default when we cannot pace in hardware: vsync every frame (tear-free) */
    pp->FullScreen_PresentationInterval = D3DPRESENT_INTERVAL_ONE;
    if (g_fpscap <= 0)                       /* uncapped: just vsync once */
        return;

    DEVMODEA dm;
    memset(&dm, 0, sizeof(dm));
    dm.dmSize = sizeof(dm);
    int hz = 0;
    if (EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &dm) &&
        (dm.dmFields & DM_DISPLAYFREQUENCY))
        hz = (int)dm.dmDisplayFrequency;
    if (hz <= 1) {                           /* some drivers report 0/1 */
        logf_("fullscreen: refresh unknown, vsync on + software limiter");
        return;
    }

    int n = (hz + g_fpscap / 2) / g_fpscap;  /* nearest integer multiple */
    if (n >= 1 && n <= 4 && abs(hz - n * g_fpscap) <= 2 &&
        (g_present_intervals & k_intervals[n])) {
        pp->FullScreen_PresentationInterval = k_intervals[n];
        pp->FullScreen_RefreshRateInHz = 0;  /* keep the desktop refresh */
        g_hw_paced = 1;
        logf_("fullscreen vsync pacing: %dHz / cap %d -> present every %d "
              "vblank(s); software limiter stands down", hz, g_fpscap, n);
        return;
    }
    /* refresh not a clean multiple: pin an enumerated cap-Hz mode if there is
     * one (present every vblank), else 2*cap-Hz (present every 2nd). */
    if (is_refresh_mode((int)pp->BackBufferWidth,
                        (int)pp->BackBufferHeight, g_fpscap)) {
        pp->FullScreen_RefreshRateInHz = (UINT)g_fpscap;
        pp->FullScreen_PresentationInterval = D3DPRESENT_INTERVAL_ONE;
        g_hw_paced = 1;
        logf_("fullscreen vsync pacing: pinned %dHz mode, present every vblank",
              g_fpscap);
        return;
    }
    if ((g_present_intervals & D3DPRESENT_INTERVAL_TWO) &&
        is_refresh_mode((int)pp->BackBufferWidth,
                        (int)pp->BackBufferHeight, g_fpscap * 2)) {
        pp->FullScreen_RefreshRateInHz = (UINT)(g_fpscap * 2);
        pp->FullScreen_PresentationInterval = D3DPRESENT_INTERVAL_TWO;
        g_hw_paced = 1;
        logf_("fullscreen vsync pacing: pinned %dHz mode, present every 2nd "
              "vblank", g_fpscap * 2);
        return;
    }
    logf_("fullscreen: %dHz is not a clean multiple of cap %d and no %d/%dHz "
          "mode; vsync on, software limiter caps", hz, g_fpscap, g_fpscap,
          g_fpscap * 2);
}

/* Frame-pacing mode. Decided ONCE by ~1.5s of calibration (see frame_limit),
 * then committed — never mixed per frame. Re-decided after every
 * CreateDevice/Reset, because a display-mode change can flip whether the
 * driver honours the swap interval (i.e. whether Present itself paces). */
enum { PACE_CAL, PACE_HW, PACE_SW };
static int g_pace_mode = PACE_CAL;
static int g_cal_frames, g_cal_slept;
static LONGLONG g_pace_freq, g_pace_next;

static void pace_recalibrate(const char *why)
{
    if (g_pace_mode == PACE_CAL && !g_cal_frames) return;  /* fresh already */
    g_pace_mode = PACE_CAL;
    g_cal_frames = g_cal_slept = 0;
    logf_("frame pacing: recalibrating (%s)", why);
}

/* Presentation-parameters fixup: force windowed (the startup fix) and,
 * when borderless is wanted, expand to the desktop and strip the window. */
static void fix_present(D3DPRESENT_PARAMETERS *pp, HWND hFocusWindow,
                        int is_reset)
{
    if (!g_enabled) return;
    int was_fullscreen = !pp->Windowed;

    /* Record the game window for the cursor hooks/watchdog, and assert the
     * hidden cursor from here — CreateDevice/Reset run on the game's thread,
     * so this covers startup and mission loads on every path (including
     * exclusive fullscreen, which returns early below). */
    HWND devwnd = pp->hDeviceWindow ? pp->hDeviceWindow : hFocusWindow;
    if (devwnd) g_game_hwnd = devwnd;
    assert_cursor_hidden(is_reset ? "device reset" : "device create");
    InterlockedExchange(&g_prime_needed, 1);   /* re-tell the Mac driver */
    pace_recalibrate(is_reset ? "device reset" : "device create");

    /* Pin the backbuffer to the Hitman2.ini resolution. That is the value
     * the engine lays its viewport / HUD / mouse mapping out against, so the
     * device and the engine stay in agreement (no corner-rendering), and it
     * replaces whatever RenderD3D's fullscreen mode-selection produced —
     * which for a resolution not in its mode ladder is a stale mode or an
     * uninitialised, garbage height. The projection FOV fixup below makes
     * any aspect correct. */
    if (g_ini_w && g_ini_h) {
        pp->BackBufferWidth = (UINT)g_ini_w;
        pp->BackBufferHeight = (UINT)g_ini_h;
    }

    /* Exclusive fullscreen — REAL WINDOWS ONLY. It needs the requested
     * resolution to be an actual enumerated display mode (else CreateDevice
     * returns D3DERR_NOTAVAILABLE); we don't clamp to a different mode because
     * the engine would keep laying out for the ini resolution and mismatch the
     * device.
     *
     * Under Wine/CrossOver on a Retina Mac, exclusive fullscreen is broken:
     * winemac (with the default RetinaMode=y) drives the captured display at a
     * 2x backing scale, so a mode like 1920x1200 becomes a 3840x2400-pixel
     * surface — larger than the physical panel — and the picture spills past
     * the screen edges with the HUD clipped off. There is no per-app fix from
     * inside the game, and the mode change also breaks alt-tab and the cursor.
     * So on Wine we never take the exclusive path: Fullscreen=1 falls through
     * to borderless-fullscreen, which fills the screen correctly (macOS
     * composites/scales the window properly) and is what "fullscreen" should
     * mean on this stack. */
    if (g_fullscreen && !is_wine() &&
        is_display_mode((int)pp->BackBufferWidth,
                        (int)pp->BackBufferHeight)) {
        pp->Windowed = FALSE;
        pp->BackBufferFormat = D3DFMT_X8R8G8B8;
        pp->FullScreen_RefreshRateInHz = 0;   /* default refresh */
        if (pp->SwapEffect == D3DSWAPEFFECT_COPY)
            pp->SwapEffect = D3DSWAPEFFECT_DISCARD;
        /* Let the display pace the frame rate where it can (vsync divisor),
         * so we get a jitter-free cap without the software limiter fighting
         * the vsync clock. May also set FullScreen_RefreshRateInHz. */
        choose_fs_interval(pp);
        g_borderless_active = 0;
        backdrop_hide();
        logf_("present fixup (%s): exclusive fullscreen %ux%u (valid mode)",
              is_reset ? "reset" : "create",
              pp->BackBufferWidth, pp->BackBufferHeight);
        return;
    }
    if (g_fullscreen && is_wine())
        logf_("Fullscreen=1 on Wine/CrossOver: exclusive fullscreen oversizes "
              "on Retina (2x backing scale -> surface bigger than the panel, "
              "content clipped) — using borderless fullscreen, which fills the "
              "screen correctly. Real exclusive fullscreen is Windows-only.");
    else if (g_fullscreen)
        logf_("Fullscreen=1 but %ux%u is not an enumerated display mode — "
              "using borderless windowed instead (set Resolution to a mode "
              "your display exposes for true fullscreen)",
              pp->BackBufferWidth, pp->BackBufferHeight);

    /* Windowed path: this is what makes CreateDevice succeed on the CrossOver
     * D3DMetal stack (the stock exclusive-fullscreen device fails with
     * "Unable to create device"). A windowed device renders into a backbuffer
     * of the desktop colour format, so there is no fullscreen mode / colour-
     * depth match to fail. The backbuffer format must be a CONCRETE desktop
     * format, though: wined3d accepts D3DFMT_UNKNOWN (matches desktop) but
     * native D3D8 rejects UNKNOWN for a windowed backbuffer with
     * D3DERR_INVALIDCALL — which broke borderless on real Windows. Use the
     * actual desktop format (queried once) so both stacks create. */
    read_caps_once();
    pp->Windowed = TRUE;
    pp->BackBufferFormat = g_desktop_fmt;
    pp->FullScreen_RefreshRateInHz = 0;
    pp->FullScreen_PresentationInterval = D3DPRESENT_INTERVAL_DEFAULT;
    if (pp->SwapEffect == D3DSWAPEFFECT_FLIP)
        pp->SwapEffect = D3DSWAPEFFECT_DISCARD;  /* FLIP is fullscreen-only */

    /* A Fullscreen=1 request on Wine becomes borderless-fullscreen (fills the
     * desktop) even if Borderless was turned off — that is what the user asked
     * for. Otherwise honour the borderless setting. */
    if ((was_fullscreen || g_fullscreen) &&
        (borderless_wanted() || (g_fullscreen && is_wine())))
        g_borderless_active = 1;

    /* Borderless-fullscreen: a frameless window covering the whole desktop,
     * with the backbuffer rendered AT the desktop resolution, so it fills the
     * screen edge to edge — no bars, no stretching, and no risky display-mode
     * switch. The engine's own idea of the resolution no longer has to match:
     * the loader clamps the (garbage) viewport to this backbuffer and rebuilds
     * the projection aspect from it, so 2D and 3D both come out right at
     * whatever size we pick. Rendering at the logical desktop size keeps the
     * cost sane on Retina (the OS upscales to native pixels). */
    int dw = GetSystemMetrics(SM_CXSCREEN);
    int dh = GetSystemMetrics(SM_CYSCREEN);
    HWND hwnd = devwnd;
    int letterbox = 0;
    if (g_borderless_active && dw >= 320 && dh >= 200 && hwnd) {
        /* Keep the backbuffer at the Hitman2.ini resolution (now honoured
         * end-to-end thanks to the snap patch) and scale it across a
         * borderless window. With PreserveAspect (default) the window is the
         * largest centred rect OF THE BACKBUFFER'S ASPECT that fits the
         * screen, and a full-screen black backdrop window supplies the
         * letterbox/pillarbox bars — nothing is stretched. PreserveAspect=0
         * stretches to fill the desktop as before.
         *
         * The stretch-fill window is oversized by one pixel on the right and
         * bottom: the Mac driver treats a pointer on a window frame's exact
         * bottom/right boundary as OUTSIDE the window (Cocoa's NSMouseInRect
         * excludes those two edges), and for a point over "no Wine window"
         * it resets to the host arrow cursor and unhides it — so a cursor
         * pinned at the bottom of the screen flashed the macOS arrow in the
         * menus. With the frame 1px past the screen edge the pinned cursor
         * stays strictly inside. The extra pixel is off-screen and the
         * stretch difference is imperceptible. (When letterboxing, the
         * BACKDROP covers the screen edges and carries the same +1 instead.) */
        int tx = 0, ty = 0, tw = dw + 1, th = dh + 1;
        if (g_preserveaspect && pp->BackBufferWidth && pp->BackBufferHeight) {
            double img = (double)pp->BackBufferWidth /
                         (double)pp->BackBufferHeight;
            double scr = (double)dw / (double)dh;
            /* aspects within 1% -> treat as matching, no bars */
            if (fabs(img - scr) > 0.01 * scr) {
                if (img > scr) {          /* wider than screen: bars top+bottom */
                    tw = dw;
                    th = (int)((double)dw / img + 0.5);
                } else {                  /* narrower: bars left+right */
                    th = dh;
                    tw = (int)((double)dh * img + 0.5);
                }
                if (tw < 1) tw = 1;
                if (th < 1) th = 1;
                tx = (dw - tw) / 2;
                ty = (dh - th) / 2;
                letterbox = 1;
                logf_("preserve aspect: %ux%u backbuffer (%.3f) vs %dx%d "
                      "screen (%.3f) -> image %dx%d at %d,%d (black bars "
                      "left/right %d px, top/bottom %d px)",
                      pp->BackBufferWidth, pp->BackBufferHeight, img,
                      dw, dh, scr, tw, th, tx, ty, tx, ty);
            }
        }
        set_borderless_window(hwnd, tx, ty, tw, th);
        if (letterbox)
            backdrop_show(hwnd, dw, dh);
    }
    if (!letterbox)
        backdrop_hide();

    logf_("present fixup (%s): %s -> windowed %ux%u%s",
          is_reset ? "reset" : "create",
          was_fullscreen ? "fullscreen" : "windowed",
          pp->BackBufferWidth, pp->BackBufferHeight,
          g_borderless_active ? (letterbox ? " (borderless, letterboxed)"
                                           : " (borderless, filling desktop)")
                              : "");
}

/* Projection fixup. Hitman 2's fixed-function projection keeps the
 * horizontal scale in _11 and puts the aspect in the VERTICAL scale:
 * _22 = _11 * (viewport_width / viewport_height). (Observed at the working
 * 1280x1024 mode: the 3D scene renders into a 1280x750 viewport with
 * _22 = 1.70455 ~= 1280/750.) At a resolution whose height the engine does
 * not recognise, the height is garbage, so _22 collapses to ~0 and the whole
 * scene squashes to a horizontal line. We detect that collapsed _22 and
 * rebuild it from the real (clamped-to-backbuffer) viewport aspect, which
 * matches the viewport the loader forces, so the 3D image is undistorted.
 *
 * A near-zero _22 is the unambiguous signature of the bug: legitimate
 * projections (the 2D/menu pass with _11=_22=1, or a correctly computed 3D
 * camera with _22 ~= aspect) are left untouched. FOVFactor optionally widens
 * the view by scaling both axes. */
static void fix_projection(D3DMATRIX *m, unsigned int bbw, unsigned int bbh)
{
    if (!g_enabled || !bbw || !bbh) return;
    if (!(m->_34 == 1.0f && m->_44 == 0.0f)) return;   /* not perspective */
    double aspect = (double)bbw / (double)bbh;

    if (g_fovcorrect && fabs((double)m->_22) < 0.01) {
        /* collapsed vertical scale -> rebuild from the viewport aspect */
        m->_22 = (float)((double)m->_11 * aspect);
    }
    if (g_fovfactor != 1.0f) {
        m->_11 = (float)((double)m->_11 / g_fovfactor);
        m->_22 = (float)((double)m->_22 / g_fovfactor);
    }
}

/* High-resolution waitable timer for the limiter's bulk wait; sub-millisecond
 * on Windows 10 1803+. Falls back to a plain timer, then to Sleep(). */
static HANDLE g_hrtimer;
static int g_limiter_init;

static void limiter_init(void)
{
    if (g_limiter_init) return;
    g_limiter_init = 1;
    /* Raise the OS timer resolution so any Sleep()/wait is accurate to ~1ms
     * instead of rounding to the ~15.6ms default. */
    timeBeginPeriod(1);
    g_hrtimer = CreateWaitableTimerExW(NULL, NULL,
        CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!g_hrtimer)                     /* older Windows: no high-res flag */
        g_hrtimer = CreateWaitableTimerExW(NULL, NULL, 0, TIMER_ALL_ACCESS);
}

/* Wait as precisely as possible until QPC reaches `deadline`. Bulk-wait with
 * the waitable timer down to ~0.5ms, then busy-spin the final stretch with
 * PAUSE so we hit the deadline tightly without burning a whole core. */
static void wait_until(LONGLONG deadline, LONGLONG freq)
{
    for (;;) {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        LONGLONG remain = deadline - now.QuadPart;
        if (remain <= 0) return;
        double ms = (double)remain * 1000.0 / (double)freq;
        if (ms > 1.5 && g_hrtimer) {
            LARGE_INTEGER due;         /* negative = relative, 100ns units */
            due.QuadPart = -(LONGLONG)((ms - 1.0) * 10000.0);
            if (SetWaitableTimer(g_hrtimer, &due, 0, NULL, NULL, FALSE))
                WaitForSingleObject(g_hrtimer, (DWORD)ms + 2);
            else
                Sleep((DWORD)(ms - 1.0));
        } else if (ms > 1.5) {
            Sleep((DWORD)(ms - 1.0));
        } else {
            _mm_pause();               /* final <1.5ms: spin */
        }
    }
}

/* Frame limiter. Hitman 2's engine advances its simulation from the measured
 * frame time, so an uncapped modern GPU makes physics, camera and scripted
 * timing run wild. We hold FpsCap once per presented frame.
 *
 * Pacing mode is decided ONCE, then committed: layering our own sleep on top
 * of a hardware vsync cap of the SAME period would slowly drift and beat
 * (periodic doubled/dropped frames — the "60fps but choppy" microstutter). So
 * we calibrate for ~1.5s: pace precisely while measuring how often we actually
 * had to sleep. If Present was already pacing the rate itself (hardware vsync
 * honoured), we almost never sleep -> switch to HW mode and stop sleeping
 * entirely. If not (e.g. a VRR/G-Sync panel strips the swap interval to
 * immediate, as on this rig), we keep pacing precisely in software. Either way
 * there is a single, consistent clock — no two-clock beat.
 *
 * WHERE the software wait happens matters too. Waiting after Present paces
 * the START of the next frame, so each present time inherits that frame's
 * work time (present N+1 = deadline N + work N+1) and frame-to-frame work
 * VARIANCE passes straight into the scanout times. A fixed-Hz vsynced
 * display absorbs that jitter in its vblank grid, but a VRR panel scans out
 * AT the moment of Present — the variance shows as intermittent
 * microstutter. So the committed software limiter waits just BEFORE Present
 * (frame_pace_pre, the on_frame hook): presents land exactly on the QPC
 * grid, the work variance is absorbed by the wait, and the panel sees the
 * even cadence hardware vsync would have given. Calibration still paces
 * after Present, because a blocking (hardware-paced) Present is exactly the
 * signal it is measuring for. */
static void frame_limit(void)      /* on_present: after Present returned */
{
    if (g_fpscap <= 0) return;

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (!g_pace_freq) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        g_pace_freq = f.QuadPart;
        g_pace_next = now.QuadPart;
        limiter_init();
    }
    if (g_pace_freq <= 0) return;

    /* Log the measured rate for the first few seconds so a run confirms the
     * cap engaged, then go quiet. */
    static int samples;
    static LARGE_INTEGER win_start;
    static long frames;
    if (samples < 5) {
        frames++;
        if (win_start.QuadPart == 0)
            win_start = now;
        if (now.QuadPart - win_start.QuadPart >= g_pace_freq) { /* ~1s window */
            logf_("frame limiter: ~%ld fps (cap %d)", frames, g_fpscap);
            samples++;
            frames = 0;
            win_start = now;
        }
    }

    if (g_pace_mode == PACE_SW)         /* paced in frame_pace_pre */
        return;
    if (g_pace_mode == PACE_HW) {       /* display paces; never sleep */
        g_pace_next = now.QuadPart;
        return;
    }

    /* PACE_CAL: pace post-present while noting whether we had to sleep. */
    LONGLONG period = g_pace_freq / g_fpscap;
    g_pace_next += period;
    if (g_pace_next < now.QuadPart)     /* over budget: don't tax the frame */
        g_pace_next = now.QuadPart;
    wait_until(g_pace_next, g_pace_freq);

    LARGE_INTEGER after;
    QueryPerformanceCounter(&after);
    if (after.QuadPart - now.QuadPart > period / 8)   /* slept >~1/8 frame */
        g_cal_slept++;
    if (++g_cal_frames >= 90) {
        g_pace_mode = (g_cal_slept <= 5) ? PACE_HW : PACE_SW;
        logf_("frame pacing: %s (slept %d/%d calibration frames, cap %d)",
              g_pace_mode == PACE_HW
                  ? "display/vsync — software limiter OFF"
                  : "precise software limiter, presents aligned to the grid",
              g_cal_slept, g_cal_frames, g_fpscap);
    }
}

/* on_frame: runs just before Present forwards to the real implementation.
 * In committed-software mode the pacing wait happens HERE, so the present
 * itself lands on the grid (see the frame_limit comment above). */
static void frame_pace_pre(IDirect3DDevice8 *dev)
{
    (void)dev;
    if (g_fpscap <= 0 || g_pace_mode != PACE_SW || g_pace_freq <= 0)
        return;

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    g_pace_next += g_pace_freq / g_fpscap;
    if (g_pace_next < now.QuadPart) {
        /* Frame work overran the cap period — the deadline already passed.
         * Present immediately and restart the schedule from now. Waiting
         * until now+period here would tax every over-budget frame a full
         * extra period (17ms of work -> 33.7ms frame), cliffing heavy
         * scenes (gunfights, crowds) from 60 straight to ~30fps. */
        g_pace_next = now.QuadPart;
    }
    wait_until(g_pace_next, g_pace_freq);
}

static const H2SAD3D8Hooks g_hooks = {
    H2SA_D3D8_HOOKS_VERSION,
    fix_present,
    fix_projection,
    NULL,           /* on_device */
    frame_limit,    /* on_present: calibration / HW bookkeeping */
    frame_pace_pre, /* on_frame: committed SW limiter waits before Present */
};

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        g_inst = inst;
        DisableThreadLibraryCalls(inst);
        GetModuleFileNameA(inst, g_dir, sizeof(g_dir));
        char *sl = strrchr(g_dir, '\\');
        if (sl) *sl = 0;
        char logpath[MAX_PATH];
        snprintf(logpath, sizeof(logpath), "%s\\H2SAWidescreen.log", g_dir);
        g_log = fopen(logpath, "w");
        read_config();
        read_game_resolution();
        /* Disable the renderer's resolution-snap ladder before it runs, so
         * the Hitman2.ini resolution is used verbatim. RenderD3D is already
         * mapped (it is why our loader/this plugin were loaded), so this
         * usually succeeds synchronously; the watchdog is a fallback. */
        if (g_enabled && !patch_renderer_snap())
            CreateThread(NULL, 0, snap_watch, NULL, 0, NULL);
        else
            g_snap_done = 1;
        if (g_enabled)
            CreateThread(NULL, 0, cursor_watch, NULL, 0, NULL);
        logf_("H2SA Widescreen loaded%s, Fullscreen=%d Borderless=%d "
              "PreserveAspect=%d FOVCorrect=%d FOVFactor=%.2f FpsCap=%d "
              "VSync=%d MouseClipFix=%d MouseMotionFix=%d, Hitman2.ini "
              "resolution %dx%d",
              g_enabled ? "" : " (disabled)",
              g_fullscreen, g_borderless, g_preserveaspect, g_fovcorrect,
              (double)g_fovfactor, g_fpscap, g_vsync, g_mouseclipfix,
              g_mousemotionfix, g_ini_w, g_ini_h);

        HMODULE loader = GetModuleHandleA("d3d8.dll");
        h2sa_register_fn reg = loader ? (h2sa_register_fn)(uintptr_t)
            GetProcAddress(loader, "H2SA_RegisterD3D8Hooks") : NULL;
        if (reg) {
            reg(&g_hooks);
            logf_("registered D3D8 hooks with the loader");
        } else {
            logf_("d3d8.dll loader / H2SA_RegisterD3D8Hooks not found — "
                  "is the bundled d3d8.dll installed and overridden?");
        }
    }
    return TRUE;
}

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
 * Exclusive fullscreen is available too (Fullscreen=1), but only works at a
 * resolution the display actually enumerates. Under CrossOver that list is
 * the Mac's Retina-scaled (16:10) modes — 1280x800, 1920x1200, 2560x1600,
 * ... — and none of the classic game resolutions (800x600, 1280x1024,
 * 1920x1080) are in it, which is exactly why the stock game's fullscreen
 * device fails. Set Resolution to one of the enumerated modes for true
 * fullscreen; at any other resolution Fullscreen falls back to borderless.
 *
 * Config: scripts/H2SAWidescreen.ini
 *   [Widescreen]
 *   Enabled=1
 *   Fullscreen=0    ; 1 = exclusive fullscreen when Resolution is a real
 *                   ; display mode, else borderless (0 = always windowed)
 *   Borderless=-1   ; when not fullscreen: -1 auto (borderless, filling the
 *                   ; desktop, under Wine), 0 plain window, 1 always borderless
 *   FOVCorrect=1    ; Hor+ projection correction on/off
 *   FOVFactor=1.0   ; extra horizontal FOV multiplier (>1 = wider)
 *   CursorFix=-1    ; hide the host (Mac) cursor over the game: -1 auto
 *                   ; (on under Wine), 0 off, 1 on
 */
#include <d3d8.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include "h2sa_d3d8.h"

static FILE *g_log;
static char g_dir[MAX_PATH];
static int g_enabled = 1;
static int g_borderless = -1;      /* -1 auto (Wine), 0 never, 1 always */
static int g_fullscreen = 0;       /* 1 = exclusive fullscreen (see below) */
static int g_fovcorrect = 1;
static float g_fovfactor = 1.0f;
static int g_borderless_active;    /* a fullscreen request was converted */
static int g_ini_w, g_ini_h;       /* Resolution WxH parsed from Hitman2.ini */
static int g_cursorfix = -1;       /* -1 auto (Wine), 0 off, 1 on */
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
        else if (sscanf(line, " FOVCorrect = %d", &b) == 1 ||
                 sscanf(line, " FOVCorrect=%d", &b) == 1)
            g_fovcorrect = b;
        else if (sscanf(line, " FOVFactor = %f", &v) == 1 ||
                 sscanf(line, " FOVFactor=%f", &v) == 1)
            g_fovfactor = v;
    }
    fclose(f);
    if (!(g_fovfactor >= 0.5f && g_fovfactor <= 2.0f)) g_fovfactor = 1.0f;
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

static int borderless_wanted(void)
{
    return g_borderless == 1 || (g_borderless == -1 && is_wine());
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

    /* Exclusive fullscreen: only viable if the requested resolution is an
     * actual enumerated display mode (otherwise CreateDevice returns
     * D3DERR_NOTAVAILABLE). We do NOT clamp to a different mode, because the
     * engine would keep laying out for the ini resolution and mismatch the
     * device. So: honour fullscreen when the resolution is a real mode,
     * otherwise fall through to the always-works windowed path and say why. */
    if (g_fullscreen && is_display_mode((int)pp->BackBufferWidth,
                                        (int)pp->BackBufferHeight)) {
        pp->Windowed = FALSE;
        pp->BackBufferFormat = D3DFMT_X8R8G8B8;
        pp->FullScreen_RefreshRateInHz = 0;   /* default refresh */
        pp->FullScreen_PresentationInterval = D3DPRESENT_INTERVAL_DEFAULT;
        if (pp->SwapEffect == D3DSWAPEFFECT_COPY)
            pp->SwapEffect = D3DSWAPEFFECT_DISCARD;
        g_borderless_active = 0;
        logf_("present fixup (%s): exclusive fullscreen %ux%u (valid mode)",
              is_reset ? "reset" : "create",
              pp->BackBufferWidth, pp->BackBufferHeight);
        return;
    }
    if (g_fullscreen)
        logf_("Fullscreen=1 but %ux%u is not an enumerated display mode — "
              "using borderless windowed instead (set Resolution to a mode "
              "your display exposes for true fullscreen)",
              pp->BackBufferWidth, pp->BackBufferHeight);

    /* Windowed path: this is what makes CreateDevice succeed on the CrossOver
     * D3DMetal stack (the stock exclusive-fullscreen device fails with
     * "Unable to create device"). Windowed uses the desktop format (UNKNOWN =
     * match desktop), so there is no fullscreen mode / colour-depth match to
     * fail. */
    pp->Windowed = TRUE;
    pp->BackBufferFormat = D3DFMT_UNKNOWN;
    pp->FullScreen_RefreshRateInHz = 0;
    pp->FullScreen_PresentationInterval = D3DPRESENT_INTERVAL_DEFAULT;
    if (pp->SwapEffect == D3DSWAPEFFECT_FLIP)
        pp->SwapEffect = D3DSWAPEFFECT_DISCARD;  /* FLIP is fullscreen-only */

    if ((was_fullscreen || g_fullscreen) && borderless_wanted())
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
    if (g_borderless_active && dw >= 320 && dh >= 200 && hwnd) {
        /* Keep the backbuffer at the Hitman2.ini resolution (now honoured
         * end-to-end thanks to the snap patch) and stretch it across a
         * full-desktop borderless window. Set Resolution to your display
         * resolution for a pixel-exact, unstretched fill.
         *
         * The window is oversized by one pixel on the right and bottom: the
         * Mac driver treats a pointer on a window frame's exact bottom/right
         * boundary as OUTSIDE the window (Cocoa's NSMouseInRect excludes
         * those two edges), and for a point over "no Wine window" it resets
         * to the host arrow cursor and unhides it — so a cursor pinned at
         * the bottom of the screen flashed the macOS arrow in the menus.
         * With the frame 1px past the screen edge the pinned cursor stays
         * strictly inside. The extra pixel is off-screen and the stretch
         * difference is imperceptible. */
        set_borderless_window(hwnd, 0, 0, dw + 1, dh + 1);
    }

    logf_("present fixup (%s): %s -> windowed %ux%u%s",
          is_reset ? "reset" : "create",
          was_fullscreen ? "fullscreen" : "windowed",
          pp->BackBufferWidth, pp->BackBufferHeight,
          g_borderless_active ? " (borderless, filling desktop)" : "");
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

static const H2SAD3D8Hooks g_hooks = {
    H2SA_D3D8_HOOKS_VERSION,
    fix_present,
    fix_projection,
    NULL,
};

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
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
              "FOVCorrect=%d FOVFactor=%.2f, Hitman2.ini resolution %dx%d",
              g_enabled ? "" : " (disabled)", g_fullscreen, g_borderless,
              g_fovcorrect, (double)g_fovfactor, g_ini_w, g_ini_h);

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

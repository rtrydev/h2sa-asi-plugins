/* H2SA ASI Loader — a d3d8.dll proxy that loads every .asi from the
 * scripts directory and gives those plugins a stable hook onto the game's
 * Direct3D 8 interface (32-bit, built with mingw like the plugins).
 *
 * Why d3d8.dll (and not dsound.dll as in the Codename 47 build): Hitman 2:
 * Silent Assassin is a Direct3D 8 title. hitman2.exe imports only
 * kernel32 and loads its renderer (RenderD3D.dll) at run time; RenderD3D
 * imports exactly one symbol from d3d8.dll — Direct3DCreate8. A d3d8.dll
 * proxy in the game directory is therefore pulled in the moment the
 * renderer initializes, before it ever creates a device, which is the
 * ideal place to intervene.
 *
 * On process attach (which for this proxy means "when the renderer loads")
 * it:
 *  - loads every *.asi from the scripts/ directory next to it, logging to
 *    scripts/H2SAAsiLoader.log;
 *  - exports all five real d3d8.dll entry points at their real ordinals
 *    (see d3d8.def) and forwards four of them to the system d3d8.dll,
 *    resolved lazily on first call;
 *  - wraps the fifth, Direct3DCreate8, so the returned IDirect3D8 —and the
 *    IDirect3DDevice8 it later creates— have their CreateDevice / Reset /
 *    SetTransform vtable slots redirected to this module. Plugins that
 *    registered via H2SA_RegisterD3D8Hooks then get to rewrite the
 *    presentation parameters and the projection matrix. The D3D8 vtable
 *    layout is a fixed COM ABI, so this needs no game-build offsets.
 *
 * Under Wine/CrossOver the bottle needs the DLL override
 * "d3d8=native,builtin" so the game-directory proxy is picked over the
 * builtin d3d8 (which itself is what we forward to).
 */
#include <d3d8.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "h2sa_d3d8.h"

/* Confirmed IDirect3D8 / IDirect3DDevice8 vtable slot indices (fixed D3D8
 * COM ABI, verified against the mingw d3d8.h vtable structs). */
#define IDX_D3D8_CREATEDEVICE   15
#define IDX_DEV_RESET           14
#define IDX_DEV_PRESENT         15
#define IDX_DEV_SETTRANSFORM    37
#define IDX_DEV_SETVIEWPORT     40

static FILE *g_log;
static char g_dir[MAX_PATH];        /* game directory (location of this dll) */
static HINSTANCE g_self;
static HMODULE g_real;              /* system d3d8.dll, resolved lazily */

#define MAX_HOOKSETS 8
static H2SAD3D8Hooks g_hooks[MAX_HOOKSETS];  /* one per registered plugin */
static int g_n_hooks;               /* number of registered hook sets */
static void *g_orig_createdevice;
static void *g_orig_reset;
static void *g_orig_present;
static void *g_orig_settransform;
static void *g_orig_setviewport;
static unsigned int g_bbw, g_bbh;   /* backbuffer size of the live device */
static int g_vp_logs, g_proj_logs;  /* diagnostic sample counters */

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

static FARPROC resolve(const char *name)
{
    if (!g_real) {
        char path[MAX_PATH];
        UINT n = GetSystemDirectoryA(path, MAX_PATH - 12);
        if (n == 0 || n >= MAX_PATH - 12) return NULL;
        strcat(path, "\\d3d8.dll");
        HMODULE h = LoadLibraryA(path);
        if (!h || h == (HMODULE)g_self) {
            logf_("system d3d8.dll unavailable (h=%p self=%p) — proxy "
                  "exports will fail", h, g_self);
            g_real = (HMODULE)(uintptr_t)-1;
        } else {
            g_real = h;
            logf_("forwarding to system d3d8.dll at %p", h);
        }
    }
    if (g_real == (HMODULE)(uintptr_t)-1) return NULL;
    FARPROC f = GetProcAddress(g_real, name);
    if (!f) logf_("system d3d8.dll lacks %s", name);
    return f;
}

/* Overwrite one COM vtable slot in place. There is a single IDirect3D8 and
 * a single device for the process, so patching the shared vtable slot is
 * fine; the original is saved on first patch. */
static void patch_slot(void *iface, int idx, void *newfn, void **saved)
{
    void **vtbl = *(void ***)iface;
    if (!*saved) *saved = vtbl[idx];
    DWORD old;
    if (VirtualProtect(&vtbl[idx], sizeof(void *), PAGE_READWRITE, &old)) {
        vtbl[idx] = newfn;
        VirtualProtect(&vtbl[idx], sizeof(void *), old, &old);
    }
}

/* ---- wrapped D3D8 methods -------------------------------------------- */

static HRESULT WINAPI hook_SetViewport(IDirect3DDevice8 *self,
    CONST D3DVIEWPORT8 *vp)
{
    typedef HRESULT (WINAPI *vp_t)(IDirect3DDevice8 *, CONST D3DVIEWPORT8 *);
    /* Hitman 2 computes a garbage viewport height for resolutions its engine
     * does not recognise (e.g. 1920-wide comes through as H=81528992), which
     * maps the whole scene to a one-pixel sliver. A viewport must fit inside
     * the render target anyway, so any viewport spilling past the backbuffer
     * is clamped to the full backbuffer — that restores correct rendering
     * without touching game code. Legitimate sub-viewports (HUD elements etc.)
     * fit and pass through untouched. */
    D3DVIEWPORT8 fixed;
    if (vp && g_bbw && g_bbh &&
        (vp->X + vp->Width > g_bbw || vp->Y + vp->Height > g_bbh ||
         vp->Width > g_bbw || vp->Height > g_bbh)) {
        if (g_vp_logs < 4) {
            void *ret = __builtin_return_address(0);
            void *h2 = (void *)GetModuleHandleA("hitman2.exe");
            void *rd = (void *)GetModuleHandleA("RenderD3D.dll");
            logf_("SetViewport caller ret=%p (hitman2=%p RenderD3D=%p)",
                  ret, h2, rd);
        }
        fixed = *vp;
        fixed.X = 0; fixed.Y = 0;
        fixed.Width = g_bbw; fixed.Height = g_bbh;
        if (g_vp_logs < 16) {
            g_vp_logs++;
            logf_("SetViewport %lux%lu at %lu,%lu out of range -> clamped to "
                  "%ux%u", (unsigned long)vp->Width, (unsigned long)vp->Height,
                  (unsigned long)vp->X, (unsigned long)vp->Y, g_bbw, g_bbh);
        }
        return ((vp_t)g_orig_setviewport)(self, &fixed);
    }
    if (vp && g_vp_logs < 16) {
        g_vp_logs++;
        logf_("SetViewport X=%lu Y=%lu W=%lu H=%lu (ok)",
              (unsigned long)vp->X, (unsigned long)vp->Y,
              (unsigned long)vp->Width, (unsigned long)vp->Height);
    }
    return ((vp_t)g_orig_setviewport)(self, vp);
}

static HRESULT WINAPI hook_SetTransform(IDirect3DDevice8 *self,
    D3DTRANSFORMSTATETYPE State, CONST D3DMATRIX *pMatrix)
{
    typedef HRESULT (WINAPI *st_t)(IDirect3DDevice8 *,
        D3DTRANSFORMSTATETYPE, CONST D3DMATRIX *);
    if (State == D3DTS_PROJECTION && pMatrix && g_proj_logs < 24) {
        g_proj_logs++;
        logf_("PROJ in: _11=%.6g _22=%.6g _33=%.6g _34=%.3g _43=%.6g "
              "_44=%.3g  (bb %ux%u)", pMatrix->_11, pMatrix->_22,
              pMatrix->_33, pMatrix->_34, pMatrix->_43, pMatrix->_44,
              g_bbw, g_bbh);
    }
    if (State == D3DTS_PROJECTION && pMatrix) {
        D3DMATRIX m = *pMatrix;
        int any = 0;
        for (int i = 0; i < g_n_hooks; i++)
            if (g_hooks[i].fix_projection) {
                g_hooks[i].fix_projection(&m, g_bbw, g_bbh);
                any = 1;
            }
        if (any)
            return ((st_t)g_orig_settransform)(self, State, &m);
    }
    return ((st_t)g_orig_settransform)(self, State, pMatrix);
}

static HRESULT WINAPI hook_Present(IDirect3DDevice8 *self,
    CONST RECT *src, CONST RECT *dst, HWND override, CONST RGNDATA *dirty)
{
    typedef HRESULT (WINAPI *pr_t)(IDirect3DDevice8 *, CONST RECT *,
        CONST RECT *, HWND, CONST RGNDATA *);
    /* Before the frame is shown: let overlays draw onto the finished back
     * buffer (drawing in on_present, below, would land after Present). */
    for (int i = 0; i < g_n_hooks; i++)
        if (g_hooks[i].on_frame)
            g_hooks[i].on_frame(self);
    HRESULT hr = ((pr_t)g_orig_present)(self, src, dst, override, dirty);
    /* One displayed frame just finished; let plugins pace the frame rate
     * (the engine's simulation is frame-time bound). */
    for (int i = 0; i < g_n_hooks; i++)
        if (g_hooks[i].on_present)
            g_hooks[i].on_present();
    return hr;
}

static HRESULT WINAPI hook_Reset(IDirect3DDevice8 *self,
    D3DPRESENT_PARAMETERS *pp)
{
    typedef HRESULT (WINAPI *rs_t)(IDirect3DDevice8 *, D3DPRESENT_PARAMETERS *);
    if (pp)
        for (int i = 0; i < g_n_hooks; i++)
            if (g_hooks[i].fix_present)
                g_hooks[i].fix_present(pp, NULL, 1);
    if (pp) { g_bbw = pp->BackBufferWidth; g_bbh = pp->BackBufferHeight; }
    return ((rs_t)g_orig_reset)(self, pp);
}

static HRESULT WINAPI hook_CreateDevice(IDirect3D8 *self, UINT Adapter,
    D3DDEVTYPE DeviceType, HWND hFocusWindow, DWORD BehaviorFlags,
    D3DPRESENT_PARAMETERS *pp, IDirect3DDevice8 **ppReturnedDeviceInterface)
{
    typedef HRESULT (WINAPI *cd_t)(IDirect3D8 *, UINT, D3DDEVTYPE, HWND,
        DWORD, D3DPRESENT_PARAMETERS *, IDirect3DDevice8 **);
    if (pp)
        logf_("CreateDevice requested: %ux%u fmt=%d windowed=%d swap=%d",
              pp->BackBufferWidth, pp->BackBufferHeight, pp->BackBufferFormat,
              pp->Windowed, pp->SwapEffect);
    if (pp)
        for (int i = 0; i < g_n_hooks; i++)
            if (g_hooks[i].fix_present)
                g_hooks[i].fix_present(pp, hFocusWindow, 0);
    if (pp)
        logf_("CreateDevice applied:   %ux%u fmt=%d windowed=%d",
              pp->BackBufferWidth, pp->BackBufferHeight, pp->BackBufferFormat,
              pp->Windowed);

    HRESULT hr = ((cd_t)g_orig_createdevice)(self, Adapter, DeviceType,
        hFocusWindow, BehaviorFlags, pp, ppReturnedDeviceInterface);
    logf_("CreateDevice returned 0x%08lx", (unsigned long)hr);

    /* Reliability: an exclusive-fullscreen CreateDevice can fail transiently
     * with D3DERR_NOTAVAILABLE / D3DERR_DEVICELOST during a display-state race
     * (VRR/G-Sync renegotiation, an HDR toggle, the Steam overlay or another
     * app briefly owning the display) — the "this render mode is not available"
     * crash that comes and goes. Retry the same request a few times with a
     * short delay before giving up, turning a flaky start into a reliable one.
     * Windowed requests always create, so this only guards the fullscreen path. */
    if (pp && !pp->Windowed) {
        for (int tries = 0; FAILED(hr) && tries < 5 &&
             (hr == D3DERR_NOTAVAILABLE || hr == D3DERR_DEVICELOST); tries++) {
            logf_("fullscreen CreateDevice failed 0x%08lx — retry %d after 80ms",
                  (unsigned long)hr, tries + 1);
            Sleep(80);
            hr = ((cd_t)g_orig_createdevice)(self, Adapter, DeviceType,
                hFocusWindow, BehaviorFlags, pp, ppReturnedDeviceInterface);
            logf_("fullscreen CreateDevice retry -> 0x%08lx", (unsigned long)hr);
        }
    }

    if (SUCCEEDED(hr) && ppReturnedDeviceInterface &&
        *ppReturnedDeviceInterface) {
        if (pp) { g_bbw = pp->BackBufferWidth; g_bbh = pp->BackBufferHeight; }
        patch_slot(*ppReturnedDeviceInterface, IDX_DEV_RESET,
                   (void *)hook_Reset, &g_orig_reset);
        patch_slot(*ppReturnedDeviceInterface, IDX_DEV_PRESENT,
                   (void *)hook_Present, &g_orig_present);
        patch_slot(*ppReturnedDeviceInterface, IDX_DEV_SETTRANSFORM,
                   (void *)hook_SetTransform, &g_orig_settransform);
        patch_slot(*ppReturnedDeviceInterface, IDX_DEV_SETVIEWPORT,
                   (void *)hook_SetViewport, &g_orig_setviewport);
        for (int i = 0; i < g_n_hooks; i++)
            if (g_hooks[i].on_device)
                g_hooks[i].on_device(*ppReturnedDeviceInterface);
        logf_("device %p wrapped (Reset+Present+SetTransform+SetViewport)",
              *ppReturnedDeviceInterface);
    }
    return hr;
}

/* ---- exports --------------------------------------------------------- */

__declspec(dllexport) IDirect3D8 * WINAPI Direct3DCreate8(UINT SDKVersion)
{
    typedef IDirect3D8 * (WINAPI *dc_t)(UINT);
    dc_t real = (dc_t)(uintptr_t)resolve("Direct3DCreate8");
    if (!real) return NULL;
    IDirect3D8 *d3d = real(SDKVersion);
    if (d3d) {
        patch_slot(d3d, IDX_D3D8_CREATEDEVICE, (void *)hook_CreateDevice,
                   &g_orig_createdevice);
        logf_("Direct3DCreate8(%u) -> %p; CreateDevice wrapped", SDKVersion,
              d3d);
    } else {
        logf_("real Direct3DCreate8 returned NULL");
    }
    return d3d;
}

/* The remaining four exports are forwarded verbatim; the game never calls
 * them, but a correct drop-in must provide them at the right ordinals. */
__declspec(dllexport) HRESULT WINAPI ValidatePixelShader(
    DWORD *a, DWORD *b, BOOL c, char **d)
{
    HRESULT (WINAPI *f)(DWORD *, DWORD *, BOOL, char **) =
        (void *)(uintptr_t)resolve("ValidatePixelShader");
    return f ? f(a, b, c, d) : E_FAIL;
}

__declspec(dllexport) HRESULT WINAPI ValidateVertexShader(
    DWORD *a, DWORD *b, DWORD *c, BOOL d, char **e)
{
    HRESULT (WINAPI *f)(DWORD *, DWORD *, DWORD *, BOOL, char **) =
        (void *)(uintptr_t)resolve("ValidateVertexShader");
    return f ? f(a, b, c, d, e) : E_FAIL;
}

__declspec(dllexport) void WINAPI DebugSetMute(void)
{
    void (WINAPI *f)(void) = (void *)(uintptr_t)resolve("DebugSetMute");
    if (f) f();
}

__declspec(dllexport) DWORD WINAPI D3D8GetSWInfo(void)
{
    DWORD (WINAPI *f)(void) = (void *)(uintptr_t)resolve("D3D8GetSWInfo");
    return f ? f() : 0;
}

/* Registration entry point used by the ASI plugins. */
__declspec(dllexport) void WINAPI H2SA_RegisterD3D8Hooks(
    const H2SAD3D8Hooks *hooks)
{
    if (!hooks || hooks->version != H2SA_D3D8_HOOKS_VERSION) {
        logf_("H2SA_RegisterD3D8Hooks: version mismatch (%u != %u)",
              hooks ? hooks->version : 0, H2SA_D3D8_HOOKS_VERSION);
        return;
    }
    if (g_n_hooks >= MAX_HOOKSETS) {
        logf_("H2SA_RegisterD3D8Hooks: too many plugins (max %d)", MAX_HOOKSETS);
        return;
    }
    H2SAD3D8Hooks *h = &g_hooks[g_n_hooks++];
    *h = *hooks;
    logf_("plugin #%d registered D3D8 hooks (fix_present=%p fix_projection=%p "
          "on_device=%p on_frame=%p on_present=%p)", g_n_hooks,
          (void *)h->fix_present, (void *)h->fix_projection,
          (void *)h->on_device, (void *)h->on_frame, (void *)h->on_present);
}

/* ---- ASI loading ----------------------------------------------------- */

static void load_asis(void)
{
    char pat[MAX_PATH];
    snprintf(pat, sizeof(pat), "%s\\scripts\\*.asi", g_dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        logf_("no ASI plugins found (%s)", pat);
        return;
    }
    int n = 0, fail = 0;
    do {
        char path[MAX_PATH];
        snprintf(path, sizeof(path), "%s\\scripts\\%s", g_dir, fd.cFileName);
        HMODULE m = LoadLibraryA(path);
        if (m) {
            n++;
            logf_("loaded %s at %p", fd.cFileName, m);
        } else {
            fail++;
            logf_("FAILED to load %s (error %lu)", fd.cFileName,
                  (unsigned long)GetLastError());
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    logf_("%d plugin(s) loaded%s", n, fail ? ", some failed!" : "");
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = inst;
        DisableThreadLibraryCalls(inst);
        GetModuleFileNameA(inst, g_dir, sizeof(g_dir));
        char *sl = strrchr(g_dir, '\\');
        if (sl) *sl = 0;
        char logpath[MAX_PATH];
        snprintf(logpath, sizeof(logpath), "%s\\scripts\\H2SAAsiLoader.log",
                 g_dir);
        g_log = fopen(logpath, "w");
        logf_("H2SA ASI Loader (d3d8.dll proxy) attached");
        load_asis();
    }
    return TRUE;
}

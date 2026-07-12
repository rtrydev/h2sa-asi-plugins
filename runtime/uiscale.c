/* h2sa_core.asi (UI-scale half) — native HUD/menu scaling for
 * Hitman 2: Silent Assassin.
 *
 * Problem: the engine lays its whole 2D layer (HUD glyph quads, menu
 * pointer, viewport rectangles, mouse mapping) out in PIXELS against the
 * Hitman2.ini `Resolution` — 1 layout unit = 1 believed pixel (the HUD
 * world scale is 2*z/(proj._22*believed_height) with z=6, i.e. exactly
 * that). At a native-resolution backbuffer on a modern display the HUD and
 * menus are therefore tiny.
 *
 * Approaches considered (see the sibling hc47-asi-plugins HudScale for the
 * Codename 47 solution this parallels):
 *
 *  - C47 writes a smaller "virtual" size into the engine's believed-
 *    resolution fields (ZSysInterface) after mode-set. H2 has no reachable
 *    equivalent: hitman2.exe is packed (no static analysis, no stable
 *    offsets) and RenderD3D snapshots the resolution at init. Rejected.
 *  - Rescaling the HUD via SetTransform is impossible: the glyph quads
 *    arrive CPU-pre-transformed with an identity WORLD set right before the
 *    draw. Patching the draw site would fix sizes but not the per-element
 *    anchor positions, which are computed in the packed exe. Rejected.
 *  - RenderD3D's `r_font_size` cvar scales glyphs only — anchors and icons
 *    stay laid out for the believed resolution, so text overlaps. Rejected.
 *  - BELIEVED-RESOLUTION DECOUPLING (this file): leave the engine believing
 *    the Hitman2.ini resolution — that becomes the UI LAYOUT resolution —
 *    and render into a LARGER backbuffer. The engine emits everything in
 *    normalized device coordinates derived from the believed size, so every
 *    element lands at the right place and relative size on the bigger
 *    target automatically; only the viewports the engine sets (in believed
 *    pixels) must be rescaled (the fix_viewport hook below). The 3D scene
 *    gains real resolution; the UI keeps its believed-resolution size.
 *
 * Text sharpness: unlike Codename 47 — whose SharpText re-rasterizes
 * TrueType OUTLINES at the real pixel size via the FreeType build embedded
 * in the game, a true quality gain at zero cost — H2 ships its text as
 * pre-baked bitmap font atlases and has no runtime rasterizer. A texture-
 * substitution pass that refiltered those bitmaps at bind time existed
 * here (UISharpen, see git history) and was removed: refiltering existing
 * pixels gained almost nothing visually while the render-thread builds
 * cost visible frame drops after every scene load. The UI textures are
 * simply magnified by the GPU's bilinear filter. Genuinely crisp text
 * would require rebuilding the COPRGTB font atlases from the TTF at scale
 * and patching the per-glyph UV tables in the GUI resources — a possible
 * future project.
 *
 * Config lives in [Widescreen] (widescreen.c parses and forwards it):
 *   UIScale=-1   ; -1 auto: backbuffer = the display's native (Retina)
 *                ; pixels for the letterboxed image — 1:1 present, max
 *                ; sharpness; UI sized as at the Hitman2.ini resolution.
 *                ; N>1: Hitman2.ini Resolution is the RENDER resolution and
 *                ; the UI is laid out N x bigger (the engine is re-believed
 *                ; to Resolution/N — see h2sa_uiscale_rebelieve below; works
 *                ; in exclusive fullscreen too, since the backbuffer stays
 *                ; the enumerated ini mode). If the re-believe scan finds
 *                ; nothing, falls back to the old meaning: backbuffer =
 *                ; N x Hitman2.ini, UI size unchanged (supersampling).
 *                ; 0/1: off (backbuffer = Hitman2.ini resolution as before).
 */
#include <d3d8.h>
#include <stdio.h>
#include <stdint.h>
#include <math.h>
#include "h2sa_d3d8.h"
#include "h2sa_core.h"

#define logf_ h2sa_core_logf

/* config (set by widescreen.c's parser before the device exists) */
static float g_cfg_uiscale = 0.0f;    /* 0/1 off, -1 auto, >1 explicit */

/* live state (set at CreateDevice/Reset via h2sa_uiscale_setup) */
static int      g_active;
static int      g_ini_w, g_ini_h;     /* believed (layout) resolution */
static unsigned g_bb_w, g_bb_h;       /* real backbuffer */
static double   g_kx = 1.0, g_ky = 1.0;
static int      g_vp_logs;

void h2sa_uiscale_config(float uiscale)
{
    g_cfg_uiscale = uiscale;
}

float h2sa_uiscale_cfg(void) { return g_cfg_uiscale; }

int h2sa_uiscale_wanted(void)
{
    return g_cfg_uiscale < 0.0f || g_cfg_uiscale > 1.0f;
}

/* ky for overlays drawing in real backbuffer pixels (profiler). */
float h2sa_uiscale_k(void)
{
    return g_active ? (float)g_ky : 1.0f;
}

static void publish_to_loader(float k)
{
    static void (WINAPI *set)(float);
    if (!set) {
        HMODULE ld = GetModuleHandleA("d3d8.dll");
        if (ld)
            set = (void (WINAPI *)(float))(uintptr_t)
                GetProcAddress(ld, "H2SA_SetUIScale");
    }
    if (set) set(k);
}

void h2sa_uiscale_off(void)
{
    if (g_active) logf_("uiscale: off");
    g_active = 0;
    publish_to_loader(1.0f);
}

/* Called from widescreen.c's fix_present once the backbuffer size is
 * decided. believed = Hitman2.ini resolution, bb = real backbuffer. */
void h2sa_uiscale_setup(int ini_w, int ini_h, unsigned bb_w, unsigned bb_h)
{
    if (ini_w < 320 || ini_h < 200 || bb_w < 320 || bb_h < 200) {
        h2sa_uiscale_off();
        return;
    }
    g_ini_w = ini_w; g_ini_h = ini_h;
    g_bb_w = bb_w;   g_bb_h = bb_h;
    g_kx = (double)bb_w / (double)ini_w;
    g_ky = (double)bb_h / (double)ini_h;
    g_active = (g_kx > 1.02 || g_ky > 1.02);
    if (!g_active) {
        h2sa_uiscale_off();
        return;
    }
    g_vp_logs = 0;
    publish_to_loader((float)g_ky);
    logf_("uiscale: active — layout %dx%d -> backbuffer %ux%u (k=%.4f/%.4f)",
          ini_w, ini_h, bb_w, bb_h, g_kx, g_ky);
}

/* ---- re-believe: patch the engine's already-parsed resolution ---------
 *
 * With UIScale=N (>1) the Hitman2.ini Resolution is the RENDER resolution
 * R and the engine must lay the UI out for L = R/N. The engine parses
 * Hitman2.ini itself at startup — before it loads the renderer, which is
 * what pulls in the d3d8 proxy and this plugin — so swapping the file
 * would always be too late. But at plugin load we are INSIDE the engine's
 * LoadLibrary(RenderD3D.dll) call: the ini is parsed, and nothing has
 * consumed the value yet (no renderer init, no device, no UI layout).
 * The parsed value is findable — an adjacent (width,height) pair equal to
 * R, as ints or floats — in the engine's writable memory; patching every
 * copy to L makes the engine believe L end to end, exactly as if the ini
 * had said L, while the plugin keeps R as the backbuffer (and, in
 * exclusive fullscreen, the display mode: R is an enumerated mode, so the
 * scaling works there too, which plain backbuffer-growing could not).
 *
 * Scanned: hitman2.exe's image (the packer unpacks in place) plus all
 * committed private RW memory (packer/heap allocations, other threads'
 * stacks — a live (R_w,R_h) pair anywhere IS the parsed resolution; the
 * engine main thread is parked inside LoadLibrary while we run). Our own
 * module's globals are MEM_IMAGE of a different module and the region
 * holding our stack is skipped, so the plugin's copies of R never
 * self-match. Returns the number of sites patched; 0 = not found (the
 * caller falls back to plain supersampling). */
int h2sa_uiscale_rebelieve(int rw, int rh, int lw, int lh)
{
    const uint32_t ow = (uint32_t)rw, oh = (uint32_t)rh;
    const uint32_t nw = (uint32_t)lw, nh = (uint32_t)lh;
    const float owf = (float)rw, ohf = (float)rh;
    const float nwf = (float)lw, nhf = (float)lh;
    uint8_t *h2 = (uint8_t *)GetModuleHandleA("hitman2.exe");
    MEMORY_BASIC_INFORMATION mbi;
    uint8_t *p = (uint8_t *)0x10000;
    int ints = 0, floats = 0;
    while ((uintptr_t)p < 0x7fff0000u &&
           VirtualQuery(p, &mbi, sizeof(mbi)) == sizeof(mbi)) {
        uint8_t *base = (uint8_t *)mbi.BaseAddress;
        SIZE_T   size = mbi.RegionSize;
        int writable = mbi.State == MEM_COMMIT &&
            !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
            (mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY |
                            PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY));
        int engine_img = h2 && (uint8_t *)mbi.AllocationBase == h2;
        int priv = mbi.Type == MEM_PRIVATE;
        int our_stack = (uint8_t *)&mbi >= base && (uint8_t *)&mbi < base + size;
        if (writable && (engine_img || priv) && !our_stack) {
            for (uint8_t *q = base; q + 2 * sizeof(uint32_t) <= base + size;
                 q += sizeof(uint32_t)) {
                uint32_t *ip = (uint32_t *)q;
                float    *fp = (float *)q;
                if (ip[0] == ow && ip[1] == oh) {
                    ip[0] = nw; ip[1] = nh;
                    if (ints + floats < 8)
                        logf_("uiscale: re-believe int pair at %p%s",
                              q, engine_img ? " (hitman2.exe image)" : "");
                    ints++;
                } else if (fp[0] == owf && fp[1] == ohf) {
                    fp[0] = nwf; fp[1] = nhf;
                    if (ints + floats < 8)
                        logf_("uiscale: re-believe float pair at %p%s",
                              q, engine_img ? " (hitman2.exe image)" : "");
                    floats++;
                }
            }
        }
        p = base + size;
    }
    logf_("uiscale: re-believe %dx%d -> %dx%d: %d int + %d float site(s)",
          rw, rh, lw, lh, ints, floats);
    return ints + floats;
}

/* ---- viewport rescale (fix_viewport hook) -----------------------------
 * The engine sets viewports in believed pixels (full screen, the 3D scene
 * rect, letter strips...). Anything that fits the believed resolution is
 * scaled to the backbuffer; a viewport that doesn't fit is either garbage
 * (the loader's clamp handles it) or already real-sized (left alone). */
void h2sa_uiscale_fix_viewport(D3DVIEWPORT8 *vp, unsigned bbw, unsigned bbh)
{
    (void)bbw; (void)bbh;
    if (!g_active || !vp) return;
    if (vp->X + vp->Width  > (DWORD)g_ini_w + 2 ||
        vp->Y + vp->Height > (DWORD)g_ini_h + 2)
        return;
    DWORD x0 = (DWORD)lround((double)vp->X * g_kx);
    DWORD y0 = (DWORD)lround((double)vp->Y * g_ky);
    DWORD x1 = (DWORD)lround((double)(vp->X + vp->Width)  * g_kx);
    DWORD y1 = (DWORD)lround((double)(vp->Y + vp->Height) * g_ky);
    if (x1 > g_bb_w) x1 = g_bb_w;
    if (y1 > g_bb_h) y1 = g_bb_h;
    if (x1 <= x0 || y1 <= y0) return;
    if (g_vp_logs < 8) {
        g_vp_logs++;
        logf_("uiscale: viewport %lux%lu at %lu,%lu -> %lux%lu at %lu,%lu",
              (unsigned long)vp->Width, (unsigned long)vp->Height,
              (unsigned long)vp->X, (unsigned long)vp->Y,
              (unsigned long)(x1 - x0), (unsigned long)(y1 - y0),
              (unsigned long)x0, (unsigned long)y0);
    }
    vp->X = x0; vp->Y = y0;
    vp->Width = x1 - x0; vp->Height = y1 - y0;
}

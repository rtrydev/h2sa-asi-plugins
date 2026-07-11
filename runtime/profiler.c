/* h2sa_core.asi (profiler half) — on-screen performance overlay (top-right
 * corner). Linked together with widescreen.c into h2sa_core.asi; widescreen.c
 * owns the DllMain and calls h2sa_profiler_init/h2sa_profiler_detach, and the
 * log (scripts/h2sa_core.log) and config (scripts/h2sa_core.ini, [Profiler]
 * section) are shared.
 *
 * Shows, once per frame:
 *   - FPS and frame time (rolling average / peak over a ~0.5s window);
 *   - a CPU-time breakdown from an EIP-sampling thread, split into
 *       X87  = the SSE2-translated RenderD3D blob (H2SAReducedX87)
 *       REND = RenderD3D.dll code NOT translated (mostly leftover x87)
 *       GAME = hitman2.exe
 *       REST = everything else (D3D/Metal, wine, system)
 *
 * The split makes the x87 translation's effect visible directly: time that
 * used to sit in emulated x87 inside RenderD3D shows up under X87 (native
 * SSE2) instead of REND.
 *
 * Drawing uses the same technique as the h2-stats-overlay reference: a
 * built-in 5x7 pixel font rendered as pre-transformed (XYZRHW) colored
 * triangles via DrawPrimitiveUP, wrapped in a full state-block save/restore
 * so the game's own rendering is untouched. It attaches through the H2SA
 * d3d8 loader's on_frame hook (drawn just before Present), so it needs no
 * game byte offsets and coexists with the widescreen code.
 */
#include <windows.h>
#include <tlhelp32.h>
#include <d3d8.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "h2sa_d3d8.h"
#include "h2sa_core.h"

/* shared log + paths live in widescreen.c (see h2sa_core.h) */
#define logf_ h2sa_core_logf

/* ------------------------------------------------------------------ */
/* config ([Profiler] section of the shared h2sa_core.ini)            */
/* ------------------------------------------------------------------ */
static int   g_enabled = 1;
static float g_scale = 1.0f;
static int   g_show_cpu = 1;
static int   g_off_x = 8;      /* inset from the right edge  */
static int   g_off_y = 8;      /* inset from the top edge    */
static FILETIME g_ini_mtime;

static void load_config(void)
{
    FILE *f = fopen(h2sa_core_ini, "r");
    if (!f) {
        f = fopen(h2sa_core_ini, "w");
        if (f) {
            fputs("[Widescreen]\n"
                  "Enabled=1\n"
                  "Fullscreen=0\n"
                  "Borderless=-1\n"
                  "PreserveAspect=1\n"
                  "FOVCorrect=1\n"
                  "FOVFactor=1.0\n"
                  "CursorFix=0\n"
                  "FpsCap=60\n"
                  "VSync=-1\n"
                  "MouseClipFix=-1\n"
                  "MouseMotionFix=-1\n"
                  "\n"
                  "[Profiler]\n"
                  "Enabled=1\n"
                  "Scale=1.0\n"
                  "ShowCPU=1\n"
                  "OffsetX=8\n"
                  "OffsetY=8\n", f);
            fclose(f);
        }
        return;
    }
    char line[128];
    char section[32] = "";
    while (fgets(line, sizeof(line), f)) {
        int b; float v;
        char s[32];
        if (sscanf(line, " [%31[^]]]", s) == 1) {
            lstrcpynA(section, s, sizeof(section));
            continue;
        }
        if (_stricmp(section, "Profiler") != 0)
            continue;      /* only consume our own section's keys */
        if (sscanf(line, " Enabled = %d", &b) == 1 ||
            sscanf(line, " Enabled=%d", &b) == 1) g_enabled = b;
        else if (sscanf(line, " ShowCPU = %d", &b) == 1 ||
                 sscanf(line, " ShowCPU=%d", &b) == 1) g_show_cpu = b;
        else if (sscanf(line, " OffsetX = %d", &b) == 1 ||
                 sscanf(line, " OffsetX=%d", &b) == 1) g_off_x = b;
        else if (sscanf(line, " OffsetY = %d", &b) == 1 ||
                 sscanf(line, " OffsetY=%d", &b) == 1) g_off_y = b;
        else if (sscanf(line, " Scale = %f", &v) == 1 ||
                 sscanf(line, " Scale=%f", &v) == 1) g_scale = v;
    }
    fclose(f);
    if (!(g_scale >= 0.25f && g_scale <= 8.0f)) g_scale = 1.0f;
}

static void reload_config_if_changed(void)
{
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExA(h2sa_core_ini, GetFileExInfoStandard, &fad))
        return;
    if (memcmp(&fad.ftLastWriteTime, &g_ini_mtime, sizeof(FILETIME)) == 0)
        return;
    g_ini_mtime = fad.ftLastWriteTime;
    load_config();
}

/* ------------------------------------------------------------------ */
/* .x87 patch file format (matches tools/translate.py) — used only to    */
/* locate the translated blob so its samples can be attributed to X87.   */
/* ------------------------------------------------------------------ */
#pragma pack(push, 1)
typedef struct {
    char     magic[8];          /* "H2SAX87P" */
    uint32_t version, preferred_base, timedatestamp, size_of_image,
             n_funcs, blob_total;
    char     module[32];
} PatchHeader;
typedef struct { uint32_t rva, blob_off, blob_len, fixup_idx, n_fixups; } FuncRec;
#pragma pack(pop)

/* One record per installed .x87 patch (RenderD3D.dll + hitman2.exe). The
 * first translated function's rva lets us follow the 5-byte jmp hook the
 * loader installs at its entry to find where the SSE2 blob was allocated. */
#define MAX_BLOBS 4
typedef struct {
    char     module[32];    /* module the patch belongs to (from header) */
    uint32_t probe_rva;     /* rva of the first translated function      */
    uint32_t blob_total;
    uint32_t base;          /* 0 until located via the entry hook        */
} Blob;
static Blob g_blobs[MAX_BLOBS];
static int  g_n_blobs;

static void load_blob_desc(void)
{
    char pat[MAX_PATH];
    snprintf(pat, sizeof(pat), "%s\\h2sa_reduced_x87\\*.x87", h2sa_core_dir);
    WIN32_FIND_DATAA fd;
    HANDLE fh = FindFirstFileA(pat, &fd);
    if (fh == INVALID_HANDLE_VALUE) {
        logf_("no .x87 patch file — X87 attribution disabled");
        return;
    }
    do {
        if (g_n_blobs >= MAX_BLOBS) break;
        char path[MAX_PATH];
        snprintf(path, sizeof(path), "%s\\h2sa_reduced_x87\\%s", h2sa_core_dir,
                 fd.cFileName);
        FILE *f = fopen(path, "rb");
        if (!f) continue;
        PatchHeader h;
        FuncRec fr;
        if (fread(&h, sizeof(h), 1, f) == 1 &&
            !memcmp(h.magic, "H2SAX87P", 8) && h.n_funcs &&
            fread(&fr, sizeof(fr), 1, f) == 1) {
            Blob *b = &g_blobs[g_n_blobs++];
            memcpy(b->module, h.module, 32);
            b->module[31] = 0;
            b->probe_rva = fr.rva;
            b->blob_total = h.blob_total;
            logf_("blob desc: %s, %lu KB", b->module,
                  (unsigned long)b->blob_total / 1024);
        }
        fclose(f);
    } while (FindNextFileA(fh, &fd));
    FindClose(fh);
}

/* ------------------------------------------------------------------ */
/* module / blob address ranges                                        */
/* ------------------------------------------------------------------ */
static uint32_t g_rd3d_base, g_rd3d_size;
static uint32_t g_game_base, g_game_size;

static void module_range(const char *name, uint32_t *base, uint32_t *size)
{
    HMODULE m = GetModuleHandleA(name);
    if (!m) { *base = 0; *size = 0; return; }
    uint8_t *b = (uint8_t *)m;
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)b;
    IMAGE_NT_HEADERS32 *nt = (IMAGE_NT_HEADERS32 *)(b + dos->e_lfanew);
    *base = (uint32_t)(uintptr_t)b;
    *size = nt->OptionalHeader.SizeOfImage;
}

/* The first translated function is emitted at blob_off 0, so the target of
 * the 5-byte jmp H2SAReducedX87 installed at its entry IS the blob base.
 * Each patch is resolved against the module NAMED IN ITS HEADER — following
 * hitman2.exe rvas inside RenderD3D reads the wrong (possibly unmapped)
 * bytes and mis-attributes all blob samples to REST. */
static void locate_blobs(void)
{
    for (int i = 0; i < g_n_blobs; i++) {
        Blob *b = &g_blobs[i];
        if (b->base) continue;
        uint32_t mbase, msize;
        module_range(b->module, &mbase, &msize);
        if (!mbase || b->probe_rva + 5 > msize) continue;
        uint8_t *entry = (uint8_t *)(uintptr_t)mbase + b->probe_rva;
        if (entry[0] != 0xE9) continue;     /* not hooked yet */
        b->base = (uint32_t)(uintptr_t)(entry + 5) + *(int32_t *)(entry + 1);
        logf_("%s translated blob located at %08x (+%lu KB)", b->module,
              b->base, (unsigned long)b->blob_total / 1024);
    }
}

/* ------------------------------------------------------------------ */
/* frame timing (published by on_frame, read by the sampler's log)      */
/* ------------------------------------------------------------------ */
static LARGE_INTEGER g_qpf, g_last_qpc, g_win_start;
static int    g_win_frames;
static double g_win_max_ms;
static volatile int   g_fps;
static volatile float g_ms_avg, g_ms_max;

/* ------------------------------------------------------------------ */
/* EIP sampler                                                         */
/* ------------------------------------------------------------------ */
enum { CAT_X87, CAT_REND, CAT_GAME, CAT_REST, N_CAT };
static volatile int g_pct[N_CAT];       /* published percentages       */
static volatile int g_have_samples;

/* Hitman 2's P5 engine runs its simulation + rendering (and thus all the
 * x87 vertex work) on a single main thread — the one that calls Present,
 * captured in on_frame. We sample only that thread's EIP. This is both the
 * thread whose x87/render breakdown we care about AND the only one that is
 * safe to suspend: the wine/D3DMetal service threads sit blocked in GPU
 * syscalls where SuspendThread+GetThreadContext never returns under
 * CrossOver (and GetThreadTimes is static here, so an "active thread"
 * filter can't tell them apart). One known game thread sidesteps both. */
static HANDLE volatile g_render_th;   /* real handle to the Present thread */
static volatile int    g_run = 1;

static int classify(uint32_t eip)
{
    for (int i = 0; i < g_n_blobs; i++)
        if (g_blobs[i].base && eip - g_blobs[i].base < g_blobs[i].blob_total)
            return CAT_X87;
    if (g_rd3d_base && eip - g_rd3d_base < g_rd3d_size)  return CAT_REND;
    if (g_game_base && eip - g_game_base < g_game_size)  return CAT_GAME;
    return CAT_REST;
}

/* ---- module table, to name what "REST" actually is (diagnostic log) ---- */
#define MAX_MODS 160
typedef struct { uint32_t base, size; char name[32]; } Mod;
static Mod g_mods[MAX_MODS];
static int g_n_mods;

static void refresh_mods(void)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    MODULEENTRY32 me; me.dwSize = sizeof(me);
    int n = 0;
    if (Module32First(snap, &me)) {
        do {
            if (n >= MAX_MODS) break;
            g_mods[n].base = (uint32_t)(uintptr_t)me.modBaseAddr;
            g_mods[n].size = me.modBaseSize;
            lstrcpynA(g_mods[n].name, me.szModule, 32);
            n++;
        } while (Module32Next(snap, &me));
    }
    CloseHandle(snap);
    g_n_mods = n;
}

static const char *mod_name_at(uint32_t eip)
{
    for (int i = 0; i < g_n_mods; i++)
        if (eip - g_mods[i].base < g_mods[i].size) return g_mods[i].name;
    return "?";
}

/* per-window tally of which modules the REST samples land in */
#define MAX_RN 24
static char     g_rn[MAX_RN][32];
static uint32_t g_rc[MAX_RN];
static int      g_n_rn;

static void rest_hit(const char *name)
{
    for (int i = 0; i < g_n_rn; i++)
        if (!lstrcmpiA(g_rn[i], name)) { g_rc[i]++; return; }
    if (g_n_rn < MAX_RN) {
        lstrcpynA(g_rn[g_n_rn], name, 32);
        g_rc[g_n_rn] = 1;
        g_n_rn++;
    }
}

/* append "name=NN% name=NN% ..." for the top 3 REST modules into buf,
 * percentages relative to all REST samples this window */
static void rest_top(char *buf, size_t n)
{
    buf[0] = 0;
    size_t off = 0;
    uint32_t total = 0;
    for (int i = 0; i < g_n_rn; i++) total += g_rc[i];
    int used[MAX_RN] = {0};
    for (int k = 0; k < 3; k++) {
        int best = -1;
        for (int i = 0; i < g_n_rn; i++)
            if (!used[i] && (best < 0 || g_rc[i] > g_rc[best])) best = i;
        if (best < 0 || !g_rc[best]) break;
        used[best] = 1;
        int pct = total ? (int)((g_rc[best] * 100 + total / 2) / total) : 0;
        int w = snprintf(buf + off, n - off, "%s%s=%d%%",
                         off ? " " : "", g_rn[best], pct);
        if (w < 0 || (size_t)w >= n - off) break;
        off += w;
    }
}

static DWORD WINAPI sampler(LPVOID arg)
{
    (void)arg;
    uint32_t cnt[N_CAT] = {0};
    DWORD last_refresh = 0, last_pub = 0, last_log = 0;
    for (;;) {
        DWORD now = GetTickCount();
        if (!g_run) break;
        if (now - last_refresh > 1000) {
            last_refresh = now;
            module_range("RenderD3D.dll", &g_rd3d_base, &g_rd3d_size);
            module_range("hitman2.exe",   &g_game_base, &g_game_size);
            locate_blobs();
            refresh_mods();
        }
        HANDLE rt = g_render_th;
        if (rt) {
            if (SuspendThread(rt) != (DWORD)-1) {
                CONTEXT ctx; ctx.ContextFlags = CONTEXT_CONTROL;
                if (GetThreadContext(rt, &ctx)) {
                    uint32_t eip = (uint32_t)ctx.Eip;
                    int cat = classify(eip);
                    cnt[cat]++;
                    if (cat == CAT_REST) rest_hit(mod_name_at(eip));
                }
                ResumeThread(rt);
            }
        }
        if (now - last_pub > 500) {
            last_pub = now;
            uint32_t total = cnt[0] + cnt[1] + cnt[2] + cnt[3];
            if (total >= 20) {
                for (int c = 0; c < N_CAT; c++)
                    g_pct[c] = (int)((cnt[c] * 100 + total / 2) / total);
                g_have_samples = 1;
            }
            cnt[0] = cnt[1] = cnt[2] = cnt[3] = 0;
        }
        /* periodic headless snapshot to the log (no screen capture available) */
        if (now - last_log > 2000) {
            last_log = now;
            char restmods[96];
            rest_top(restmods, sizeof(restmods));
            logf_("fps=%d ms=%.1f/%.1f  X87=%d%% REND=%d%% GAME=%d%% REST=%d%%"
                  "  REST-top: %s", g_fps, g_ms_avg, g_ms_max,
                  g_pct[CAT_X87], g_pct[CAT_REND], g_pct[CAT_GAME],
                  g_pct[CAT_REST], restmods[0] ? restmods : "(none)");
            g_n_rn = 0;
        }
        Sleep(4);       /* ~250 Hz */
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* frame timing                                                        */
/* ------------------------------------------------------------------ */
static void tick_frame(void)
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (g_last_qpc.QuadPart) {
        double ms = (double)(now.QuadPart - g_last_qpc.QuadPart) * 1000.0
                    / (double)g_qpf.QuadPart;
        if (ms > g_win_max_ms) g_win_max_ms = ms;
        g_win_frames++;
    } else {
        g_win_start = now;
    }
    g_last_qpc = now;

    double win_ms = (double)(now.QuadPart - g_win_start.QuadPart) * 1000.0
                    / (double)g_qpf.QuadPart;
    if (win_ms >= 500.0 && g_win_frames > 0) {
        g_fps    = (int)(g_win_frames * 1000.0 / win_ms + 0.5);
        g_ms_avg = (float)(win_ms / g_win_frames);
        g_ms_max = (float)g_win_max_ms;
        g_win_start = now;
        g_win_frames = 0;
        g_win_max_ms = 0.0;
    }
}

/* ------------------------------------------------------------------ */
/* 5x7 pixel font (only the glyphs used below)                          */
/* ------------------------------------------------------------------ */
static const char *glyph_rows(char c, int row)
{
    static const char *SP[] = {"00000","00000","00000","00000","00000","00000","00000"};
    static const char *D0[] = {"01110","10001","10011","10101","11001","10001","01110"};
    static const char *D1[] = {"00100","01100","00100","00100","00100","00100","01110"};
    static const char *D2[] = {"01110","10001","00001","00010","00100","01000","11111"};
    static const char *D3[] = {"11110","00001","00001","01110","00001","00001","11110"};
    static const char *D4[] = {"00010","00110","01010","10010","11111","00010","00010"};
    static const char *D5[] = {"11111","10000","10000","11110","00001","00001","11110"};
    static const char *D6[] = {"01110","10000","10000","11110","10001","10001","01110"};
    static const char *D7[] = {"11111","00001","00010","00100","01000","01000","01000"};
    static const char *D8[] = {"01110","10001","10001","01110","10001","10001","01110"};
    static const char *D9[] = {"01110","10001","10001","01111","00001","00001","01110"};
    static const char *A_[] = {"01110","10001","10001","11111","10001","10001","10001"};
    static const char *D_[] = {"11110","10001","10001","10001","10001","10001","11110"};
    static const char *E_[] = {"11111","10000","10000","11110","10000","10000","11111"};
    static const char *F_[] = {"11111","10000","10000","11110","10000","10000","10000"};
    static const char *G_[] = {"01111","10000","10000","10011","10001","10001","01111"};
    static const char *M_[] = {"10001","11011","10101","10101","10001","10001","10001"};
    static const char *N_[] = {"10001","11001","10101","10011","10001","10001","10001"};
    static const char *P_[] = {"11110","10001","10001","11110","10000","10000","10000"};
    static const char *R_[] = {"11110","10001","10001","11110","10100","10010","10001"};
    static const char *S_[] = {"01111","10000","10000","01110","00001","00001","11110"};
    static const char *T_[] = {"11111","00100","00100","00100","00100","00100","00100"};
    static const char *X_[] = {"10001","10001","01010","00100","01010","10001","10001"};
    static const char *PCT[]= {"11001","11010","00100","00100","00100","01011","10011"};
    static const char *DOT[]= {"00000","00000","00000","00000","00000","01100","01100"};
    static const char *SLH[]= {"00001","00010","00100","00100","01000","10000","10000"};
    switch (c) {
    case '0': return D0[row]; case '1': return D1[row]; case '2': return D2[row];
    case '3': return D3[row]; case '4': return D4[row]; case '5': return D5[row];
    case '6': return D6[row]; case '7': return D7[row]; case '8': return D8[row];
    case '9': return D9[row];
    case 'A': return A_[row]; case 'D': return D_[row]; case 'E': return E_[row];
    case 'F': return F_[row]; case 'G': return G_[row]; case 'M': return M_[row];
    case 'N': return N_[row]; case 'P': return P_[row]; case 'R': return R_[row];
    case 'S': return S_[row]; case 'T': return T_[row]; case 'X': return X_[row];
    case '%': return PCT[row]; case '.': return DOT[row]; case '/': return SLH[row];
    default:  return SP[row];
    }
}

/* ------------------------------------------------------------------ */
/* D3D8 overlay drawing                                                */
/* ------------------------------------------------------------------ */
typedef struct { float x, y, z, rhw; DWORD color; } Vtx;
#define MAX_VTX 60000
static Vtx    g_vtx[MAX_VTX];
static int    g_nvtx;

#define VT(dev, idx, T) ((T)(*(void ***)(dev))[idx])
typedef HRESULT (STDMETHODCALLTYPE *SetRS_t)(IDirect3DDevice8*, D3DRENDERSTATETYPE, DWORD);
typedef HRESULT (STDMETHODCALLTYPE *GetRS_t)(IDirect3DDevice8*, D3DRENDERSTATETYPE, DWORD*);
typedef HRESULT (STDMETHODCALLTYPE *SetTex_t)(IDirect3DDevice8*, DWORD, IDirect3DBaseTexture8*);
typedef HRESULT (STDMETHODCALLTYPE *GetTex_t)(IDirect3DDevice8*, DWORD, IDirect3DBaseTexture8**);
typedef HRESULT (STDMETHODCALLTYPE *SetTSS_t)(IDirect3DDevice8*, DWORD, D3DTEXTURESTAGESTATETYPE, DWORD);
typedef HRESULT (STDMETHODCALLTYPE *GetTSS_t)(IDirect3DDevice8*, DWORD, D3DTEXTURESTAGESTATETYPE, DWORD*);
typedef HRESULT (STDMETHODCALLTYPE *SetVS_t)(IDirect3DDevice8*, DWORD);
typedef HRESULT (STDMETHODCALLTYPE *GetVS_t)(IDirect3DDevice8*, DWORD*);
typedef HRESULT (STDMETHODCALLTYPE *SetPS_t)(IDirect3DDevice8*, DWORD);
typedef HRESULT (STDMETHODCALLTYPE *GetPS_t)(IDirect3DDevice8*, DWORD*);
typedef HRESULT (STDMETHODCALLTYPE *SetSS_t)(IDirect3DDevice8*, UINT, IDirect3DVertexBuffer8*, UINT);
typedef HRESULT (STDMETHODCALLTYPE *GetSS_t)(IDirect3DDevice8*, UINT, IDirect3DVertexBuffer8**, UINT*);
typedef HRESULT (STDMETHODCALLTYPE *GetVP_t)(IDirect3DDevice8*, D3DVIEWPORT8*);
typedef HRESULT (STDMETHODCALLTYPE *DrawUP_t)(IDirect3DDevice8*, D3DPRIMITIVETYPE, UINT, const void*, UINT);

/* Exactly the device state draw() touches. A per-frame
 * CreateStateBlock(D3DSBT_ALL) capture/apply/delete makes wined3d snapshot
 * and replay the whole device state every frame — needless main-thread cost
 * under CrossOver. Save/restore only what we change (plus stream 0, which
 * DrawPrimitiveUP invalidates). */
static const D3DRENDERSTATETYPE k_saved_rs[] = {
    D3DRS_LIGHTING, D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_CULLMODE,
    D3DRS_ALPHABLENDENABLE, D3DRS_SRCBLEND, D3DRS_DESTBLEND, D3DRS_FOGENABLE,
};
#define N_SAVED_RS (sizeof(k_saved_rs) / sizeof(k_saved_rs[0]))
static const D3DTEXTURESTAGESTATETYPE k_saved_tss[] = {
    D3DTSS_COLOROP, D3DTSS_COLORARG1, D3DTSS_ALPHAOP, D3DTSS_ALPHAARG1,
};
#define N_SAVED_TSS (sizeof(k_saved_tss) / sizeof(k_saved_tss[0]))

typedef struct {
    DWORD rs[N_SAVED_RS];
    DWORD tss[N_SAVED_TSS];
    IDirect3DBaseTexture8 *tex;      /* AddRef'd by GetTexture      */
    IDirect3DVertexBuffer8 *stream0; /* AddRef'd by GetStreamSource */
    UINT stream0_stride;
    DWORD vs, ps;
} SavedState;

static void save_device_state(IDirect3DDevice8 *dev, SavedState *s)
{
    GetRS_t getRS  = VT(dev, 51, GetRS_t);
    GetTSS_t getTSS = VT(dev, 62, GetTSS_t);
    for (size_t i = 0; i < N_SAVED_RS; i++)
        getRS(dev, k_saved_rs[i], &s->rs[i]);
    for (size_t i = 0; i < N_SAVED_TSS; i++)
        getTSS(dev, 0, k_saved_tss[i], &s->tss[i]);
    VT(dev, 60, GetTex_t)(dev, 0, &s->tex);
    VT(dev, 84, GetSS_t)(dev, 0, &s->stream0, &s->stream0_stride);
    VT(dev, 77, GetVS_t)(dev, &s->vs);
    VT(dev, 89, GetPS_t)(dev, &s->ps);
}

static void restore_device_state(IDirect3DDevice8 *dev, SavedState *s)
{
    SetRS_t setRS  = VT(dev, 50, SetRS_t);
    SetTSS_t setTSS = VT(dev, 63, SetTSS_t);
    for (size_t i = 0; i < N_SAVED_RS; i++)
        setRS(dev, k_saved_rs[i], s->rs[i]);
    for (size_t i = 0; i < N_SAVED_TSS; i++)
        setTSS(dev, 0, k_saved_tss[i], s->tss[i]);
    VT(dev, 61, SetTex_t)(dev, 0, s->tex);
    VT(dev, 83, SetSS_t)(dev, 0, s->stream0, s->stream0_stride);
    VT(dev, 76, SetVS_t)(dev, s->vs);
    VT(dev, 88, SetPS_t)(dev, s->ps);
    /* the Get* calls AddRef'd these; Release = vtable slot 2 */
    typedef ULONG (STDMETHODCALLTYPE *Release_t)(void *);
    if (s->tex)     VT(s->tex, 2, Release_t)(s->tex);
    if (s->stream0) VT(s->stream0, 2, Release_t)(s->stream0);
    s->tex = NULL;
    s->stream0 = NULL;
}

static void add_rect(float x, float y, float w, float h, DWORD c)
{
    if (g_nvtx + 6 > MAX_VTX) return;
    Vtx *v = &g_vtx[g_nvtx];
    v[0] = (Vtx){x,     y,     0, 1, c};
    v[1] = (Vtx){x + w, y,     0, 1, c};
    v[2] = (Vtx){x + w, y + h, 0, 1, c};
    v[3] = (Vtx){x,     y,     0, 1, c};
    v[4] = (Vtx){x + w, y + h, 0, 1, c};
    v[5] = (Vtx){x,     y + h, 0, 1, c};
    g_nvtx += 6;
}

static void add_glyph(char ch, float x, float y, float cell, DWORD c)
{
    for (int r = 0; r < 7; r++) {
        const char *rowd = glyph_rows(ch, r);
        for (int col = 0; col < 5; col++)
            if (rowd[col] == '1')
                add_rect(x + col * cell, y + r * cell, cell, cell, c);
    }
}

static float text_w(const char *s, float cell) { return (float)strlen(s) * 6.0f * cell; }

static void add_text(const char *s, float x, float y, float cell, DWORD c)
{
    for (const char *p = s; *p; p++) {
        if (*p != ' ') add_glyph(*p, x, y, cell, c);
        x += 6.0f * cell;
    }
}

/* right-aligned, with a 1px drop shadow */
static void line_r(const char *s, float right, float y, float cell, DWORD c)
{
    float x = right - text_w(s, cell);
    add_text(s, x + 1, y + 1, cell, 0xC0000000);
    add_text(s, x, y, cell, c);
}

#define COL_HEAD 0xFFFFFFFF
#define COL_X87  0xFF56D364   /* green — native SSE2 (good) */
#define COL_REND 0xFFE0B040   /* amber — leftover emulated x87 */
#define COL_GAME 0xFF6CA0FF   /* blue  — game logic */
#define COL_REST 0xFFB0B0B0   /* grey  — everything else */

static void draw(IDirect3DDevice8 *dev)
{
    D3DVIEWPORT8 vp;
    if (FAILED(VT(dev, 41, GetVP_t)(dev, &vp))) return;

    /* Identical glyph metric to h2-stats-overlay: cell = max(1, 2*Scale),
     * so a given Scale renders the same size in both plugins. Default Scale
     * 1.0 => cell 2 (integer, so pixel-crisp; non-integer cells blur).
     * Like it, no background panel — just outlined text. */
    const float cell = 2.0f * g_scale < 1.0f ? 1.0f : 2.0f * g_scale;
    const float lh   = 9.0f * cell;                 /* line height */
    const float right = (float)(vp.X + vp.Width - g_off_x);
    float y = (float)(vp.Y + g_off_y);

    /* lines */
    char buf[48];
    g_nvtx = 0;

    snprintf(buf, sizeof(buf), "FPS %d", g_fps);
    line_r(buf, right, y, cell, COL_HEAD); y += lh;
    snprintf(buf, sizeof(buf), "MS %.1f/%.1f", g_ms_avg, g_ms_max);
    line_r(buf, right, y, cell, COL_HEAD); y += lh;

    if (g_show_cpu && g_have_samples) {
        snprintf(buf, sizeof(buf), "X87 %d%%",  g_pct[CAT_X87]);
        line_r(buf, right, y, cell, COL_X87);  y += lh;
        snprintf(buf, sizeof(buf), "REND %d%%", g_pct[CAT_REND]);
        line_r(buf, right, y, cell, COL_REND); y += lh;
        snprintf(buf, sizeof(buf), "GAME %d%%", g_pct[CAT_GAME]);
        line_r(buf, right, y, cell, COL_GAME); y += lh;
        snprintf(buf, sizeof(buf), "REST %d%%", g_pct[CAT_REST]);
        line_r(buf, right, y, cell, COL_REST); y += lh;
    }

    if (!g_nvtx) return;

    /* save the touched device state, set 2D untextured alpha-blended draw,
     * restore */
    SavedState saved = {0};
    save_device_state(dev, &saved);

    SetRS_t setRS = VT(dev, 50, SetRS_t);
    setRS(dev, D3DRS_LIGHTING, FALSE);
    setRS(dev, D3DRS_ZENABLE, D3DZB_FALSE);
    setRS(dev, D3DRS_ZWRITEENABLE, FALSE);
    setRS(dev, D3DRS_CULLMODE, D3DCULL_NONE);
    setRS(dev, D3DRS_ALPHABLENDENABLE, TRUE);
    setRS(dev, D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    setRS(dev, D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    setRS(dev, D3DRS_FOGENABLE, FALSE);
    VT(dev, 61, SetTex_t)(dev, 0, NULL);
    SetTSS_t setTSS = VT(dev, 63, SetTSS_t);
    setTSS(dev, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    setTSS(dev, 0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    setTSS(dev, 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    setTSS(dev, 0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
    VT(dev, 88, SetPS_t)(dev, 0);
    VT(dev, 76, SetVS_t)(dev, D3DFVF_XYZRHW | D3DFVF_DIFFUSE);

    VT(dev, 72, DrawUP_t)(dev, D3DPT_TRIANGLELIST, g_nvtx / 3, g_vtx, sizeof(Vtx));

    restore_device_state(dev, &saved);
}

/* ------------------------------------------------------------------ */
/* hook callbacks                                                      */
/* ------------------------------------------------------------------ */
static DWORD g_last_cfg;

static void on_frame(IDirect3DDevice8 *dev)
{
    if (!dev) return;
    /* First frame: grab a real, suspendable handle to this (the Present /
     * main game) thread for the sampler. GetCurrentThread() is a pseudo-
     * handle only valid on this thread, so duplicate it into a real one. */
    if (!g_render_th) {
        HANDLE real = NULL;
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(),
                        GetCurrentProcess(), &real,
                        THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT,
                        FALSE, 0);
        g_render_th = real;
    }
    tick_frame();
    DWORD now = GetTickCount();
    if (now - g_last_cfg > 1000) { g_last_cfg = now; reload_config_if_changed(); }
    if (!g_enabled) return;
    draw(dev);
}

static const H2SAD3D8Hooks g_hooks = {
    H2SA_D3D8_HOOKS_VERSION,
    NULL,        /* fix_present    */
    NULL,        /* fix_projection */
    NULL,        /* on_device      */
    NULL,        /* on_present     */
    on_frame,    /* on_frame       */
};

/* ------------------------------------------------------------------ */
/* init — called from widescreen.c's DllMain (h2sa_core_dir/ini and the
 * log are already set up there)                                       */
/* ------------------------------------------------------------------ */
void h2sa_profiler_init(HMODULE self)
{
    (void)self;
    logf_("h2sa_core profiler loaded");

    load_config();
    load_blob_desc();

    QueryPerformanceFrequency(&g_qpf);

    CreateThread(NULL, 0, sampler, NULL, 0, NULL);

    HMODULE loader = GetModuleHandleA("d3d8.dll");
    h2sa_register_fn reg = loader ? (h2sa_register_fn)(uintptr_t)
        GetProcAddress(loader, "H2SA_RegisterD3D8Hooks") : NULL;
    if (reg) { reg(&g_hooks); logf_("registered D3D8 on_frame hook (profiler)"); }
    else logf_("d3d8.dll loader / H2SA_RegisterD3D8Hooks not found");
}

void h2sa_profiler_detach(void)
{
    g_run = 0;
}

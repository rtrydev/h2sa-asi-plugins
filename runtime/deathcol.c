/* h2sa_core.asi (death-screen half) — de-blind the dying animation
 * ([DeathScreen] section of h2sa_core.ini). Two pure GAME-DATA changes,
 * no code patched, no D3D call intercepted:
 *
 * 1. BACKGROUND COLOR. When 47 dies, the engine switches to a scene camera
 *    named "Cam_Right_Arm_OverrideBackCol" (in every level's GMS data).
 *    Camera names ending in "_OverrideBackCol" are special-cased by
 *    MainCameraControl (VA 0x4C524F): the active camera's background color
 *    is replaced with THAT camera's BackCol, and because its fFogNear/
 *    fFogFar are 0/0 the engine applies its CameraFogBegin/CameraFogFull
 *    defaults (0.65/0.95) — a full-screen fog in the BackCol. The stock
 *    data ships BackCol = 0x00FFFFFF: the blinding white void. Every
 *    caller of the background/fog applier (0x44CBC0) pushes some camera's
 *    +0x68, so rewriting that one dword recolors the whole death fade.
 *    (The similarly-named "BlackScreenCamera_OverrideBackCol" is the black
 *    loading/reload fade and is left untouched.)
 *
 * 2. THE WHITE SQUARE. The dying animation plays inside a die-sequence
 *    template that every level embeds (BUF names right next to the death
 *    camera: "Bounding_Room" — a ZROOM parked outside the playable world —
 *    with ZSTDOBJs "Plane_Lower" and "White_BloodCover", the white quads
 *    47 falls onto; they were authored white to blend with the stock white
 *    background). Each ZSTDOBJ proxy object {vt 0x644020, +4 name,
 *    +8 instance, +C classinfo} points to a 0x70-byte placed-instance
 *    record: 3x3 matrix, position, +0x3C flags, +0x40 bbox, +0x50 room
 *    instance, +0x54 back-pointer to the proxy. Comparing known-visible
 *    records (Plane_Lower 0x09868480, White_BloodCover 0x09860400)
 *    against known-invisible ones (dummy marker Pos_Hm1 0x09820400,
 *    others 0x098x0000) isolates bit 0x00040000 as the render/visible
 *    flag — but live pokes on a paused death screen showed the renderer
 *    ignores flag bits once the cut sequence has activated the records
 *    (it also flips White_BloodCover's magic byte 0x09->0x0B and sets
 *    bit 0x1000 on both at death start). What the renderer DOES consume
 *    every frame is the record's 3x3 matrix: zeroing it removed the
 *    square instantly. HidePlane=1 keeps both quads' matrices zeroed
 *    (scale 0 = degenerate geometry, nothing rasterized) so the body
 *    lies directly on the (now dark) background.
 *
 * Offsets are from the unpacked retail exe, cross-checked live with
 * memory probes. Mechanics mirror camera.c: gate on the known retail exe
 * being unpacked, find objects with a background scanner (sweep over
 * private committed memory, ReadProcessMemory only — see camera.c for the
 * Wine decommit-during-sweep lesson), and do the actual writes on the
 * game's present thread after re-validating everything in place. A scene
 * change reallocates the objects; the scanner keeps sweeping (every
 * RESCAN_MS) so the changes land moments after each load, long before a
 * death can use them. */
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "h2sa_core.h"

#define logf_ h2sa_core_logf

/* config ([DeathScreen] section of the shared h2sa_core.ini) */
static int g_enable = 1;
static uint32_t g_backcol = 0x00101010u;   /* near-black neutral */
static int g_hideplane = 1;

/* retail hitman2.exe identity + unpack signature (same gate as camera.c) */
#define H2_TIMESTAMP      0x3EF859D5u
#define H2_SIZEOFIMAGE    0x2EC000u
#define SIG_VA            0x4C4975u
static const uint8_t SIG[] = { 0x8B,0x88,0x04,0x17,0x00,0x00,0x8B,0x11,
                               0x68,0x4C,0x2D,0x68,0x00,0xFF,0x52,0x14,0xC3 };

/* camera side */
#define VA_ZCAMERA_VTBL   0x0063E558u
#define OFF_CAM_NAME      0x04u
#define OFF_CAM_BACKCOL   0x68u
#define CAM_MIN_SIZE      0x74u          /* vptr..fFogFar */
#define NAME_SUFFIX       "_OverrideBackCol"
#define NAME_MAX          64

/* white-square side */
#define VA_ZSTDOBJ_VTBL   0x00644020u
#define VA_ZSTDOBJ_CI     0x006A9570u    /* ?ZSTDOBJ_ClassInfo@@... */
#define OFF_OBJ_INST      0x08u
#define OBJ_HDR_SIZE      0x10u
#define INST_SIZE         0x70u
#define OFF_INST_FLAGS    0x3Cu
#define OFF_INST_BACKPTR  0x54u
/* magic byte of a placed-instance record's flag dword: 0x09 normally;
 * the cut sequence flips White_BloodCover's to 0x0B while the death
 * screen is up, so mask out that bit when validating */
#define INST_FLAG_MAGIC   0x09000000u
#define INST_FLAG_MASK    0xFD000000u
#define OFF_INST_MATRIX   0x00u          /* 9 dwords: 3x3 float matrix */
#define MATRIX_DWORDS     9
static const char *PLANE_NAMES[] = { "Plane_Lower", "White_BloodCover" };
#define NPLANE 2

#define MAX_CAND          8
static void * volatile g_cand[MAX_CAND];        /* white cameras */
struct plane { void *proxy; void *inst; };
static struct plane volatile g_plane[MAX_CAND]; /* die-sequence quads */

static int g_state = -1;             /* -1 waiting for unpack, 0 off, 1 armed */
static int g_hide_logs;              /* hide log budget */

static int is_bright(uint32_t c)
{
    return ((c >> 16) & 0xffu) >= 0xE0u &&
           ((c >>  8) & 0xffu) >= 0xE0u &&
           ( c        & 0xffu) >= 0xE0u;
}

static int readable(const void *p, size_t n)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (!p || VirtualQuery(p, &mbi, sizeof(mbi)) != sizeof(mbi))
        return 0;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
        return 0;
    return (const uint8_t *)p + n <=
           (const uint8_t *)mbi.BaseAddress + mbi.RegionSize;
}

static int safe_read(const void *src, void *dst, size_t n)
{
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), src, dst, n, &got) &&
           got == n;
}

/* name ends with "_OverrideBackCol" (bounded, printable) */
static int name_matches(char *name, size_t cap)
{
    size_t len = 0;
    while (len < cap && name[len]) {
        unsigned char c = (unsigned char)name[len];
        if (c < 0x20 || c > 0x7e)
            return 0;
        len++;
    }
    if (len == 0 || len >= cap)
        return 0;
    size_t sl = sizeof(NAME_SUFFIX) - 1;
    return len >= sl && memcmp(name + len - sl, NAME_SUFFIX, sl) == 0;
}

/* candidate looks like a live white "_OverrideBackCol" ZCAMERA
 * (scanner-thread version: RPM'd copies only) */
static int cam_probe(uint8_t *cand, char *name_out /* NAME_MAX */,
                     uint32_t *col_out)
{
    uint8_t camb[CAM_MIN_SIZE];
    uint32_t vptr;
    char *nm;
    if (!safe_read(cand, camb, sizeof(camb)))
        return 0;
    memcpy(&vptr, camb, sizeof(vptr));
    if (vptr != VA_ZCAMERA_VTBL)
        return 0;
    memcpy(&nm, camb + OFF_CAM_NAME, sizeof(nm));
    if (!nm || !safe_read(nm, name_out, NAME_MAX))
        return 0;
    name_out[NAME_MAX - 1] = 0;
    if (!name_matches(name_out, NAME_MAX - 1))
        return 0;
    memcpy(col_out, camb + OFF_CAM_BACKCOL, sizeof(*col_out));
    return is_bright(*col_out);
}

/* candidate proxy at `cand` (a dword equal to the ZSTDOBJ vtable) is one
 * of the die-sequence quads: name matches, instance record's back-pointer
 * closes the loop, flags look like a placed-instance record.
 * Scanner-thread version: RPM'd copies only. */
static int plane_probe(uint8_t *cand, void **inst_out)
{
    uint32_t hdr[OBJ_HDR_SIZE / 4];
    char name[NAME_MAX];
    char *nm;
    if (!safe_read(cand, hdr, sizeof(hdr)))
        return 0;
    if (hdr[0] != VA_ZSTDOBJ_VTBL || hdr[3] != VA_ZSTDOBJ_CI)
        return 0;
    nm = (char *)(uintptr_t)hdr[1];
    if (!nm || !safe_read(nm, name, sizeof(name)))
        return 0;
    name[NAME_MAX - 1] = 0;
    int k;
    for (k = 0; k < NPLANE; k++)
        if (!strcmp(name, PLANE_NAMES[k]))
            break;
    if (k == NPLANE)
        return 0;
    uint8_t *inst = (uint8_t *)(uintptr_t)hdr[2];
    uint8_t instb[INST_SIZE];
    if (!inst || !safe_read(inst, instb, sizeof(instb)))
        return 0;
    uint32_t backptr, flags;
    memcpy(&backptr, instb + OFF_INST_BACKPTR, sizeof(backptr));
    memcpy(&flags, instb + OFF_INST_FLAGS, sizeof(flags));
    if (backptr != (uint32_t)(uintptr_t)cand)
        return 0;
    if ((flags & INST_FLAG_MASK) != INST_FLAG_MAGIC)
        return 0;
    *inst_out = inst;
    return 1;
}

/* one-time gate: known retail exe, unpacked (kept in sync with camera.c) */
static int probe_exe(void)
{
    uint8_t *base = (uint8_t *)GetModuleHandleA(NULL);
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    IMAGE_NT_HEADERS32 *nt = (IMAGE_NT_HEADERS32 *)(base + dos->e_lfanew);
    if (nt->FileHeader.TimeDateStamp != H2_TIMESTAMP ||
        nt->OptionalHeader.SizeOfImage != H2_SIZEOFIMAGE) {
        logf_("deathscreen: hitman2.exe is not the known retail build "
              "(stamp %08lx size %08lx) — off",
              (unsigned long)nt->FileHeader.TimeDateStamp,
              (unsigned long)nt->OptionalHeader.SizeOfImage);
        return 0;
    }
    if ((uintptr_t)base != 0x400000u) {
        logf_("deathscreen: hitman2.exe relocated to %p — off", base);
        return 0;
    }
    if (memcmp((void *)(uintptr_t)SIG_VA, SIG, sizeof(SIG)) != 0)
        return -1;                        /* packed .text not unpacked yet */
    logf_("deathscreen: retail exe unpacked, armed (BackCol=%06lx "
          "HidePlane=%d)", (unsigned long)g_backcol, g_hideplane);
    return 1;
}

/* sweep buffer: .bss is MEM_IMAGE, so the MEM_PRIVATE sweep never sees our
 * own copies of the vtable constant. +64 tail so name strings crossing a
 * chunk boundary still match. */
static uint8_t g_scan_buf[0x10000 + 64];

typedef void (*chunk_cb)(uint8_t *live, uint8_t *copy, size_t n);

static void sweep(chunk_cb cb, int tail)
{
    uint8_t *p = (uint8_t *)0x10000;
    MEMORY_BASIC_INFORMATION mbi;
    while ((uintptr_t)p < 0xfffe0000u &&    /* exe is LAA-patched */
           VirtualQuery(p, &mbi, sizeof(mbi)) == sizeof(mbi)) {
        uint8_t *base = (uint8_t *)mbi.BaseAddress;
        SIZE_T size = mbi.RegionSize;
        int ok = mbi.State == MEM_COMMIT &&
            mbi.Type == MEM_PRIVATE &&
            !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) &&
            (mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY |
                            PAGE_EXECUTE_READWRITE |
                            PAGE_EXECUTE_WRITECOPY));
        /* skip our own stack: it can hold spilled copies of what we
         * scan for */
        if ((uint8_t *)&mbi >= base && (uint8_t *)&mbi < base + size)
            ok = 0;
        SIZE_T off = 0;
        while (ok && off < (size & ~(SIZE_T)3)) {
            SIZE_T want = (size & ~(SIZE_T)3) - off;
            SIZE_T got = 0;
            if (want > 0x10000)
                want = 0x10000;
            if (!ReadProcessMemory(GetCurrentProcess(), base + off,
                                   g_scan_buf, want, &got) && !got) {
                /* Windows RPM is all-or-nothing across a hole: retry one
                 * page, then skip it if it is the hole */
                want = 0x1000;
                if (!ReadProcessMemory(GetCurrentProcess(), base + off,
                                       g_scan_buf, want, &got) && !got) {
                    off += 0x1000;
                    continue;
                }
            }
            got &= ~(SIZE_T)3;
            if (tail && got == want && off + got < size) {
                SIZE_T extra = size - (off + got), g2 = 0;
                if (extra > 64) extra = 64;
                ReadProcessMemory(GetCurrentProcess(), base + off + got,
                                  g_scan_buf + got, extra, &g2);
                cb(base + off, g_scan_buf, got + g2);
            } else {
                cb(base + off, g_scan_buf, got);
            }
            off += got > want ? want : (got ? got : 0x1000);
        }
        p = base + size;
        if (p < base) break;                /* address wrap */
    }
}

/* per-sweep results, filled by the callbacks */
static void *s_cam[MAX_CAND];   static int s_ncam;
static uintptr_t s_str[NPLANE][4]; static int s_nstr[NPLANE];
static struct plane s_pl[MAX_CAND]; static int s_npl;

static void pass1(uint8_t *live, uint8_t *copy, size_t n)
{
    /* white cameras: dword scan for the ZCAMERA vtable */
    for (size_t i = 0; i + 4 <= (n & ~(size_t)3); i += 4) {
        uint32_t v;
        memcpy(&v, copy + i, sizeof(v));
        if (v == VA_ZCAMERA_VTBL && s_ncam < MAX_CAND) {
            char nm[NAME_MAX];
            uint32_t col;
            if (cam_probe(live + i, nm, &col))
                s_cam[s_ncam++] = live + i;
        }
    }
    if (!g_hideplane)
        return;
    /* die-sequence quad names: byte scan (they live in the BUF arena) */
    for (size_t i = 0; i < n; i++)
        for (int k = 0; k < NPLANE; k++) {
            size_t l = strlen(PLANE_NAMES[k]) + 1;
            if (i + l <= n && copy[i] == (uint8_t)PLANE_NAMES[k][0] &&
                !memcmp(copy + i, PLANE_NAMES[k], l) && s_nstr[k] < 4)
                s_str[k][s_nstr[k]++] = (uintptr_t)(live + i);
        }
}

static void pass2(uint8_t *live, uint8_t *copy, size_t n)
{
    /* proxies whose +4 name pointer targets a found name string */
    for (size_t i = 0; i + 4 <= n; i += 4) {
        uint32_t v;
        memcpy(&v, copy + i, sizeof(v));
        for (int k = 0; k < NPLANE; k++)
            for (int s = 0; s < s_nstr[k]; s++)
                if (v == s_str[k][s] && s_npl < MAX_CAND) {
                    void *inst;
                    uint8_t *proxy = live + i - 4;   /* name is at +4 */
                    if (i >= 4 && plane_probe(proxy, &inst)) {
                        s_pl[s_npl].proxy = proxy;
                        s_pl[s_npl].inst = inst;
                        s_npl++;
                    }
                }
    }
}

#define RESCAN_MS 3000
static DWORD WINAPI scan_thread(LPVOID arg)
{
    (void)arg;
    for (;;) {
        if (g_state != 1)
            return 0;
        s_ncam = 0;
        s_npl = 0;
        memset(s_nstr, 0, sizeof(s_nstr));
        sweep(pass1, 1);
        if (g_hideplane && (s_nstr[0] || s_nstr[1]))
            sweep(pass2, 0);
        for (int i = 0; i < MAX_CAND; i++) {
            g_cand[i] = i < s_ncam ? s_cam[i] : NULL;
            g_plane[i].proxy = i < s_npl ? s_pl[i].proxy : NULL;
            g_plane[i].inst  = i < s_npl ? s_pl[i].inst  : NULL;
        }
        Sleep(RESCAN_MS);
    }
}

/* per frame, from the loader's Present hook (the game's main thread).
 * Frees happen on this thread, so in-place reads are safe here after
 * readable() — same rule as camera.c. */
void h2sa_deathcol_frame(void)
{
    if (!g_enable || g_state != 1)
        return;

    /* 1. white death-camera background -> configured color */
    for (int i = 0; i < MAX_CAND; i++) {
        uint8_t *cam = (uint8_t *)g_cand[i];
        if (!cam || !readable(cam, CAM_MIN_SIZE))
            continue;
        uint32_t vptr;
        memcpy(&vptr, cam, sizeof(vptr));
        if (vptr != VA_ZCAMERA_VTBL)
            continue;
        char *nm;
        memcpy(&nm, cam + OFF_CAM_NAME, sizeof(nm));
        if (!readable(nm, NAME_MAX))
            continue;
        char name[NAME_MAX];
        memcpy(name, nm, NAME_MAX - 1);
        name[NAME_MAX - 1] = 0;
        if (!name_matches(name, NAME_MAX - 1))
            continue;
        uint32_t col;
        memcpy(&col, cam + OFF_CAM_BACKCOL, sizeof(col));
        if (!is_bright(col) || col == g_backcol)
            continue;
        memcpy(cam + OFF_CAM_BACKCOL, &g_backcol, sizeof(g_backcol));
        logf_("deathscreen: \"%s\" at %p BackCol %06lx -> %06lx",
              name, cam, (unsigned long)col, (unsigned long)g_backcol);
    }

    /* 2. die-sequence white quads -> collapsed. The renderer consumes the
     * instance matrix every frame (flag bits are NOT re-checked once the
     * cut sequence has activated the record — verified by live pokes on a
     * paused death screen: flag clears changed nothing, zeroing the
     * matrix removed the square instantly). Keep the matrix zeroed every
     * frame in case the engine re-places the template. */
    if (!g_hideplane)
        return;
    for (int i = 0; i < MAX_CAND; i++) {
        uint8_t *proxy = (uint8_t *)g_plane[i].proxy;
        uint8_t *inst  = (uint8_t *)g_plane[i].inst;
        if (!proxy || !inst)
            continue;
        if (!readable(proxy, OBJ_HDR_SIZE) || !readable(inst, INST_SIZE))
            continue;
        uint32_t hdr[OBJ_HDR_SIZE / 4];
        memcpy(hdr, proxy, sizeof(hdr));
        if (hdr[0] != VA_ZSTDOBJ_VTBL || hdr[3] != VA_ZSTDOBJ_CI ||
            (uint8_t *)(uintptr_t)hdr[2] != inst)
            continue;
        uint32_t backptr, flags;
        memcpy(&backptr, inst + OFF_INST_BACKPTR, sizeof(backptr));
        memcpy(&flags, inst + OFF_INST_FLAGS, sizeof(flags));
        if (backptr != (uint32_t)(uintptr_t)proxy ||
            (flags & INST_FLAG_MASK) != INST_FLAG_MAGIC)
            continue;
        uint32_t mtx[MATRIX_DWORDS];
        memcpy(mtx, inst + OFF_INST_MATRIX, sizeof(mtx));
        int dirty = 0;
        for (int d = 0; d < MATRIX_DWORDS; d++)
            if (mtx[d]) { dirty = 1; break; }
        if (!dirty)
            continue;
        memset(mtx, 0, sizeof(mtx));
        memcpy(inst + OFF_INST_MATRIX, mtx, sizeof(mtx));
        if (g_hide_logs < 8) {
            g_hide_logs++;
            char *nm;
            char name[NAME_MAX] = "?";
            memcpy(&nm, proxy + 4, sizeof(nm));
            if (readable(nm, NAME_MAX)) {
                memcpy(name, nm, NAME_MAX - 1);
                name[NAME_MAX - 1] = 0;
            }
            logf_("deathscreen: collapsed \"%s\" (instance %p, matrix "
                  "zeroed, flags %08lx)", name, inst, (unsigned long)flags);
        }
    }
}

static DWORD WINAPI arm_thread(LPVOID arg)
{
    (void)arg;
    for (int i = 0; i < 1200 && g_state < 0; i++) {   /* up to 5 minutes */
        g_state = probe_exe();
        if (g_state < 0)
            Sleep(250);
    }
    if (g_state < 0) {
        logf_("deathscreen: exe never unpacked to the known image — off");
        g_state = 0;
        return 0;
    }
    if (g_state == 1)
        scan_thread(NULL);
    return 0;
}

void h2sa_deathcol_init(void)
{
    FILE *f = fopen(h2sa_core_ini, "r");
    if (f) {
        char line[128], section[32] = "";
        int b;
        unsigned int col;
        while (fgets(line, sizeof(line), f)) {
            char s[32];
            if (sscanf(line, " [%31[^]]]", s) == 1) {
                lstrcpynA(section, s, sizeof(section));
                continue;
            }
            if (_stricmp(section, "DeathScreen") != 0)
                continue;
            if (sscanf(line, " Enabled = %d", &b) == 1 ||
                sscanf(line, " Enabled=%d", &b) == 1)
                g_enable = (b != 0);
            if (sscanf(line, " BackCol = %x", &col) == 1 ||
                sscanf(line, " BackCol=%x", &col) == 1)
                g_backcol = col & 0x00ffffffu;
            if (sscanf(line, " HidePlane = %d", &b) == 1 ||
                sscanf(line, " HidePlane=%d", &b) == 1)
                g_hideplane = (b != 0);
        }
        fclose(f);
    }
    logf_("deathscreen: Enabled=%d BackCol=%06lx HidePlane=%d", g_enable,
          (unsigned long)g_backcol, g_hideplane);
    if (g_enable) {
        HANDLE t = CreateThread(NULL, 0, arm_thread, NULL, 0, NULL);
        if (t) {
            SetThreadPriority(t, THREAD_PRIORITY_BELOW_NORMAL);
            CloseHandle(t);
        }
    }
}

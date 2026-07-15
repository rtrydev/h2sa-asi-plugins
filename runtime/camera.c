/* h2sa_core.asi (camera half) — auto zoom-out for the third-person camera.
 *
 * The gameplay camera ("HM2StickCamera", the camera-on-a-stick behind 47)
 * pulls in when level geometry blocks the line from the pivot to the camera
 * — that part is fine — but it does NOT swing back out when the obstruction
 * clears. The engine's camera boom keeps a RATCHETED "allowed length":
 * every frame the boom casts its collision lines and takes the shortest
 * free distance (the "measured" length); when measured drops below allowed,
 * allowed follows it IMMEDIATELY, but when measured grows past allowed
 * again the engine only raises allowed after a 0x1400 (5120) ms cooldown —
 * and the cooldown timestamp is refreshed on every change, so after walking
 * out of a tight spot the camera stays needlessly zoomed for seconds until
 * the player scrolls out by hand (the zoom-out scroll resets allowed
 * directly, which is why scrolling fixes it instantly).
 *
 * This feature ([Camera] AutoZoomOut in h2sa_core.ini) removes the wait,
 * with two per-frame DATA writes — no code is patched, so it coexists with
 * the h2sa_reduced_x87 entry hooks (which translate these very functions):
 *
 *  1. The boom's cooldown timestamp is backdated past the 5120 ms window,
 *     so the engine's own grow path raises allowed the first frame the
 *     measured distance opens up: the camera eases back out at its stock
 *     speed the moment it can. Zoom-in, the collision math, the easing and
 *     the [20,300] operating bounds are all untouched — the game simply
 *     stops sitting on an expired constraint.
 *
 *  2. While a collision actually holds the camera below the player's
 *     preferred distance (mouse-wheel zoom: 150 = in, 300 = out), the
 *     preferred distance is reset to the zoomed-out preset (300). The
 *     wheel still works as a temporary override — but once an auto zoom-in
 *     happens, the camera opens fully at the first occasion instead of
 *     returning to the closer pick.
 *
 * Finding the camera — two mechanisms, hook first, scanner as fallback:
 *
 *  - VTABLE HOOK (instant): the camera's activation handler (VA 0x4F2D60,
 *    the virtual that restores the saved distances and sets the active
 *    flag when gameplay starts) is reached exclusively through slot
 *    +0x8C of the class vtable at 0x652B98. That one dword is repointed
 *    at a wrapper which adopts `this` the moment the engine activates the
 *    camera — the fix is in force from the mission's first frame. A
 *    vtable is data, so this cannot collide with the h2sa_reduced_x87
 *    entry hooks, and it only affects HM2StickCamera (each class has its
 *    own vtable). The slot is byte-checked before patching.
 *
 *  - HEAP SCAN (fallback): a low-priority background thread scans private
 *    committed memory for objects whose vptr is 0x652B98 — verified live:
 *    the engine allocates exactly one HM2StickCamera per scene. (The
 *    engine's own name registry lookup, [[0x6A6C5C]+0x1704]->
 *    find("HM2StickCam"), returns NULL from the Present hook's context,
 *    so it is not used.)
 *
 * The Present-thread hook only ever touches a candidate after
 * re-validating it in place. All offsets below were taken from the
 * unpacked exe (the engine compiles its objects byte-packed, hence the
 * odd offsets) and cross-checked against a running game with an external
 * memory probe:
 *
 *   [0x6A6C5C]        ZEngineDataBase* singleton
 *     +0x38           uint32 game time, ms (the cooldown clock; pauses with
 *                     the game, which is exactly what the ratchet compares)
 *   camera (HM2StickCamera, vptr 0x652B98)
 *     +0x84           byte  active flag (set on activate, cleared on leave)
 *     +0x128          float current distance (eased, then collision-clamped)
 *     +0x12C          float preferred distance (wheel presets 150/300)
 *     +0x161          boom (collision line set) pointer, heap block 0x253
 *     +0x1A9          float desired distance (eases toward preferred)
 *   boom
 *     +0x23A          float measured free length this frame (~1e38 = clear)
 *     +0x23F          float near offset (subtracted for the length clamp)
 *     +0x24B          float allowed length (the ratchet)
 *     +0x24F          uint32 timestamp of the last allowed change
 *
 * The exe ships packed, so none of this exists in memory until its unpacker
 * has run: the scan is gated on a 17-byte signature of the unpacked image
 * (inside a getter at VA 0x4C4975 — a site the x87 plugin never rewrites)
 * and the feature disables itself, with a log line, if the exe is not the
 * known retail build.
 */
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "h2sa_core.h"

#define logf_ h2sa_core_logf

/* config ([Camera] section of the shared h2sa_core.ini) */
static int g_autozoom = 1;

/* retail hitman2.exe identity + unpack signature */
#define H2_TIMESTAMP      0x3EF859D5u
#define H2_SIZEOFIMAGE    0x2EC000u
#define SIG_VA            0x4C4975u
static const uint8_t SIG[] = { 0x8B,0x88,0x04,0x17,0x00,0x00,0x8B,0x11,
                               0x68,0x4C,0x2D,0x68,0x00,0xFF,0x52,0x14,0xC3 };

#define VA_ENGINE_DB      0x6A6C5Cu
#define OFF_DB_TIME_MS    0x38u
#define VA_STICKCAM_VTBL  0x652B98u
#define VA_ACTIVATE_SLOT  0x652C24u  /* vtbl slot +0x8C, holds 0x4F2D60 */
#define VA_ACTIVATE_FN    0x4F2D60u  /* HM2StickCamera activation handler */
#define OFF_CAM_ACTIVE    0x84u
#define OFF_CAM_CURRENT   0x128u
#define OFF_CAM_PREFERRED 0x12Cu
#define OFF_CAM_BOOM      0x161u
#define OFF_BOOM_MEASURED 0x23Au
#define OFF_BOOM_NEAROFF  0x23Fu
#define OFF_BOOM_STAMP    0x24Fu
#define BOOM_SIZE         0x253u
#define CAM_SIZE          0x1B0u
#define GROW_COOLDOWN_MS  0x1400u
#define DIST_ZOOMED_OUT   300.0f     /* the game's own wheel zoom-out preset */

static volatile LONG g_cam_found;    /* candidate from the scanner thread */
static int g_state = -1;             /* -1 waiting for unpack, 0 off, 1 armed */
static uint8_t *g_cam;               /* validated camera (present thread) */
static int g_reset_logs;             /* preferred-distance reset log budget */

static int readable(const void *p, size_t n)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (!p || VirtualQuery(p, &mbi, sizeof(mbi)) != sizeof(mbi))
        return 0;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
        return 0;
    /* require the whole range inside this region (fields never straddle) */
    return (const uint8_t *)p + n <=
           (const uint8_t *)mbi.BaseAddress + mbi.RegionSize;
}

static float fld32(const void *base, uint32_t off)
{
    float v;
    memcpy(&v, (const uint8_t *)base + off, sizeof(v));
    return v;
}

/* the candidate must look like a live stick camera before it is touched */
static int cam_valid(uint8_t *cam)
{
    if (!cam || !readable(cam, CAM_SIZE))
        return 0;
    uint32_t vptr;
    memcpy(&vptr, cam, sizeof(vptr));
    if (vptr != VA_STICKCAM_VTBL)
        return 0;
    uint8_t *boom;
    memcpy(&boom, cam + OFF_CAM_BOOM, sizeof(boom));   /* packed: unaligned */
    if (!readable(boom, BOOM_SIZE))
        return 0;
    float pref = fld32(cam, OFF_CAM_PREFERRED);
    float near_ = fld32(boom, OFF_BOOM_NEAROFF);
    float meas = fld32(boom, OFF_BOOM_MEASURED);
    return pref >= 10.0f && pref <= 1000.0f &&
           near_ >= -1000.0f && near_ <= 1000.0f &&
           meas >= 0.0f;
}

/* vtable hook on the activation handler: adopt the camera the moment the
 * engine activates it, so the fix is in force from the mission's first
 * frame (the heap scanner below then only serves as a fallback). thiscall,
 * one stack arg, callee-cleans — mirrors the original's `ret 4`. */
typedef int (__thiscall *activate_fn)(void *self, void *arg);

static int __thiscall activate_hook(void *self, void *arg)
{
    int r = ((activate_fn)(uintptr_t)VA_ACTIVATE_FN)(self, arg);
    if (cam_valid((uint8_t *)self)) {
        if ((uint8_t *)(uintptr_t)g_cam_found != (uint8_t *)self) {
            uint8_t *boom;
            memcpy(&boom, (uint8_t *)self + OFF_CAM_BOOM, sizeof(boom));
            logf_("camera: HM2StickCam at %p (boom %p) — adopted on "
                  "activation, current=%.1f preferred=%.1f", self, boom,
                  (double)fld32(self, OFF_CAM_CURRENT),
                  (double)fld32(self, OFF_CAM_PREFERRED));
        }
        InterlockedExchange(&g_cam_found, (LONG)(uintptr_t)self);
    }
    return r;
}

static void install_activate_hook(void)
{
    uint32_t *slot = (uint32_t *)(uintptr_t)VA_ACTIVATE_SLOT;
    if (*slot != VA_ACTIVATE_FN) {   /* another mod got here first? */
        logf_("camera: vtable slot %08x holds %08x (expected %08x) — "
              "not hooking activation, scanner only",
              VA_ACTIVATE_SLOT, *slot, VA_ACTIVATE_FN);
        return;
    }
    DWORD old;
    if (!VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &old)) {
        logf_("camera: VirtualProtect on the vtable slot failed — "
              "scanner only");
        return;
    }
    *slot = (uint32_t)(uintptr_t)activate_hook;
    VirtualProtect(slot, sizeof(*slot), old, &old);
    logf_("camera: activation hook installed (vtbl slot %08x)",
          VA_ACTIVATE_SLOT);
}

/* one-time gate: known retail exe, unpacked */
static int probe_exe(void)
{
    uint8_t *base = (uint8_t *)GetModuleHandleA(NULL);
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    IMAGE_NT_HEADERS32 *nt = (IMAGE_NT_HEADERS32 *)(base + dos->e_lfanew);
    if (nt->FileHeader.TimeDateStamp != H2_TIMESTAMP ||
        nt->OptionalHeader.SizeOfImage != H2_SIZEOFIMAGE) {
        logf_("camera: hitman2.exe is not the known retail build "
              "(stamp %08lx size %08lx) — AutoZoomOut off",
              (unsigned long)nt->FileHeader.TimeDateStamp,
              (unsigned long)nt->OptionalHeader.SizeOfImage);
        return 0;
    }
    if ((uintptr_t)base != 0x400000u) {   /* fixed VAs assume no reloc */
        logf_("camera: hitman2.exe relocated to %p — AutoZoomOut off", base);
        return 0;
    }
    if (memcmp((void *)(uintptr_t)SIG_VA, SIG, sizeof(SIG)) != 0)
        return -1;                        /* packed .text not unpacked yet */
    logf_("camera: retail exe unpacked, AutoZoomOut armed");
    return 1;
}

/* background scanner: find the HM2StickCamera allocation by vtable pointer
 * in the game's private committed heap. One live instance exists per scene,
 * but a scene change frees the old one WITHOUT wiping its bytes — a freed
 * copy can keep passing the field checks (even with its active flag frozen
 * to 1), so validity of the cached pointer is NOT enough to skip scanning:
 *
 *  - only an ACTIVE candidate is ever cached (an inactive one is useless —
 *    the per-frame writes are gated on the active flag anyway);
 *  - when several actives match (real one + stale bytes), the one whose
 *    boom timestamp is CLOSEST to the engine clock wins: the engine
 *    re-stamps the live boom every tick (and the per-frame backdate keeps
 *    it within the cooldown window of now), while a stale copy's stamp
 *    froze at its scene's end;
 *  - the scan repeats every RESCAN_MS even while the cache looks good, so
 *    a cached stale copy is corrected shortly after the new scene is up.
 *
 * Reads only; the present thread re-validates the candidate in place
 * before writing.
 *
 * Every read of scanned memory goes through ReadProcessMemory, never a
 * direct dereference: the game frees heap blocks on its main thread WHILE
 * the sweep runs (level load especially), and Wine's heap decommits freed
 * pages far more eagerly than the Windows heap — a region that was
 * MEM_COMMIT at VirtualQuery time can lose pages mid-sweep (observed as a
 * page-fault crash on level load under CrossOver; never bites on real
 * Windows, where the freed pages stay committed). RPM turns the vanished
 * page into a failed/short read. The present thread is exempt: frees
 * happen on that same thread, so nothing decommits under it. */
static int safe_read(const void *src, void *dst, size_t n)
{
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), src, dst, n, &got) &&
           got == n;
}

/* cam_valid + active-flag check, on RPM'd copies (scanner-thread version;
 * keep the field checks in sync with cam_valid above). Returns the boom
 * pointer, its stamp and the two distances for the caller's pick/logging
 * so the caller never touches the candidate's memory directly. */
static int cam_probe(uint8_t *cand, uint8_t **boom_out, uint32_t *stamp_out,
                     float *cur_out, float *pref_out)
{
    uint8_t camb[CAM_SIZE], boomb[BOOM_SIZE];
    uint32_t vptr;
    uint8_t *boom;
    if (!safe_read(cand, camb, sizeof(camb)))
        return 0;
    memcpy(&vptr, camb, sizeof(vptr));
    if (vptr != VA_STICKCAM_VTBL || !camb[OFF_CAM_ACTIVE])
        return 0;
    memcpy(&boom, camb + OFF_CAM_BOOM, sizeof(boom));   /* packed: unaligned */
    if (!safe_read(boom, boomb, sizeof(boomb)))
        return 0;
    float pref  = fld32(camb, OFF_CAM_PREFERRED);
    float near_ = fld32(boomb, OFF_BOOM_NEAROFF);
    float meas  = fld32(boomb, OFF_BOOM_MEASURED);
    if (!(pref >= 10.0f && pref <= 1000.0f &&
          near_ >= -1000.0f && near_ <= 1000.0f && meas >= 0.0f))
        return 0;
    *boom_out = boom;
    memcpy(stamp_out, boomb + OFF_BOOM_STAMP, sizeof(*stamp_out));
    *cur_out  = fld32(camb, OFF_CAM_CURRENT);
    *pref_out = pref;
    return 1;
}

/* sweep buffer: .bss of this module is MEM_IMAGE, so the sweep (which only
 * looks at MEM_PRIVATE) never sees its own copies of the vtable constant */
static uint8_t g_scan_buf[0x10000];

#define RESCAN_MS 3000
static DWORD WINAPI scan_thread(LPVOID arg)
{
    (void)arg;
    for (;;) {
        if (g_state == 0)
            return 0;
        if (g_state == 1) {
            uint8_t *found = NULL, *found_boom = NULL;
            uint32_t found_dist = 0xffffffffu;
            float found_cur = 0.0f, found_pref = 0.0f;
            uint32_t now_ms = 0;
            uint8_t *eng = *(uint8_t **)(uintptr_t)VA_ENGINE_DB;
            if (readable(eng, OFF_DB_TIME_MS + 4))
                memcpy(&now_ms, eng + OFF_DB_TIME_MS, sizeof(now_ms));
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
                /* skip our own stack: it can hold spilled copies of the
                 * vtable constant we are scanning for */
                if ((uint8_t *)&mbi >= base && (uint8_t *)&mbi < base + size)
                    ok = 0;
                SIZE_T off = 0;
                while (ok && off < (size & ~(SIZE_T)3)) {
                    SIZE_T want = (size & ~(SIZE_T)3) - off;
                    SIZE_T got = 0;
                    if (want > sizeof(g_scan_buf))
                        want = sizeof(g_scan_buf);
                    if (!ReadProcessMemory(GetCurrentProcess(), base + off,
                                           g_scan_buf, want, &got) && !got) {
                        /* Windows RPM is all-or-nothing across a hole:
                         * retry one page, then skip it if it is the hole */
                        want = 0x1000;
                        if (!ReadProcessMemory(GetCurrentProcess(),
                                               base + off, g_scan_buf,
                                               want, &got) && !got) {
                            off += 0x1000;
                            continue;
                        }
                    }
                    got &= ~(SIZE_T)3;
                    /* operator new returns 4-aligned blocks: the vptr sits
                     * at a 4-aligned address even in this packed engine */
                    for (SIZE_T i = 0; i + 4 <= got; i += 4) {
                        uint32_t v;
                        memcpy(&v, g_scan_buf + i, sizeof(v));
                        if (v != VA_STICKCAM_VTBL)
                            continue;
                        uint8_t *cand = base + off + i;
                        uint8_t *boom;
                        uint32_t stamp;
                        float cur, pref;
                        if (!cam_probe(cand, &boom, &stamp, &cur, &pref))
                            continue;
                        uint32_t d = now_ms - stamp;    /* distance to now */
                        if (d > 0x80000000u) d = 0u - d;
                        if (!found || d < found_dist) {
                            found = cand;
                            found_dist = d;
                            found_boom = boom;
                            found_cur = cur;
                            found_pref = pref;
                        }
                    }
                    off += got ? got : 0x1000;   /* keep 4-aligned progress */
                }
                p = base + size;
                if (p < base) break;                /* address wrap */
            }
            if (found &&
                (uint8_t *)(uintptr_t)g_cam_found != found) {
                logf_("camera: HM2StickCam at %p (boom %p), "
                      "current=%.1f preferred=%.1f", found, found_boom,
                      (double)found_cur, (double)found_pref);
            }
            if (found)
                InterlockedExchange(&g_cam_found, (LONG)(uintptr_t)found);
        }
        /* no active camera yet (menus, loading): retry quickly so the fix
         * is in place moments after a mission starts */
        {
            uint8_t *boom;
            uint32_t stamp;
            float cur, pref;
            if (!cam_probe((uint8_t *)(uintptr_t)g_cam_found,
                           &boom, &stamp, &cur, &pref))
                Sleep(1000);
            else
                Sleep(RESCAN_MS);
        }
    }
}

/* per frame, from the loader's Present hook (the game's main thread) */
void h2sa_camera_frame(void)
{
    if (!g_autozoom || g_state != 1)
        return;

    g_cam = (uint8_t *)(uintptr_t)g_cam_found;
    if (!cam_valid(g_cam) || !g_cam[OFF_CAM_ACTIVE])
        return;
    uint8_t *cam = g_cam;
    uint8_t *boom;
    memcpy(&boom, cam + OFF_CAM_BOOM, sizeof(boom));

    uint8_t *eng = *(uint8_t **)(uintptr_t)VA_ENGINE_DB;
    if (!readable(eng, OFF_DB_TIME_MS + 4))
        return;

    /* 1. expire the grow-back cooldown: the engine's own grow path then
     * raises the allowed length the first frame the way is clear */
    uint32_t now_ms;
    memcpy(&now_ms, eng + OFF_DB_TIME_MS, sizeof(now_ms));
    uint32_t backdated = now_ms - (GROW_COOLDOWN_MS + 1);
    memcpy(boom + OFF_BOOM_STAMP, &backdated, sizeof(backdated));

    /* 2. collision currently forcing the camera below the player's wheel
     * pick -> open fully once it clears (the wheel remains a temporary
     * override; scrolling again re-arms it) */
    float preferred = fld32(cam, OFF_CAM_PREFERRED);
    float measured  = fld32(boom, OFF_BOOM_MEASURED);
    float nearoff   = fld32(boom, OFF_BOOM_NEAROFF);
    if (measured - nearoff < preferred - 1.0f &&
        preferred < DIST_ZOOMED_OUT - 0.5f) {
        float out = DIST_ZOOMED_OUT;
        memcpy(cam + OFF_CAM_PREFERRED, &out, sizeof(out));
        if (g_reset_logs < 8) {
            g_reset_logs++;
            logf_("camera: auto zoom-in at %.1f (< preferred %.1f) — "
                  "preferred reset to %.0f",
                  (double)(measured - nearoff), (double)preferred,
                  (double)DIST_ZOOMED_OUT);
        }
    }

    /* episode timing (log-budgeted): measure how long the camera takes to
     * reopen after the obstruction clears — the number this feature is
     * about. States: 0 idle, 1 pulled in, 2 way clear + easing back out. */
    {
        static int st;
        static uint32_t clear_ms;
        static float ep_min;
        static int ep_logs;
        float current = fld32(cam, OFF_CAM_CURRENT);
        preferred = fld32(cam, OFF_CAM_PREFERRED);   /* after the reset */
        int pulled = current < preferred - 5.0f;
        int clear  = measured - nearoff >= preferred - 1.0f;
        switch (st) {
        case 0:
            if (pulled) { st = 1; ep_min = current; }
            break;
        case 1:
            if (current < ep_min) ep_min = current;
            if (!pulled) st = 0;                     /* reopened already */
            else if (clear) { st = 2; clear_ms = now_ms; }
            break;
        case 2:
            if (!clear) st = 1;                      /* blocked again */
            else if (!pulled) {
                if (ep_logs < 16) {
                    ep_logs++;
                    logf_("camera: reopened to %.0f in %lu ms after "
                          "clearing (was pulled to %.0f)", (double)preferred,
                          (unsigned long)(now_ms - clear_ms), (double)ep_min);
                }
                st = 0;
            }
            break;
        }
    }
}

static DWORD WINAPI arm_thread(LPVOID arg)
{
    (void)arg;
    /* wait for the packed exe to unpack itself (the unpacker runs before
     * the renderer loads, so this normally succeeds within a second) */
    for (int i = 0; i < 1200 && g_state < 0; i++) {   /* up to 5 minutes */
        g_state = probe_exe();
        if (g_state < 0)
            Sleep(250);
    }
    if (g_state < 0) {
        logf_("camera: exe never unpacked to the known image — "
              "AutoZoomOut off");
        g_state = 0;
        return 0;
    }
    if (g_state == 1) {
        install_activate_hook();
        scan_thread(NULL);
    }
    return 0;
}

void h2sa_camera_init(void)
{
    FILE *f = fopen(h2sa_core_ini, "r");
    if (f) {
        char line[128], section[32] = "";
        int b;
        while (fgets(line, sizeof(line), f)) {
            char s[32];
            if (sscanf(line, " [%31[^]]]", s) == 1) {
                lstrcpynA(section, s, sizeof(section));
                continue;
            }
            if (_stricmp(section, "Camera") != 0)
                continue;
            if (sscanf(line, " AutoZoomOut = %d", &b) == 1 ||
                sscanf(line, " AutoZoomOut=%d", &b) == 1)
                g_autozoom = (b != 0);
        }
        fclose(f);
    }
    logf_("camera: AutoZoomOut=%d", g_autozoom);
    if (g_autozoom) {
        HANDLE t = CreateThread(NULL, 0, arm_thread, NULL, 0, NULL);
        if (t) {
            SetThreadPriority(t, THREAD_PRIORITY_BELOW_NORMAL);
            CloseHandle(t);
        }
    }
}

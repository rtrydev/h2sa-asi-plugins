/* H2SADump.asi — one-shot memory dumper for the packed hitman2.exe.
 *
 * hitman2.exe ships packed: a single writable, max-entropy .text unpacked by
 * a stub at startup, so its real code exists only in memory. To translate its
 * x87 (the game-logic float math that Rosetta emulates) we first need the
 * UNPACKED image. This ASI waits until the game is clearly running (the
 * renderer has loaded and a settle delay has passed, well after the unpacker
 * finished), then writes hitman2.exe's mapped image to disk.
 *
 * Output: scripts/hitman2_dump.bin  — the raw in-memory image, byte i =
 *         *(base+i). tools/undump.py rewrites the section headers
 *         (PointerToRawData=VirtualAddress) so pefile/analysis can read it.
 * Log:    scripts/H2SADump.log — base, SizeOfImage, entry, bytes written.
 *
 * Not part of the default build. Build with `make dump`, drop the ASI in
 * scripts/, launch once, then remove it.
 */
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>

static FILE *g_log;
static char  g_dir[MAX_PATH];

static void logf_(const char *fmt, ...)
{
    if (!g_log) return;
    va_list ap; va_start(ap, fmt);
    vfprintf(g_log, fmt, ap); va_end(ap);
    fputc('\n', g_log); fflush(g_log);
}

/* write [base, base+size) to file, page by page; uncommitted/unreadable
 * pages are written as zeros so file offset stays == RVA. */
static int dump_image(uint8_t *base, uint32_t size, const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    static uint8_t zero[0x1000];
    uint32_t off = 0;
    uint32_t written = 0, holes = 0;
    while (off < size) {
        uint8_t *p = base + off;
        MEMORY_BASIC_INFORMATION mbi;
        uint32_t chunk = 0x1000;
        if (VirtualQuery(p, &mbi, sizeof(mbi)) == sizeof(mbi)) {
            int readable = (mbi.State == MEM_COMMIT) &&
                !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) &&
                (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                                PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                                PAGE_EXECUTE_WRITECOPY | PAGE_EXECUTE));
            if (readable) {
                uint32_t region_left = (uint32_t)((uint8_t *)mbi.BaseAddress +
                                       mbi.RegionSize - p);
                chunk = region_left < 0x1000 ? region_left : 0x1000;
                if (off + chunk > size) chunk = size - off;
                fwrite(p, 1, chunk, f);
                written += chunk;
            } else {
                fwrite(zero, 1, 0x1000, f);
                holes += 0x1000;
                chunk = 0x1000;
            }
        } else {
            fwrite(zero, 1, 0x1000, f);
            holes += 0x1000;
        }
        off += chunk;
    }
    fclose(f);
    logf_("dumped %u bytes (%u committed, %u zero-filled holes) -> %s",
          off, written, holes, path);
    return 1;
}

static DWORD WINAPI worker(LPVOID arg)
{
    (void)arg;
    /* wait for the renderer (game fully up => unpacker long done) */
    for (int i = 0; i < 240; i++) {          /* up to 60s */
        if (GetModuleHandleA("RenderD3D.dll") ||
            GetModuleHandleA("RenderOpenGL.dll")) break;
        Sleep(250);
    }
    Sleep(8000);                              /* settle */

    HMODULE h = GetModuleHandleA("hitman2.exe");
    if (!h) h = GetModuleHandleA(NULL);       /* the exe is the main module */
    uint8_t *base = (uint8_t *)h;
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    IMAGE_NT_HEADERS32 *nt = (IMAGE_NT_HEADERS32 *)(base + dos->e_lfanew);
    uint32_t soi = nt->OptionalHeader.SizeOfImage;
    uint32_t ep  = nt->OptionalHeader.AddressOfEntryPoint;
    logf_("hitman2.exe base=%p SizeOfImage=0x%x entry_rva=0x%x sections=%u",
          (void *)base, soi, ep, nt->FileHeader.NumberOfSections);

    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s\\hitman2_dump.bin", g_dir);
    dump_image(base, soi, path);
    logf_("DONE. Run tools/undump.py on hitman2_dump.bin.");
    return 0;
}

static void init(HMODULE self)
{
    GetModuleFileNameA(self, g_dir, sizeof(g_dir));
    char *sl = strrchr(g_dir, '\\');
    if (sl) *sl = 0;
    char logp[MAX_PATH];
    snprintf(logp, sizeof(logp), "%s\\H2SADump.log", g_dir);
    g_log = fopen(logp, "w");
    logf_("H2SADump loaded; waiting for game to unpack...");
    CreateThread(NULL, 0, worker, NULL, 0, NULL);
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        init((HMODULE)inst);
    }
    return TRUE;
}

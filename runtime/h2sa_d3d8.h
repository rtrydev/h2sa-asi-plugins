/* Shared interface between the H2SA d3d8.dll loader and the ASI plugins
 * that want to influence Direct3D 8 device creation and rendering.
 *
 * Hitman 2: Silent Assassin is a Direct3D 8 title: hitman2.exe imports
 * only kernel32 and loads RenderD3D.dll at runtime, which in turn imports
 * Direct3DCreate8 from d3d8.dll. The loader ships as a d3d8.dll proxy, so
 * it is the natural place to intercept the D3D8 COM interface. Rather than
 * byte-patch RenderD3D.dll (build-specific and fragile), plugins register
 * callbacks here and the loader installs them on the real D3D8 vtables.
 *
 * A plugin registers by resolving H2SA_RegisterD3D8Hooks from the loaded
 * d3d8.dll in its own DllMain (the loader has already been mapped by the
 * time it loads the plugin), e.g.:
 *
 *     HMODULE ld = GetModuleHandleA("d3d8.dll");
 *     h2sa_register_fn reg = (h2sa_register_fn)
 *         GetProcAddress(ld, "H2SA_RegisterD3D8Hooks");
 *     if (reg) reg(&my_hooks);
 */
#ifndef H2SA_D3D8_H
#define H2SA_D3D8_H

#include <d3d8.h>

#define H2SA_D3D8_HOOKS_VERSION 3

typedef struct H2SAD3D8Hooks {
    unsigned int version;   /* set to H2SA_D3D8_HOOKS_VERSION */

    /* Called just before IDirect3D8::CreateDevice and IDirect3DDevice8::
     * Reset forward to the real implementation, so the plugin can rewrite
     * the presentation parameters (e.g. force windowed/borderless).
     * is_reset is 0 for CreateDevice, 1 for Reset. hFocusWindow is the
     * focus window passed to CreateDevice (NULL on Reset). */
    void (*fix_present)(D3DPRESENT_PARAMETERS *pp, HWND hFocusWindow,
                        int is_reset);

    /* Called for every IDirect3DDevice8::SetTransform whose state is
     * D3DTS_PROJECTION, on a private copy the loader then forwards, so the
     * plugin can apply an aspect-correct FOV without touching game code.
     * bbw/bbh are the backbuffer size the device was created/reset with. */
    void (*fix_projection)(D3DMATRIX *m, unsigned int bbw, unsigned int bbh);

    /* Optional: called once with the real device right after a successful
     * CreateDevice (may be NULL). */
    void (*on_device)(IDirect3DDevice8 *dev);

    /* Optional (may be NULL): called right after each successful
     * IDirect3DDevice8::Present returns, i.e. once per displayed frame. Used
     * to pace the frame rate — Hitman 2's engine ties simulation to frame
     * time, so an uncapped modern GPU makes it run wild; the plugin sleeps
     * here to hold a target FPS. */
    void (*on_present)(void);

    /* Optional (may be NULL): called with the live device just BEFORE each
     * IDirect3DDevice8::Present forwards to the real implementation — i.e.
     * once per frame while the finished back buffer is still intact. This is
     * where overlays draw (an on_present draw would land after the frame is
     * already shown). The scene has ended by this point; a plugin that draws
     * must save/restore device state around its own draw calls. */
    void (*on_frame)(IDirect3DDevice8 *dev);
} H2SAD3D8Hooks;

/* Multiple plugins may register; the loader keeps every hook set and invokes
 * each non-NULL callback in registration order. fix_present / fix_projection
 * are applied in turn (each sees the previous plugin's edits). */

typedef void (WINAPI *h2sa_register_fn)(const H2SAD3D8Hooks *hooks);

#endif /* H2SA_D3D8_H */

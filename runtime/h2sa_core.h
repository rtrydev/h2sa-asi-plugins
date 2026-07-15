/* Internals shared between the translation units of h2sa_core.asi
 * (widescreen.c + profiler.c). widescreen.c owns the DllMain, the log file
 * (scripts/h2sa_core.log) and the config path (scripts/h2sa_core.ini); the
 * profiler is initialized from there and shares both. The single ini holds a
 * [Widescreen] and a [Profiler] section — each parser only consumes keys
 * from its own section.
 */
#ifndef H2SA_CORE_H
#define H2SA_CORE_H

#include <windows.h>

extern char h2sa_core_dir[MAX_PATH];   /* the scripts/ directory */
extern char h2sa_core_ini[MAX_PATH];   /* scripts/h2sa_core.ini  */

void h2sa_core_logf(const char *fmt, ...);

void h2sa_profiler_init(HMODULE self);
void h2sa_profiler_detach(void);

/* camera.c — auto zoom-out for the third-person camera ([Camera] section).
 * init parses the ini; frame runs once per displayed frame from
 * widescreen.c's on_present hook (the game's main thread). */
void h2sa_camera_init(void);
void h2sa_camera_frame(void);

/* uiscale.c — believed-resolution UI scaling. widescreen.c forwards the
 * [Widescreen] UIScale config, decides the backbuffer size in fix_present
 * and calls setup/off; the fix_viewport hook goes into widescreen.c's v4
 * hook registration (declared there — it is D3D-typed). The profiler
 * multiplies its glyph size by h2sa_uiscale_k(). */
void  h2sa_uiscale_config(float uiscale);
float h2sa_uiscale_cfg(void);
int   h2sa_uiscale_wanted(void);
int   h2sa_uiscale_rebelieve(int rw, int rh, int lw, int lh);
void  h2sa_uiscale_setup(int ini_w, int ini_h,
                         unsigned bb_w, unsigned bb_h);
void  h2sa_uiscale_off(void);
float h2sa_uiscale_k(void);

#endif /* H2SA_CORE_H */

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

#endif /* H2SA_CORE_H */

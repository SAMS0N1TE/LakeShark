/* Force-included ONLY for app sources in host builds. Include the host CRT
   before redirecting names so Windows dllimport declarations stay untouched.
   Firmware's ELF resolver maps the same four names to generation allocations.
   Synchronous calls also accept the active callback stack. Only retained
   pointers (SD read/list destinations) require these allocations or statics. */
#ifndef LAP_SIM_ALLOC_H
#define LAP_SIM_ALLOC_H
#include <stdlib.h>
void *ls_lap_malloc(size_t);
void *ls_lap_calloc(size_t, size_t);
void *ls_lap_realloc(void *, size_t);
void ls_lap_free(void *);
#define malloc ls_lap_malloc
#define calloc ls_lap_calloc
#define realloc ls_lap_realloc
#define free ls_lap_free
#endif

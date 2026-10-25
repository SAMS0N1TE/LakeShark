/* Recoverable replacement for FAT, whose rename cannot overwrite a destination.
 * The old file is retained under a hidden name until the new rename succeeds.
 * Boot recovery restores the old file if power failed between the two renames.
 * This is recoverable, not a power-loss-atomic FAT transaction. Worker/boot only. */
#ifndef LS_LAP_STORAGE_H
#define LS_LAP_STORAGE_H
#include <stdbool.h>
bool ls_lap_replace(const char *incoming, const char *target);
bool ls_lap_recover_dir(const char *dir);
#endif

/* BUGHUNT's name lookups: the shared engine in app-sdk/lib/lap_ids.h (one
   card job at a time, answers kept, the owner's names first). A name asked
   for the first time comes back NULL and turns up a frame or two later. */
#ifndef BH_LOOKUP_H
#define BH_LOOKUP_H
#include "bh_ids.h"

#define BHL_FILE1 LAP_IDS_FILE2         /* bughunt-ids.bin from PACKS; ble-ids.bin is read first */
#define BHL_FILE2 LAP_IDS_FILE3         /* put by hand */

#define bhl_open lap_ids_open
#define bhl_close lap_ids_close
#define bhl_pump lap_ids_pump
#define bhl_get lap_ids_get
#define bhl_maker lap_ids_maker
#define bhl_state lap_ids_state
#endif

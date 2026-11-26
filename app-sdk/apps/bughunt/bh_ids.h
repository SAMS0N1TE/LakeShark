/* BUGHUNT's device names. The reader moved to the shared library
   (app-sdk/lib/lap_ids.h) so every BLE app reads the same file and the
   owner's own names; these are BUGHUNT's old names for it. */
#ifndef BH_IDS_H
#define BH_IDS_H
#include "lap_ids.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { BHID_COMPANY = LAP_IDS_COMPANY, BHID_UUID = LAP_IDS_SERVICE, BHID_OUI = LAP_IDS_OUI,
       BHID_APPEARANCE = LAP_IDS_APPEARANCE, BHID_KINDS = LAP_IDS_KINDS };
#define BHID_TABLES LAP_IDS_TABLES
#define BHID_HEAD_MAX LAP_IDS_HEAD_MAX
#define BHID_NAME LAP_IDS_NAME
#define BHID_IDX_MAX LAP_IDS_IDX_MAX

typedef lap_ids_tab_t bhid_tab_t;
typedef lap_ids_head_t bhid_head_t;

#define bhid_head lap_ids_head
#define bhid_table lap_ids_table
#define bhid_block lap_ids_block
#define bhid_in_block lap_ids_in_block
#define bhid_name_fix lap_ids_name_fix

/* Makers whose devices are cameras first: Hikvision, Dahua, Wyze, Ring ... */
bool bhid_camera_maker(const char *name);
#endif

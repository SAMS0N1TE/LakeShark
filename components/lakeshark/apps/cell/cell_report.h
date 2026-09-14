/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef LS_CELL_REPORT_H
#define LS_CELL_REPORT_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "cell_monitor.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Wire protocol CW1: 32 explicitly little-endian bytes, standard base64.
 * Coordinate sentinels are INT32_MIN. No subscriber identifiers. */
typedef struct {
    uint32_t boot,sequence,epoch,frequency_khz;
    int32_t lat_e5,lon_e5;
    uint16_t changed;
    int16_t pci;
    int8_t rise;
    uint8_t band,kind,flags;
} cell_report_record_t;
bool cell_report_encode(const cell_report_record_t *r,char *text,size_t size);
void cell_report_init(void);
void cell_report_observe(const cell_status_t *s);
void cell_report_activity(const cell_status_t *s,char kind);
bool cell_report_target(const char *peer);
void cell_report_status(char *text,size_t size);
int cell_report_command(int argc,char **argv);
/* Transport integration. queued != sent != recipient acknowledged. */
bool cell_report_transport_known(const char *peer);
bool cell_report_transport_send(const char *peer,const char *text);
int cell_report_transport_state(const char *peer,const char *text);
#ifdef __cplusplus
}
#endif
#endif

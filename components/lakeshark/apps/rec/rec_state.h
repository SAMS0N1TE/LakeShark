#ifndef REC_STATE_H
#define REC_STATE_H

/*LS-500*/

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define REC_RTL_RATE      256000u
#define REC_DEFAULT_FREQ  433920000UL
#define REC_DEFAULT_GAIN  400

#define REC_MAX_EDGES     4096
#define REC_MIN_PULSE_US  40
#define REC_GAP_END_US    30000
#define REC_MIN_EDGES     6
#define REC_MAX_SPAN_US   8000000

typedef enum {
    REC_IDLE = 0,
    REC_ARMED,
    REC_CAPTURING,
    REC_DONE,
} rec_phase_t;

/*LS-517*/
typedef enum {
    REC_END_NONE = 0,
    REC_END_GAP,
    REC_END_SPAN,
    REC_END_EDGES,
} rec_end_t;

typedef struct {
    rec_phase_t phase;
    uint32_t    freq_hz;
    int         gain_tenths;
    int         edges;
    uint32_t    span_us;
    int         mag_now;
    int         mag_floor;
    int         mag_thresh;
    int         thresh_fixed;
    int         gap_ms;
    uint32_t    captures;
    uint32_t    bytes_sec;
    char        last_file[40];
    /*LS-516*/
    uint32_t    bw_hz;
    uint32_t    min_pulse_us;
    uint32_t    max_span_us;
    int         min_edges;
    /*LS-517*/
    int         end_reason;
    uint32_t    min_mark_us;
    uint32_t    max_mark_us;
    uint32_t    baud_est;
} rec_status_t;

int  rec_app_register(void);

void rec_get_status(rec_status_t *out);
void rec_set_freq(uint32_t hz);
uint32_t rec_get_freq(void);
void rec_set_gain(int tenths);
/*LS-503*/
void rec_set_thresh(int absolute);
int  rec_get_thresh(void);
/*LS-504*/
void rec_set_gap_ms(int ms);
int  rec_get_gap_ms(void);

/*LS-516*/
void     rec_set_bw(uint32_t hz);
uint32_t rec_get_bw(void);
void     rec_set_min_pulse(uint32_t us);
uint32_t rec_get_min_pulse(void);
void     rec_set_max_span(uint32_t us);
uint32_t rec_get_max_span(void);
void     rec_set_min_edges(int n);
int      rec_get_min_edges(void);

/*LS-517*/
const char *rec_end_reason_name(int reason);

bool rec_active(void);
void rec_arm(void);
/*LS-506*/
void rec_arm_request(void);
void rec_disarm(void);

/*LS-507*/
uint32_t rec_bytes_sec(void);

/*LS-508*/
int rec_edge_count(void);
int rec_edges_copy(int from, int32_t *out, int max);

int  rec_save(const char *name, char *path_out, size_t path_len);

int  rec_list(char *out, size_t len);
int  rec_dump(const char *name, void (*emit)(const char *line, void *ctx), void *ctx);
int  rec_remove(const char *name);

#ifdef __cplusplus
}
#endif

#endif

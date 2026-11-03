/* Private Radio RX API 2.4 worker/capture boundary. */
#ifndef LS_LAP_RX24_H
#define LS_LAP_RX24_H
typedef struct rx24 rx24_t;
static rx24_t *rx24_create(void);
static void rx24_fail(rx24_t *x, int error);
static void rx24_destroy(rx24_t *x);
static void rx24_gain(rx24_t *x, int gain);
static void rx24_mark(rx24_t *x, uint32_t flags);
static void rx24_push(rx24_t *x, const int16_t *iq, size_t pairs, uint64_t first, uint64_t center, uint32_t rate);
static void rx24_work(rx24_t *x, ls_lap_rx_actual_t *actual);
static void rx24_refresh(const ls_lap_rx_backend_t *backend, int n);
#endif

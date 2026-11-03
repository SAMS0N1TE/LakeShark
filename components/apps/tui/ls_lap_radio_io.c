#include "ls_lap_context.h"
/* API 2.4 radio I/O. Mailboxes contain copies, all SD/bus operations run on
 * lap_io. The lock protects short copies only, never a bus/file operation.
 * Capture producers try-lock and drop rather than blocking real-time tasks. */
#include "ls_lap_radio_io.h"
#include "ls_lap_api.h"
#include "ls_lap_sys.h"
#include "ls_lap_features.h"
#include "ls_nfc.h"
#include <errno.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#include <fcntl.h>
#include <io.h>
#define MKDIR(p) _mkdir(p)
#else
#define MKDIR(p) mkdir(p, 0777)
#endif

/* fopen(path, "wbx"): a new file, EEXIST for one already there. The host
   harness's Windows C runtime has no "x" mode, so it gets the same from
   _open(O_EXCL). */
static FILE *create_new(const char *path)
{
#ifdef _WIN32
    const int fd = _open(path, _O_CREAT | _O_EXCL | _O_WRONLY | _O_BINARY, _S_IREAD | _S_IWRITE);
    return fd < 0 ? NULL : _fdopen(fd, "wb");
#else
    return fopen(path, "wbx");
#endif
}
#ifdef ESP_PLATFORM
#include "esp_attr.h"
#include "esp_timer.h"
#else
#define EXT_RAM_BSS_ATTR
#endif
#ifndef LS_LAP_SD_ROOT
#define LS_LAP_SD_ROOT "/sdcard"
#endif
#define Q 8
#define BLOCK 2048
#define TX_BYTES 65534
static LS_LAP_POOL atomic_uint p4_tap_drops_slots[LS_LAP_CONTEXTS];
#define p4_tap_drops (p4_tap_drops_slots[ls_lap_context_index()])

static LS_LAP_POOL atomic_int p4_file_fault_slots[LS_LAP_CONTEXTS];
#define p4_file_fault (p4_file_fault_slots[ls_lap_context_index()])

static LS_LAP_POOL atomic_bool p4_physical_tx_slots[LS_LAP_CONTEXTS];
#define p4_physical_tx (p4_physical_tx_slots[ls_lap_context_index()])

void ls_lap_io_file_fault(int error) { atomic_store(&p4_file_fault, error); }
#if defined(ESP_PLATFORM) && !defined(LS_LAP_HOST_TEST)
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
static StaticSemaphore_t guard_storage;
static SemaphoreHandle_t guard_mutex;
static atomic_int guard_init;
static void lk(void) {
    if (atomic_load_explicit(&guard_init, memory_order_acquire) != 2) {
        int expected = 0;
        if (atomic_compare_exchange_strong(&guard_init, &expected, 1)) {
            guard_mutex = xSemaphoreCreateMutexStatic(&guard_storage);
            atomic_store_explicit(&guard_init, 2, memory_order_release);
        } else
            while (atomic_load_explicit(&guard_init, memory_order_acquire) != 2)
                vTaskDelay(1);
    }
    xSemaphoreTake(guard_mutex, portMAX_DELAY);
}
static void ul(void) { xSemaphoreGive(guard_mutex); }
static bool try_lock(void) {
    return atomic_load_explicit(&guard_init, memory_order_acquire) == 2 &&
           xSemaphoreTake(guard_mutex, 0) == pdTRUE;
}
#else
static atomic_flag guard = ATOMIC_FLAG_INIT;
static void lk(void) {
    while (atomic_flag_test_and_set_explicit(&guard, memory_order_acquire)) {
    }
}
static void ul(void) { atomic_flag_clear_explicit(&guard, memory_order_release); }
static bool try_lock(void) {
    return !atomic_flag_test_and_set_explicit(&guard, memory_order_acquire);
}
#endif
static uint64_t now_us(void) {
#ifdef ESP_PLATFORM
    return esp_timer_get_time();
#else
    extern uint32_t (*ls_lap_api_host_clock)(void);
    return ls_lap_api_host_clock ? (uint64_t)ls_lap_api_host_clock() * 1000u : 0;
#endif
}
static bool ptr(const void *p, size_t n) {
    return ls_lap_api_inside() && ls_lap_api_readable(p, n);
}
static int grant(unsigned g) {
    return !ls_lap_api_inside() ? LAP_RX_EINVAL : !(ls_lap_granted() & g) ? LAP_RX_EACCES : 0;
}
static int next_id;
static int handle(void) {
    if (next_id == INT_MAX)
        next_id = 1;
    else
        ++next_id;
    return next_id;
}
static bool strcopy(char *out, const char *in, size_t cap) {
    for (size_t i = 0; i < cap; i++) {
        if (!ptr(in + i, 1))
            return false;
        out[i] = in[i];
        if (!out[i])
            return true;
    }
    return false;
}
static bool config(lap_packet_config_t *out, const lap_packet_config_t *in) {
    if (!ptr(in, sizeof(*in)))
        return false;
    *out = *in;
    return out->size == sizeof(*out) && out->version == 1 && out->frequency_hz &&
           out->frequency_hz <= UINT64_C(6000000000) && out->modulation >= LAP_MOD_LORA &&
           out->modulation <= LAP_MOD_OOK && out->port <= LAP_PORT_RX_ONLY && !out->reserved[0] &&
           !out->reserved[1] && out->crc <= 1 &&
           (out->modulation == LAP_MOD_LORA
                ? (out->sf >= 5 && out->sf <= 12 && out->coding_rate >= 5 &&
                   out->coding_rate <= 8 && out->bandwidth_hz && out->sync_bits <= 8)
                : (out->bitrate >= (out->modulation==LAP_MOD_FSK?500u:600u) && out->bitrate <= 2000000 && out->bandwidth_hz &&
                   out->sync_bits >= 1 &&
                   out->sync_bits <= (out->modulation == LAP_MOD_OOK ? 16 : 32)));
}
static unsigned modbit(const lap_packet_config_t *c) { return 1u << (c->modulation - 1); }
typedef struct {
    lap_packet_meta_t meta;
    unsigned bytes, rate;
    uint8_t data[BLOCK];
} block_t;
static EXT_RAM_BSS_ATTR struct {
    int id, kind, radio; /* kind: 1 packet, 2 edges, 3 scanner, 4 virtual replay */
    lap_packet_config_t cfg;
    lap_io_status_t status;
    lap_packet_meta_t rssi;
    block_t q[Q];
    unsigned head, tail, seq;
    lap_scan_config_t scan;
    lap_scan_status_t ss;
    lap_packet_meta_t hits[16];
    unsigned hh, ht;
    unsigned control;
    uint64_t skip;
} p4_rx_slots[LS_LAP_CONTEXTS];
#define p4_rx (p4_rx_slots[ls_lap_context_index()])

static EXT_RAM_BSS_ATTR struct {
    int id, radio, format;
    lap_packet_config_t cfg;
    unsigned bytes, rate;
    char name[33];
    uint8_t data[TX_BYTES];
    lap_io_status_t status;
    bool cancel;
    unsigned vga; bool amp;
} p4_tx_slots[LS_LAP_CONTEXTS];
#define p4_tx (p4_tx_slots[ls_lap_context_index()])

static EXT_RAM_BSS_ATTR struct {
    int id;
    bool write;
    char bank[33];
    uint32_t first;
    int count;
    lap_channel_t rows[64];
    lap_io_status_t status;
} p4_db_slots[LS_LAP_CONTEXTS];
#define p4_db (p4_db_slots[ls_lap_context_index()])

static EXT_RAM_BSS_ATTR struct {
    int id;
    lap_record_config_t cfg;
    lap_packet_config_t packet;
    lap_record_status_t status;
    bool replay, stop, seek;
    uint64_t target;
    char root[128];
    block_t q[Q];
    unsigned head, tail;
} p4_rec_slots[LS_LAP_CONTEXTS];
#define p4_rec (p4_rec_slots[ls_lap_context_index()])

#if LAP_NFC
static EXT_RAM_BSS_ATTR struct {
    int id;
    lap_io_status_t status;
    lap_nfc_event_t ev[Q];
    uint8_t data[Q][2048];
    unsigned head, tail;
    bool write;
    unsigned len;
    uint8_t pending[2048];
} p4_nfc_slots[LS_LAP_CONTEXTS];
#define p4_nfc (p4_nfc_slots[ls_lap_context_index()])
#endif

static LS_LAP_POOL char p4_app_id_slots[LS_LAP_CONTEXTS][32];
#define p4_app_id (p4_app_id_slots[ls_lap_context_index()])

bool ls_lap_io_radio_busy(void) {
    lk();
    bool busy=false;
    for(unsigned i=0;i<LS_LAP_CONTEXTS;i++) if(
        (p4_rx_slots[i].id&&p4_rx_slots[i].kind!=4) ||
        (p4_rec_slots[i].id&&p4_rec_slots[i].replay&&p4_rec_slots[i].status.state<=LAP_IO_RUNNING) ||
        atomic_load(&p4_physical_tx_slots[i]) || p4_tx_slots[i].status.state==LAP_IO_QUEUED || p4_tx_slots[i].status.state==LAP_IO_RUNNING) busy=true;
    ul();
    return busy;
}
static bool valid(int h, int id) { return h > 0 && h == id; }
static lap_io_status_t status_init(void) {
    return (lap_io_status_t){.size = sizeof(lap_io_status_t), .version = 1, .state = LAP_IO_QUEUED};
}
static lap_record_status_t record_init(void) {
    return (lap_record_status_t){
        .size = sizeof(lap_record_status_t), .version = 1, .state = LAP_IO_QUEUED};
}
static void stamp(lap_packet_meta_t *m) {
    m->size = sizeof(*m);
    m->version = 1;
    if (!m->timestamp_us)
        m->timestamp_us = now_us();
}
int lap_radio_modes(int radio, lap_radio_modes_t *out) {
    if (!ptr(out, sizeof(*out)))
        return LAP_RX_EINVAL;
    lap_radio_modes_t m = {.size = sizeof(m), .version = 1};
    int e = ls_lap_io_backend()->modes(radio, &m);
    if (!LAP_HACKRF_TX) {
        lap_radio_info_t info;
        if (ls_lap_radio_snapshot(radio, &info) >= 0 && info.kind == LAP_RADIO_KIND_HACKRF)
            m.modes &= ~(LAP_MODE_TX_IQ | LAP_MODE_TX_PACKET | LAP_MODE_TX_EDGES);
    }
    if (e >= 0)
        *out = m;
    return e;
}
uint32_t ls_lap_io_caps(void) {
    uint32_t flags = LAP_HW_RECORD;
    for (int i = 0; i < LAP_RADIOS_MAX; i++) {
        lap_radio_modes_t m = {0};
        if (ls_lap_io_backend()->modes(i, &m) < 0)
            continue;
        if (m.modes & 7)
            flags |= LAP_HW_PACKET;
        if (m.modes & LAP_MODE_EDGES)
            flags |= LAP_HW_EDGES;
    }
    return flags | (LAP_NFC ? ls_lap_io_backend()->nfc_caps() : 0);
}
static int open_rx(int radio, const lap_packet_config_t *cfg, int kind,
                   const lap_scan_config_t *scan) {
    int e = grant(LAP_CAP_RADIO_RX);
    if (e)
        return e;
    lap_packet_config_t c;
    if (!config(&c, cfg) || c.frequency_hz>UINT32_MAX)
        return LAP_RX_EINVAL;
    lap_radio_modes_t modes;
    if ((e = lap_radio_modes(radio, &modes)) < 0)
        return e;
    if (!(modes.modes & modbit(&c)) || (kind == 2 && !(modes.modes & LAP_MODE_EDGES)))
        return LAP_RX_EUNSUPPORTED;
    if (ls_lap_rx_physical_busy())
        return LAP_RX_EBUSY;
    lk();
    if (atomic_load(&p4_physical_tx) || p4_rx.id || p4_tx.status.state == LAP_IO_RUNNING ||
        p4_tx.status.state == LAP_IO_QUEUED) {
        ul();
        return LAP_RX_EBUSY;
    }
    if(!ls_lap_resource_claim(LS_LAP_LEASE_PACKET,true)) { ul(); return LAP_RX_EBUSY; }
    memset(&p4_rx, 0, sizeof(p4_rx));
    p4_rx.id = handle();
    p4_rx.radio = radio;
    p4_rx.kind = kind;
    p4_rx.cfg = c;
    p4_rx.status = status_init();
    if (scan) {
        p4_rx.scan = *scan;
        p4_rx.ss = (lap_scan_status_t){.size = sizeof(p4_rx.ss), .version = 1, .state = LAP_IO_QUEUED};
    }
    int h = p4_rx.id;
    ul();
    return h;
}
int lap_packet_open(int radio, const lap_packet_config_t *cfg) {
    return open_rx(radio, cfg, 1, NULL);
}
int lap_edges_open(int radio, const lap_packet_config_t *cfg) {
    if (!ptr(cfg, sizeof(*cfg)) || cfg->modulation != LAP_MOD_OOK)
        return LAP_RX_EINVAL;
    return open_rx(radio, cfg, 2, NULL);
}
int lap_packet_status(int h, lap_io_status_t *out) {
    if (!ptr(out, sizeof(*out)))
        return LAP_RX_EINVAL;
    lk();
    int e = valid(h, p4_rx.id) ? 0 : LAP_RX_EINVAL;
    if (!e) {
        *out = p4_rx.status;
        out->queued = p4_rx.head - p4_rx.tail;
    }
    ul();
    return e;
}
static int read_rx(int h, void *data, size_t cap, lap_packet_meta_t *meta, bool edges) {
    int e = grant(LAP_CAP_RADIO_RX);
    if (e)
        return e;
    if (!ptr(meta, sizeof(*meta)) || cap > BLOCK || !ptr(data, cap))
        return LAP_RX_EINVAL;
    lk();
    if (!valid(h, p4_rx.id) || (edges ? p4_rx.kind != 2 && p4_rx.kind != 4 : p4_rx.kind != 1 && p4_rx.kind != 4)) {
        ul();
        return LAP_RX_EINVAL;
    }
    if (p4_rx.head == p4_rx.tail) {
        e = p4_rx.status.error;
        ul();
        return e;
    }
    block_t *b = &p4_rx.q[p4_rx.tail % Q];
    if (cap < b->bytes) {
        ul();
        return LAP_IO_ESIZE;
    }
    memcpy(data, b->data, b->bytes);
    *meta = b->meta;
    int n = b->bytes;
    p4_rx.tail++;
    ul();
    return edges ? n / 4 : n;
}
int lap_packet_read(int h, void *data, size_t cap, lap_packet_meta_t *meta) {
    return read_rx(h, data, cap > BLOCK ? BLOCK : cap, meta, false);
}
int lap_edges_read(int h, int32_t *data, int max, lap_packet_meta_t *meta) {
    if (max < 1 || max > 256)
        return LAP_RX_EINVAL;
    return read_rx(h, data, (size_t)max * 4, meta, true);
}
void lap_packet_close(int h) {
    if (!ls_lap_api_inside())
        return;
    lk();
    if (valid(h, p4_rx.id))
        p4_rx.id = 0;
    ul();
}
int lap_radio_rssi(int h, lap_packet_meta_t *out) {
    if (!ptr(out, sizeof(*out)))
        return LAP_RX_EINVAL;
    lk();
    int e = !valid(h, p4_rx.id) ? LAP_RX_EINVAL : !p4_rx.rssi.size ? LAP_RX_EAGAIN : 0;
    if (!e)
        *out = p4_rx.rssi;
    ul();
    return e;
}
int lap_scan_start(const lap_scan_config_t *cfg) {
    if (!ptr(cfg, sizeof(*cfg)) || cfg->size != sizeof(*cfg) || cfg->version != 1 ||
        cfg->count < 1 || cfg->count > 64 || cfg->dwell_ms < 20 || cfg->dwell_ms > 10000 ||
        cfg->hang_ms > 10000)
        return LAP_RX_EINVAL;
    lap_scan_config_t c = *cfg;
    for (unsigned i = 0; i < c.count; i++)
        if (!c.frequencies[i] || c.frequencies[i] > UINT32_MAX)
            return LAP_RX_ERANGE;
    c.receiver.frequency_hz = c.frequencies[0];
    return open_rx(c.radio, &c.receiver, 3, &c);
}

int lap_scan_status(int h, lap_scan_status_t *out) {
    if (!ptr(out, sizeof(*out)))
        return LAP_RX_EINVAL;
    lk();
    int e = valid(h, p4_rx.id) && p4_rx.kind == 3 ? 0 : LAP_RX_EINVAL;
    if (!e)
        *out = p4_rx.ss;
    ul();
    return e;
}
int lap_scan_control(int h, int command) {
    if (!ls_lap_api_inside() || command < 1 || command > 4)
        return LAP_RX_EINVAL;
    lk();
    int e = valid(h, p4_rx.id) && p4_rx.kind == 3 ? 0 : LAP_RX_EINVAL;
    if (!e)
        p4_rx.control = command;
    ul();
    return e;
}
int lap_scan_next(int h, lap_packet_meta_t *out) {
    if (!ptr(out, sizeof(*out)))
        return LAP_RX_EINVAL;
    lk();
    int e = !valid(h, p4_rx.id) || p4_rx.kind != 3 ? LAP_RX_EINVAL : p4_rx.hh == p4_rx.ht ? 0 : 1;
    if (e == 1)
        *out = p4_rx.hits[p4_rx.ht++ % 16];
    ul();
    return e;
}
void lap_scan_stop(int h) { lap_packet_close(h); }
static int db_request(const char *bank, uint32_t first, const lap_channel_t *rows, int count,
                      bool write) {
    int e = grant(LAP_CAP_RADIO_RX);
    if (e)
        return e;
    char b[33];
    if (!strcopy(b, bank, sizeof(b)) || !ls_lap_plain_name(b, 32) || count < 0 || count > 64 ||
        first > 16384 || (write && !ptr(rows, (size_t)count * sizeof(*rows))))
        return LAP_RX_EINVAL;
    if (write)
        for (int i = 0; i < count; i++)
            if (rows[i].size != sizeof(*rows) || rows[i].version != 1 || !rows[i].frequency_hz ||
                rows[i].frequency_hz > UINT32_MAX || rows[i].mode > 2 || rows[i].bank > 7 ||
                rows[i].tone > 511 || !memchr(rows[i].name, 0, 16) || rows[i].reserved ||
                rows[i].reserved2)
                return LAP_RX_EINVAL;
    lk();
    if (p4_db.status.state == LAP_IO_QUEUED || p4_db.status.state == LAP_IO_RUNNING) {
        ul();
        return LAP_RX_EBUSY;
    }
    p4_db.id = handle();
    p4_db.status = status_init();
    p4_db.write = write;
    p4_db.first = first;
    p4_db.count = count;
    strcpy(p4_db.bank, b);
    if (write)
        memcpy(p4_db.rows, rows, (size_t)count * sizeof(*rows));
    int h = p4_db.id;
    ul();
    return h;
}
int lap_channels_query(const char *bank, uint32_t first, int count) {
    return db_request(bank, first, NULL, count, false);
}
int lap_channels_write(const char *bank, const lap_channel_t *rows, int count) {
    return db_request(bank, 0, rows, count, true);
}
int lap_channels_read(int h, lap_channel_t *out, int max) {
    if (max < 0 || max > 64 || !ptr(out, (size_t)max * sizeof(*out)))
        return LAP_RX_EINVAL;
    lk();
    int e = !valid(h, p4_db.id) || p4_db.write     ? LAP_RX_EINVAL
            : p4_db.status.error                ? p4_db.status.error
            : p4_db.status.state != LAP_IO_DONE ? LAP_RX_EAGAIN
            : max < p4_db.count                 ? LAP_IO_ESIZE
                                             : p4_db.count;
    if (e >= 0)
        memcpy(out, p4_db.rows, (size_t)e * sizeof(*out));
    ul();
    return e;
}
int lap_io_status(int h, lap_io_status_t *out) {
    if (!ptr(out, sizeof(*out)))
        return LAP_RX_EINVAL;
    lk();
    int e = valid(h, p4_db.id) ? 0 : LAP_RX_EINVAL;
    if (!e)
        *out = p4_db.status;
    ul();
    return e;
}
static int record_request(const lap_record_config_t *cfg, bool replay) {
    if (replay && ls_lap_rx_physical_busy())
        return LAP_RX_EBUSY;
    int e = grant(LAP_CAP_RADIO_RX);
    if (e)
        return e;
    if (!(ls_lap_sys_caps() & LAP_HW_SD))
        return LAP_RX_ENODEV;
    if (!ptr(cfg, sizeof(*cfg)) || cfg->size != sizeof(*cfg) || cfg->version != 1 ||
        !memchr(cfg->name, 0, sizeof(cfg->name)) || !ls_lap_plain_name(cfg->name, 32) ||
        cfg->format < 1 || cfg->format > 4 ||
        (!replay && (cfg->max_bytes < 1024 || cfg->max_bytes > 64u * 1024u * 1024u)))
        return LAP_RX_EINVAL;
    lk();
    if ((p4_rec.id && p4_rec.status.state <= LAP_IO_RUNNING) || (replay && p4_rx.id)) {
        ul();
        return LAP_RX_EBUSY;
    }
    if (!replay && cfg->format >= 3 && !valid(cfg->source, p4_rx.id)) {
        ul();
        return LAP_RX_EINVAL;
    }
    memset(&p4_rec, 0, sizeof(p4_rec));
    p4_rec.id = handle();
    p4_rec.cfg = *cfg;
    p4_rec.packet = p4_rx.cfg;
    p4_rec.replay = replay;
    p4_rec.status = record_init();
    p4_rec.status.format = cfg->format;
    snprintf(p4_rec.root, sizeof(p4_rec.root), "%s/apps_data/%s/%s", LS_LAP_SD_ROOT, p4_app_id, cfg->name);
    int h = p4_rec.id;
    if (replay) {
        memset(&p4_rx, 0, sizeof(p4_rx));
        p4_rx.id = h;
        p4_rx.kind = 4;
        p4_rx.status = status_init();
    }
    atomic_store(&p4_tap_drops, 0);
    ul();
    return h;
}
int lap_record_start(const lap_record_config_t *cfg) { return record_request(cfg, false); }
int lap_record_status(int h, lap_record_status_t *out) {
    if (!ptr(out, sizeof(*out)))
        return LAP_RX_EINVAL;
    lk();
    int e = valid(h, p4_rec.id) ? 0 : LAP_RX_EINVAL;
    if (!e)
        *out = p4_rec.status;
    ul();
    return e;
}
int lap_record_stop(int h) {
    if (!ls_lap_api_inside())
        return LAP_RX_EINVAL;
    lk();
    int e = valid(h, p4_rec.id) && !p4_rec.replay ? 0 : LAP_RX_EINVAL;
    if (!e)
        p4_rec.stop = true;
    ul();
    return e;
}
int lap_record_mark(int h, const char *label) {
    char text[64];
    if (!strcopy(text, label, sizeof(text)))
        return LAP_RX_EINVAL;
    lk();
    int e = !valid(h,p4_rec.id)||p4_rec.replay||p4_rec.stop?LAP_RX_EINVAL:
            p4_rec.status.error?p4_rec.status.error:
            p4_rec.head-p4_rec.tail==Q?LAP_RX_EAGAIN:0;
    if (!e) {
        block_t *b = &p4_rec.q[p4_rec.head++ % Q];
        memset(b, 0, sizeof(*b));
        b->meta.timestamp_us = now_us();
        b->meta.reserved = 1;
        b->bytes = strlen(text) + 1;
        memcpy(b->data, text, b->bytes);
    }
    ul();
    return e;
}
int lap_replay_open(const char *name) {
    lap_record_config_t c = {.size = sizeof(c), .version = 1, .format = 1};
    if (!strcopy(c.name, name, sizeof(c.name)))
        return LAP_RX_EINVAL;
    return record_request(&c, true);
}
int lap_replay_status(int h, lap_record_status_t *out) { return lap_record_status(h, out); }
int lap_replay_read(int h, void *data, size_t cap, lap_packet_meta_t *meta) {
    lk();
    bool replay = valid(h, p4_rec.id) && p4_rec.replay;
    ul();
    if (!replay)
        return LAP_RX_EINVAL;
    return read_rx(h, data, cap > BLOCK ? BLOCK : cap, meta, false);
}
int lap_replay_seek(int h, uint64_t block) {
    if (!ls_lap_api_inside())
        return LAP_RX_EINVAL;
    lk();
    int e = !valid(h, p4_rec.id) || !p4_rec.replay ? LAP_RX_EINVAL : 0;
    if (!e) {
        p4_rec.seek = true;
        p4_rec.target = block;
        p4_rx.tail = p4_rx.head;
    }
    ul();
    return e;
}
void lap_replay_close(int h) {
    if (!ls_lap_api_inside())
        return;
    lk();
    if (valid(h, p4_rec.id) && p4_rec.replay) {
        p4_rec.id = 0;
        if (valid(h, p4_rx.id))
            p4_rx.id = 0;
    }
    ul();
}
/* No synchronous file access: TX replay reads one bounded recording on worker.
 */
static LS_LAP_POOL lap_hackrf_tx_config_t hackrf_config_slots[LS_LAP_CONTEXTS];
#define hackrf_config (hackrf_config_slots[ls_lap_context_index()])
int lap_tx_hackrf_config(int radio, const lap_hackrf_tx_config_t *cfg) {
    if (!LAP_HACKRF_TX) return LAP_RX_EUNSUPPORTED;
    int e=grant(LAP_CAP_RADIO_TX); if(e) return e;
    if(!ptr(cfg,sizeof(*cfg))) return LAP_RX_EINVAL;
    lap_hackrf_tx_config_t c=*cfg;
    if(c.size!=sizeof(c)||c.version!=1||c.vga_db>47||c.amp>1||c.reserved[0]||c.reserved[1]) return LAP_RX_EINVAL;
    lap_radio_info_t r; if(ls_lap_radio_snapshot(radio,&r)<0||!r.present) return LAP_RX_ENODEV;
    if(r.kind!=LAP_RADIO_KIND_HACKRF) return LAP_RX_EUNSUPPORTED;
    lk();
    if(atomic_load(&p4_physical_tx)||p4_tx.status.state==LAP_IO_QUEUED||p4_tx.status.state==LAP_IO_RUNNING) e=LAP_RX_EBUSY;
    else hackrf_config=c;
    ul(); return e;
}
static int tx_request(int radio, const lap_packet_config_t *cfg, int format, const void *data,
                      size_t bytes, uint32_t rate, const char *name) {
    int e = grant(LAP_CAP_RADIO_TX);
    if (e)
        return e;
    lap_packet_config_t c;
    if (!config(&c, cfg) || bytes > TX_BYTES || (!name && !ptr(data, bytes)))
        return LAP_RX_EINVAL;
    lap_radio_modes_t m;
    if ((e = lap_radio_modes(radio, &m)) < 0)
        return e;
    lap_radio_info_t info;
    if(ls_lap_radio_snapshot(radio,&info)<0) return LAP_RX_ENODEV;
    if (!LAP_HACKRF_TX && info.kind == LAP_RADIO_KIND_HACKRF) return LAP_RX_EUNSUPPORTED;
    if(info.kind!=LAP_RADIO_KIND_HACKRF&&c.frequency_hz>UINT32_MAX) return LAP_RX_ERANGE;
    if (c.port == LAP_PORT_RX_ONLY)
        return LAP_IO_ERXPORT;
    if (c.power_dbm < m.min_power_dbm || c.power_dbm > m.max_power_dbm)
        return LAP_IO_EPOWER;
    if (!name && !(m.modes & (format == LAP_RECORD_IQ16    ? LAP_MODE_TX_IQ
                              : format == LAP_RECORD_EDGES ? LAP_MODE_TX_EDGES
                                                           : LAP_MODE_TX_PACKET)))
        return LAP_RX_EUNSUPPORTED;
    if (!name && (!(m.modes & modbit(&c)) && format == LAP_RECORD_PACKETS))
        return LAP_RX_EUNSUPPORTED;
    char n[33] = {0};
    if (name && (!strcopy(n, name, sizeof(n)) || !ls_lap_plain_name(n, 32)))
        return LAP_RX_EINVAL;
    if (ls_lap_rx_physical_busy())
        return LAP_RX_EBUSY;
    lk();
    if (atomic_load(&p4_physical_tx) || p4_rx.id || p4_tx.status.state == LAP_IO_QUEUED ||
        p4_tx.status.state == LAP_IO_RUNNING) {
        ul();
        return LAP_RX_EBUSY;
    }
    if(!ls_lap_resource_claim(LS_LAP_LEASE_PACKET,true)) { ul(); return LAP_RX_EBUSY; }
    p4_tx.id = handle();
    p4_tx.radio = radio;
    p4_tx.cfg = c;
    p4_tx.vga=hackrf_config.vga_db; p4_tx.amp=hackrf_config.amp;
    p4_tx.format = format;
    p4_tx.bytes = bytes;
    p4_tx.rate = rate;
    p4_tx.cancel = false;
    strcpy(p4_tx.name, n);
    if (bytes)
        memcpy(p4_tx.data, data, bytes);
    p4_tx.status = status_init();
    int h = p4_tx.id;
    ul();
    return h;
}
int lap_tx_packet(int radio, const lap_packet_config_t *cfg, const void *data, size_t len) {
    if (!len || len > 255)
        return LAP_IO_ESIZE;
    return tx_request(radio, cfg, LAP_RECORD_PACKETS, data, len, 0, NULL);
}
int lap_tx_iq(int radio, const lap_packet_config_t *cfg, const int8_t *iq, int pairs,
              uint32_t rate_hz) {
    if (pairs < 1 || pairs > 32767 || !rate_hz)
        return LAP_IO_ESIZE;
    return tx_request(radio, cfg, LAP_RECORD_IQ16, iq, (size_t)pairs * 2, rate_hz, NULL);
}
int lap_tx_replay(int radio, const lap_packet_config_t *cfg, const char *name) {
    return tx_request(radio, cfg, 0, NULL, 0, 0, name);
}
int lap_tx_status(int h, lap_io_status_t *out) {
    if (!ptr(out, sizeof(*out)))
        return LAP_RX_EINVAL;
    lk();
    int e = valid(h, p4_tx.id) ? 0 : LAP_RX_EINVAL;
    if (!e)
        *out = p4_tx.status;
    ul();
    return e;
}
void lap_tx_cancel(int h) {
    if (!ls_lap_api_inside())
        return;
    lk();
    bool cancel = valid(h, p4_tx.id);
    if (cancel)
        p4_tx.cancel = true;
    ul();
    if (cancel && atomic_load(&p4_physical_tx))
        ls_lap_io_tx_interrupt();
}

#if LAP_NFC
int lap_nfc_open(void) {
    if (!ls_lap_api_inside())
        return LAP_RX_EINVAL;
    if (!(ls_lap_io_backend()->nfc_caps() & LAP_HW_NFC))
        return LAP_RX_ENODEV;
    lk();
    if (p4_nfc.id || !ls_lap_resource_claim(LS_LAP_LEASE_NFC,true)) {
        ul();
        return LAP_RX_EBUSY;
    }
    memset(&p4_nfc, 0, sizeof(p4_nfc));
    p4_nfc.id = handle();
    p4_nfc.status = status_init();
    int h = p4_nfc.id;
    ul();
    return h;
}
int lap_nfc_next(int h, lap_nfc_event_t *out, void *data, size_t cap) {
    if (!ptr(out, sizeof(*out)) || !ptr(data, cap))
        return LAP_RX_EINVAL;
    lk();
    int e = !valid(h, p4_nfc.id)      ? LAP_RX_EINVAL
            : p4_nfc.status.error     ? p4_nfc.status.error
            : p4_nfc.head == p4_nfc.tail ? 0
                                   : 1;
    if (e == 1) {
        unsigned i = p4_nfc.tail % Q;
        if (cap < p4_nfc.ev[i].ndef_bytes) {
            ul();
            return LAP_IO_ESIZE;
        }
        *out = p4_nfc.ev[i];
        memcpy(data, p4_nfc.data[i], out->ndef_bytes);
        p4_nfc.tail++;
    }
    ul();
    return e;
}
int lap_nfc_write(int h, const void *data, size_t len) {
    if (len > 2048 || !ptr(data, len))
        return LAP_RX_EINVAL;
    if (!(ls_lap_io_backend()->nfc_caps() & LAP_HW_NDEF_WRITE))
        return LAP_RX_EUNSUPPORTED;
    ls_nfc_ndef_record_t records[16];
    size_t count = 0;
    if (len && ls_ndef_parse(data, len, records, 16, &count))
        return LAP_IO_EFORMAT;
    lk();
    int e = !valid(h, p4_nfc.id) ? LAP_RX_EINVAL : p4_nfc.write ? LAP_RX_EBUSY : 0;
    if (!e) {
        memcpy(p4_nfc.pending, data, len);
        p4_nfc.len = len;
        p4_nfc.write = true;
    }
    ul();
    return e;
}
void lap_nfc_close(int h) {
    if (!ls_lap_api_inside())
        return;
    lk();
    if (valid(h, p4_nfc.id))
        p4_nfc.id = 0;
    ul();
}
#else
int lap_nfc_open(void) { return LAP_RX_EUNSUPPORTED; }
int lap_nfc_next(int h, lap_nfc_event_t *out, void *data, size_t cap) { return LAP_RX_EUNSUPPORTED; }
int lap_nfc_write(int h, const void *data, size_t len) { return LAP_RX_EUNSUPPORTED; }
void lap_nfc_close(int h) {}
#endif
void ls_lap_io_end(void) {
    lk();
    p4_tx.status.state = LAP_IO_CANCELLED;
    p4_db.status.state = LAP_IO_CANCELLED;
    p4_rec.status.state = LAP_IO_CANCELLED;
    p4_rx.id = 0;
    p4_tx.cancel = true;
    p4_tx.id = 0;
    p4_db.id = 0;
#if LAP_NFC
    p4_nfc.id = 0;
#endif
    p4_rec.id = 0;
    ul();
    if (atomic_load(&p4_physical_tx))
        ls_lap_io_tx_interrupt();
}
void ls_lap_io_begin(const lap_app_t *app) {
    ls_lap_io_end();
    lk();
    memset(&hackrf_config,0,sizeof(hackrf_config));
    snprintf(p4_app_id, sizeof(p4_app_id), "%s", app ? app->id : "");
    ul();
}
static void enqueue_record(const void *data, unsigned bytes, const lap_packet_meta_t *meta) {
    if (p4_rec.id && !p4_rec.replay && !p4_rec.stop && p4_rec.status.state <= LAP_IO_RUNNING) {
        if (p4_rec.head - p4_rec.tail == Q) {
            p4_rec.status.dropped++;
            return;
        }
        block_t *b = &p4_rec.q[p4_rec.head++ % Q];
        b->meta = *meta;
        p4_rec.status.dropped += atomic_exchange(&p4_tap_drops, 0);
        b->meta.dropped += p4_rec.status.dropped;
        if (p4_rec.status.dropped)
            b->meta.flags |= LAP_DATA_GAP;
        b->bytes = bytes;
        b->rate = p4_rec.cfg.rate_hz;
        memcpy(b->data, data, bytes);
    }
}
void ls_lap_io_tap(int format, const void *data, size_t bytes, uint64_t hz, uint32_t rate,
                   uint32_t flags) {
    if (!try_lock()) {
        atomic_fetch_add(&p4_tap_drops, 1);
        return;
    }
    if (p4_rec.id && !p4_rec.replay && (int)p4_rec.cfg.format == format) {
        lap_packet_meta_t m = {.timestamp_us = now_us(),
                               .frequency_hz = hz,
                               .flags = flags,
                               .rssi_tenths_dbm = INT16_MIN,
                               .snr_tenths_db = INT16_MIN};
        stamp(&m);
        p4_rec.cfg.rate_hz = rate;
        while (bytes) {
            unsigned n = bytes > BLOCK ? BLOCK : bytes;
            enqueue_record(data, n, &m);
            data = (const uint8_t *)data + n;
            bytes -= n;
        }
    }
    ul();
}
static void deliver(int id, const void *data, unsigned bytes, lap_packet_meta_t meta, bool record) {
    stamp(&meta);
    lk();
    if (valid(id, p4_rx.id)) {
        meta.sequence = p4_rx.seq++;
        meta.dropped += p4_rx.status.dropped;
        if (meta.dropped)
            meta.flags |= LAP_DATA_GAP;
        if (p4_rx.head - p4_rx.tail == Q)
            p4_rx.status.dropped++;
        else {
            block_t *b = &p4_rx.q[p4_rx.head++ % Q];
            b->meta = meta;
            b->bytes = bytes;
            memcpy(b->data, data, bytes);
        }
        if (record && p4_rec.cfg.source == id)
            enqueue_record(data, bytes, &meta);
    }
    ul();
}
/* LSR2 wire format, explicit little-endian offsets (host and RV32 identical).
 */
static void put(uint8_t *p, uint64_t v, unsigned n) {
    while (n--) {
        *p++ = (uint8_t)v;
        v >>= 8;
    }
}
static uint64_t get(const uint8_t *p, unsigned n) {
    uint64_t v = 0;
    for (unsigned i = 0; i < n; i++)
        v |= (uint64_t)p[i] << (8 * i);
    return v;
}
static int write_block(FILE *f, const block_t *b) {
    uint8_t h[48] = {0};
    put(h, b->bytes, 4);
    put(h + 4, b->meta.flags, 4);
    put(h + 8, b->meta.timestamp_us, 8);
    put(h + 16, b->meta.frequency_hz, 8);
    put(h + 24, (uint16_t)b->meta.rssi_tenths_dbm, 2);
    put(h + 26, (uint16_t)b->meta.snr_tenths_db, 2);
    put(h + 28, b->meta.sequence, 4);
    put(h + 32, b->meta.dropped, 4);
    put(h + 36, b->meta.reserved, 2);
    put(h + 40, b->rate, 4);
    errno=0;
    bool ok=fwrite(h,1,48,f)==48&&fwrite(b->data,1,b->bytes,f)==b->bytes;
    return ok?0:errno==ENOSPC?LAP_IO_ENOSPC:LAP_RX_EIO;
}
static int read_block(FILE *f, block_t *b) {
    uint8_t h[48];
    size_t n = fread(h, 1, 48, f);
    if (!n)
        return feof(f) ? 0 : LAP_RX_EIO;
    if (n != 48)
        return LAP_IO_EFORMAT;
    memset(b, 0, sizeof(*b));
    b->bytes = get(h, 4);
    if (b->bytes > BLOCK)
        return LAP_IO_EFORMAT;
    b->meta.flags = get(h + 4, 4) | LAP_DATA_REPLAY;
    b->meta.timestamp_us = get(h + 8, 8);
    b->meta.frequency_hz = get(h + 16, 8);
    b->meta.rssi_tenths_dbm = (int16_t)get(h + 24, 2);
    b->meta.snr_tenths_db = (int16_t)get(h + 26, 2);
    b->meta.sequence = get(h + 28, 4);
    b->meta.dropped = get(h + 32, 4);
    b->meta.reserved = get(h + 36, 2);
    stamp(&b->meta);
    b->meta.length = b->bytes;
    b->rate = get(h + 40, 4);
    return fread(b->data, 1, b->bytes, f) == b->bytes ? 1 : LAP_IO_EFORMAT;
}
static int header(FILE *f, lap_record_config_t *c, bool write, const lap_packet_config_t *p) {
    uint8_t h[128] = {0};
    if (write) {
        memcpy(h, "LSR2", 4);
        put(h + 4, 1, 2);
        put(h + 6, 128, 2);
        put(h + 8, c->format, 4);
        put(h + 12, c->rate_hz, 4);
        put(h + 16, c->frequency_hz, 8);
        if (p) {
            h[24] = p->modulation;
            h[25] = p->sync_bits;
            h[26] = p->crc;
            h[27] = p->sf;
            h[28] = p->coding_rate;
            h[29] = p->payload_bytes;
            put(h + 32, p->bitrate, 4);
            put(h + 36, p->bandwidth_hz, 4);
            put(h + 40, p->deviation_hz, 4);
            put(h + 44, p->sync_word, 4);
            put(h + 48, p->preamble, 2);
        }
        return fwrite(h, 1, 128, f) == 128 ? 0 : LAP_RX_EIO;
    }
    if (fread(h, 1, 128, f) != 128 || memcmp(h, "LSR2", 4) || get(h + 4, 2) != 1 ||
        get(h + 6, 2) != 128)
        return LAP_IO_EFORMAT;
    c->format = get(h + 8, 4);
    c->rate_hz = get(h + 12, 4);
    c->frequency_hz = get(h + 16, 8);
    return c->format >= 1 && c->format <= 4 ? 0 : LAP_IO_EFORMAT;
}
static void make_dirs(const char *path) {
    char p[128];
    snprintf(p, sizeof(p), "%s", path);
    for (char *c = p + 1; *c; c++)
        if (*c == '/') {
            *c = 0;
            MKDIR(p);
            *c = '/';
        }
}
/* Worker state never touched by app/capture tasks. */
static LS_LAP_POOL struct {
    int rx_id, tx_id, rec_id, nfc_id, rx_error;
    bool opened, rx_done;
    unsigned channel;
    uint64_t deadline, last_signal;
    bool carrier;
    unsigned tries, tx_tries;
    bool seeking;
    uint64_t seek_target;
    FILE *file;
    lap_record_config_t cfg;
    uint64_t position;
} p4_w_slots[LS_LAP_CONTEXTS];
#define p4_w (p4_w_slots[ls_lap_context_index()])

static EXT_RAM_BSS_ATTR uint8_t tx_work[TX_BYTES];
static int file_tx(const char *name, const char *id, lap_packet_config_t *cfg, int *format,
                   unsigned *bytes, unsigned *rate) {
    char path[128];
    snprintf(path, sizeof(path), "%s/apps_data/%s/%s", LS_LAP_SD_ROOT, id, name);
    FILE *f = fopen(path, "rb");
    if (!f)
        return LAP_RX_EIO;
    lap_record_config_t c = {0};
    int e = header(f, &c, false, NULL);
    *format = c.format;
    *rate = c.rate_hz;
    *bytes = 0;
    block_t b;
    unsigned consumed = 0;
    while (!e) {
        int n = read_block(f, &b);
        if (n <= 0) {
            e = n;
            break;
        }
        consumed += 48 + b.bytes;
        if (consumed > 524288) {
            e = LAP_IO_ESIZE;
            break;
        }
        if (b.meta.reserved)
            continue;
        if (c.format == LAP_RECORD_IQ16) {
            if (b.bytes % 4) {
                e = LAP_IO_EFORMAT;
                break;
            }
            if (*bytes + b.bytes / 2 > TX_BYTES) {
                e = LAP_IO_ESIZE;
                break;
            }
            if (b.rate) {
                if (*rate && *rate != b.rate) {
                    e = LAP_IO_EFORMAT;
                    break;
                }
                *rate = b.rate;
            }
            for (unsigned i = 0; i < b.bytes / 2; i++)
                tx_work[(*bytes)++] = b.data[i * 2 + 1];
        } else {
            if (*bytes + b.bytes > TX_BYTES) {
                e = LAP_IO_ESIZE;
                break;
            }
            memcpy(tx_work + *bytes, b.data, b.bytes);
            *bytes += b.bytes;
        }
        if (c.format == LAP_RECORD_PACKETS) {
            if (*bytes > 255)
                e = LAP_IO_ESIZE;
            else {
                int extra;
                do {
                    extra = read_block(f, &b);
                } while (extra > 0 && b.meta.reserved);
                if (extra != 0)
                    e = extra < 0 ? extra : LAP_IO_ESIZE;
            }
            break;
        }
    }
    fclose(f);
    if (e < 0)
        return e;
    if (!*bytes)
        return LAP_IO_EFORMAT;
    if (c.format == LAP_RECORD_AUDIO16)
        return LAP_RX_EUNSUPPORTED;
    (void)cfg;
    return 0;
}
bool ls_lap_io_service(void) {
    const ls_lap_io_backend_t *be = ls_lap_io_backend();
    int id, kind, radio;
    lap_packet_config_t cfg;
    lap_scan_config_t sc;
    unsigned control;
    lk();
    id = p4_rx.id;
    kind = p4_rx.kind;
    radio = p4_rx.radio;
    cfg = p4_rx.cfg;
    sc = p4_rx.scan;
    control = p4_rx.control;
    p4_rx.control = 0;
    ul();
    if (p4_w.rx_id != id) {
        if (p4_w.rx_id)
            be->close();
        p4_w.opened = false;
        p4_w.tries = 0;
        p4_w.rx_id = id;
        p4_w.rx_error = 0;
        p4_w.rx_done = false;
        p4_w.channel = 0;
        p4_w.carrier = false;
        p4_w.deadline = 0;
    }
    if (id && kind != 4 && !p4_w.rx_error && !p4_w.rx_done) {
        int e = 0;
        if (!p4_w.opened) {
            e = be->open(radio, &cfg, kind == 2);
            if (e == LAP_RX_EAGAIN && ++p4_w.tries >= 250) {
                be->close();
                e = LAP_RX_EBUSY;
            }
            if (e < 0 && e != LAP_RX_EAGAIN) {
                p4_w.rx_error = e;
                be->close();
            }
            if (e >= 0) {
                p4_w.opened = true;
                p4_w.deadline = now_us() + (uint64_t)sc.dwell_ms * 1000;
            }
        }
        lk();
        if (valid(id, p4_rx.id)) {
            p4_rx.status.state = e == LAP_RX_EAGAIN ? LAP_IO_QUEUED
                              : e < 0            ? LAP_IO_FAILED
                                                 : LAP_IO_RUNNING;
            p4_rx.status.error = e == LAP_RX_EAGAIN ? 0 : e;
            p4_rx.ss.state = p4_rx.status.state;
            p4_rx.ss.error = e;
        }
        ul();
        if (p4_w.opened) {
            lap_packet_meta_t m = {.frequency_hz = cfg.frequency_hz,
                                   .rssi_tenths_dbm = INT16_MIN,
                                   .snr_tenths_db = INT16_MIN};
            stamp(&m);
            lap_radio_info_t info;
            if (!ls_lap_radio_snapshot(radio, &info) && info.kind == LAP_RADIO_KIND_SIM)
                m.flags |= LAP_DATA_SYNTHETIC;
            if (be->rssi(&m.rssi_tenths_dbm) >= 0) {
                lk();
                if (valid(id, p4_rx.id))
                    p4_rx.rssi = m;
                ul();
            }
            if (kind == 3) {
                bool signal =
                    m.rssi_tenths_dbm != INT16_MIN && m.rssi_tenths_dbm >= sc.squelch_tenths_dbm;
                lk();
                if (valid(id, p4_rx.id)) {
                    if (control == LAP_SCAN_HOLD)
                        p4_rx.ss.held = 1;
                    if (control == LAP_SCAN_RESUME || control == LAP_SCAN_NEXT ||
                        control == LAP_SCAN_SKIP)
                        p4_rx.ss.held = 0;
                    if (control == LAP_SCAN_SKIP)
                        p4_rx.skip |= 1ull << p4_w.channel;
                    if (signal && !p4_w.carrier) {
                        if (p4_rx.hh - p4_rx.ht == 16)
                            p4_rx.ss.dropped++;
                        else
                            p4_rx.hits[p4_rx.hh++ % 16] = m;
                        p4_rx.ss.hits++;
                    }
                    p4_rx.ss.squelch_open = signal;
                    p4_rx.ss.channel = p4_w.channel;
                    p4_rx.ss.frequency_hz = cfg.frequency_hz;
                }
                bool held = p4_rx.ss.held;
                uint64_t skip = p4_rx.skip;
                ul();
                p4_w.carrier = signal;
                if (signal)
                    p4_w.last_signal = now_us();
                if (!held &&
                    (control == LAP_SCAN_NEXT || control == LAP_SCAN_SKIP ||
                     (now_us() >= p4_w.deadline &&
                      (!signal && now_us() - p4_w.last_signal >= (uint64_t)sc.hang_ms * 1000)))) {
                    unsigned prev = p4_w.channel;
                    do {
                        p4_w.channel = (p4_w.channel + 1) % sc.count;
                        if (!p4_w.channel) {
                            lk();
                            p4_rx.ss.sweeps++;
                            ul();
                        }
                    } while ((skip & (1ull << p4_w.channel)) && p4_w.channel != prev);
                    if (skip & (1ull << p4_w.channel)) {
                        lk();
                        p4_rx.ss.state = LAP_IO_DONE;
                        p4_rx.status.state = LAP_IO_DONE;
                        ul();
                        be->close();
                        p4_w.opened = false;
                        lk();
                        p4_w.rx_done = true;
                        ul();
                    } else {
                        cfg.frequency_hz = sc.frequencies[p4_w.channel];
                        be->close();
                        e = be->open(radio, &cfg, false);
                        p4_w.opened = e >= 0;
                        p4_w.carrier = false;
                        p4_w.deadline = now_us() + (uint64_t)sc.dwell_ms * 1000;
                        lk();
                        if (valid(id, p4_rx.id)) {
                            p4_rx.cfg = cfg;
                            if (e < 0 && e != LAP_RX_EAGAIN) {
                                p4_w.rx_error = e;
                                p4_rx.status.state = p4_rx.ss.state = LAP_IO_FAILED;
                                p4_rx.status.error = e;
                                p4_rx.ss.error = e;
                            }
                        }
                        ul();
                    }
                }
            } else {
                uint8_t data[BLOCK];
                int n = be->read(data, sizeof(data), &m);
                if (n > 0 && n <= BLOCK)
                    deliver(id, data, n, m, true);
                else if (n < 0) {
                    lk();
                    if (valid(id, p4_rx.id)) {
                        p4_rx.status.error = n;
                        p4_rx.status.state = LAP_IO_FAILED;
                    }
                    ul();
                }
            }
        }
    }
    /* DB jobs use worker-local copied rows, then publish one bounded page. */
    lk();
    int dh = p4_db.id, ds = p4_db.status.state, dc = p4_db.count;
    bool dw = p4_db.write;
    uint32_t first = p4_db.first;
    char bank[33];
    strcpy(bank, p4_db.bank);
    ul();
    if (dh && ds == LAP_IO_QUEUED) {
        lap_channel_t *rows = calloc(64, sizeof(*rows));
        int e = LAP_RX_ENOMEM;
        if (rows) {
            lk();
            if (dw)
                memcpy(rows, p4_db.rows, dc * sizeof(*rows));
            if (valid(dh, p4_db.id))
                p4_db.status.state = LAP_IO_RUNNING;
            ul();
            e = be->channels(bank, first, rows, dc, dw);
            lk();
            if (valid(dh, p4_db.id)) {
                if (!dw && e >= 0) {
                    p4_db.count = e;
                    memcpy(p4_db.rows, rows, e * sizeof(*rows));
                }
                p4_db.status.state = e < 0 ? LAP_IO_FAILED : LAP_IO_DONE;
                p4_db.status.error = e < 0 ? e : 0;
            }
            ul();
            free(rows);
        }
    }
    /* File work is bounded to one block per pass. */
    lk();
    int rh = p4_rec.id;
    bool replay = p4_rec.replay, stop = p4_rec.stop, seek = p4_rec.seek;
    uint64_t target = p4_rec.target;
    p4_rec.seek = false;
    lap_record_config_t rc = p4_rec.cfg;
    lap_packet_config_t packet_cfg = p4_rec.packet;
    char path[128];
    strcpy(path, p4_rec.root);
    ul();
    if (p4_w.rec_id != rh) {
        if (p4_w.file)
            fclose(p4_w.file);
        p4_w.file = NULL;
        p4_w.rec_id = rh;
        p4_w.position = 0;
        p4_w.seeking = false;
        if (rh) {
            int e = 0;
            p4_w.cfg = rc;
            if (!replay) {
                make_dirs(path);
                p4_w.file = create_new(path);
                if (!p4_w.file && errno == EEXIST)
                    e = LAP_RX_EBUSY;
            } else
                p4_w.file = fopen(path, "rb");
            if (!p4_w.file && !e)
                e = LAP_RX_EIO;
            if (!e)
                e = header(p4_w.file, &p4_w.cfg, !replay, &packet_cfg);
            lk();
            if (valid(rh, p4_rec.id)) {
                p4_rec.status.state = e < 0 ? LAP_IO_FAILED : LAP_IO_RUNNING;
                p4_rec.status.error = e;
                p4_rec.status.format = p4_w.cfg.format;
                p4_rec.status.bytes = replay ? 0 : 128;
            }
            ul();
            if (e < 0 && p4_w.file) {
                fclose(p4_w.file);
                p4_w.file = NULL;
            }
        }
    }
    if (rh && replay && seek && !p4_w.file) {
        p4_w.file = fopen(path, "rb");
        int e = p4_w.file ? header(p4_w.file, &p4_w.cfg, false, NULL) : LAP_RX_EIO;
        lk();
        if (valid(rh, p4_rec.id)) {
            p4_rec.status.error = e;
            p4_rec.status.state = e < 0 ? LAP_IO_FAILED : LAP_IO_RUNNING;
        }
        ul();
        if (e < 0 && p4_w.file) {
            fclose(p4_w.file);
            p4_w.file = NULL;
        }
    }
    if (p4_w.file && rh) {
        block_t b;
        int e = 0;
        if (replay) {
            if (seek) {
                if (fseek(p4_w.file, 128, SEEK_SET))
                    e = LAP_RX_EIO;
                p4_w.position = 0;
                p4_w.seeking = true;
                p4_w.seek_target = target;
            }
            if (!e && p4_w.seeking && p4_w.position < p4_w.seek_target) {
                int n = read_block(p4_w.file, &b);
                if (n <= 0)
                    e = n < 0 ? n : LAP_RX_ERANGE;
                else
                    p4_w.position++;
            }
            if (p4_w.position == p4_w.seek_target)
                p4_w.seeking = false;
            lk();
            bool room = p4_w.cfg.format <= LAP_RECORD_AUDIO16 || p4_rx.head - p4_rx.tail < Q;
            ul();
            if (!e && room && !p4_w.seeking) {
                int n = read_block(p4_w.file, &b);
                if (n <= 0) {
                    e = n;
                    if (!n) {
                        ls_lap_rx_replay_mark(0, 0, LAP_IQ_REPLAY);
                        fclose(p4_w.file);
                        p4_w.file = NULL;
                        lk();
                        if (valid(rh, p4_rec.id))
                            p4_rec.status.state = LAP_IO_DONE;
                        ul();
                    }
                } else {
                    p4_w.position++;
                    if (!b.meta.reserved) {
                        if ((p4_w.cfg.format == LAP_RECORD_PACKETS && b.bytes > 255) ||
                            (p4_w.cfg.format == LAP_RECORD_EDGES && b.bytes % 4))
                            e = LAP_IO_EFORMAT;
                        if (!e)
                            deliver(rh, b.data, b.bytes, b.meta, false);
                        if (p4_w.cfg.format == LAP_RECORD_IQ16) {
                            if (b.bytes % 4)
                                e = LAP_IO_EFORMAT;
                            else {
                                ls_lap_rx_replay_mark(
                                    b.meta.frequency_hz, (b.rate ? b.rate : p4_w.cfg.rate_hz),
                                    LAP_IQ_REPLAY |
                                        ((b.meta.flags & LAP_DATA_SYNTHETIC) ? LAP_IQ_SYNTHETIC
                                                                             : 0) |
                                        ((b.meta.flags & LAP_DATA_GAP) ? LAP_IQ_GAP : 0));
                                ls_lap_rx_replay_push((int16_t *)b.data, b.bytes / 4);
                            }
                        } else if (p4_w.cfg.format == LAP_RECORD_AUDIO16) {
                            if (b.bytes % 2)
                                e = LAP_IO_EFORMAT;
                            else
                                ls_lap_audio_tap((int16_t *)b.data, b.bytes / 2);
                        }
                    }
                    lk();
                    if (valid(rh, p4_rec.id)) {
                        p4_rec.status.position = p4_w.position;
                        p4_rec.status.bytes += b.bytes;
                        p4_rec.status.flags |= LAP_DATA_REPLAY;
                        p4_rec.status.blocks++;
                    }
                    ul();
                }
            }
        } else {
            lk();
            bool have = p4_rec.head != p4_rec.tail;
            if (have)
                b = p4_rec.q[p4_rec.tail++ % Q];
            uint64_t used = p4_rec.status.bytes;
            ul();
            if (have) {
                if (atomic_load(&p4_file_fault))
                    e = atomic_load(&p4_file_fault);
                else if (used + 48 + b.bytes > rc.max_bytes)
                    e = LAP_IO_ENOSPC;
                else
                    e = write_block(p4_w.file, &b);
                lk();
                if (valid(rh, p4_rec.id)) {
                    p4_rec.status.bytes += e ? 0 : 48 + b.bytes;
                    p4_rec.status.blocks += !e;
                    p4_rec.status.dropped += atomic_exchange(&p4_tap_drops, 0);
                }
                ul();
            } else if (stop) {
                e = fflush(p4_w.file) ? LAP_RX_EIO : 0;
                if (fclose(p4_w.file) && !e)
                    e = LAP_RX_EIO;
                p4_w.file = NULL;
                lk();
                if (valid(rh, p4_rec.id))
                    p4_rec.status.state = e < 0 ? LAP_IO_FAILED : LAP_IO_DONE;
                ul();
            }
        }
        if (e < 0) {
            if (p4_w.file)
                fclose(p4_w.file);
            p4_w.file = NULL;
            lk();
            if (valid(rh, p4_rec.id)) {
                p4_rec.status.error = e;
                p4_rec.status.state = LAP_IO_FAILED;
            }
            ul();
        }
    }
    /* TX copies are immutable once queued. The backend retains only tx_work. */
    lk();
    int th = p4_tx.id, ts = p4_tx.status.state;
    bool cancel = p4_tx.cancel;
    int tr = p4_tx.radio, tf = p4_tx.format;
    unsigned tn = p4_tx.bytes, rate = p4_tx.rate, vga=p4_tx.vga; bool amp=p4_tx.amp;
    lap_packet_config_t tc = p4_tx.cfg;
    char name[33], owner[32];
    strcpy(name, p4_tx.name);
    strcpy(owner, p4_app_id);
    if (th && ts == LAP_IO_QUEUED && !cancel) {
        memcpy(tx_work, p4_tx.data, tn);
        p4_tx.status.state = LAP_IO_RUNNING;
    }
    ul();
    if (p4_w.tx_id && (!th || th != p4_w.tx_id || cancel)) {
        be->tx_cancel();
        lk();
        if (valid(th, p4_tx.id)) {
            p4_tx.status.state = LAP_IO_CANCELLED;
            p4_tx.status.error = LAP_IO_ECANCELLED;
        }
        ul();
    }
    if (th && ts == LAP_IO_QUEUED) {
        int e = cancel ? LAP_IO_ECANCELLED : 0;
        if (!e && name[0])
            e = file_tx(name, owner, &tc, &tf, &tn, &rate);
        lk();
        bool current = valid(th, p4_tx.id) && !p4_tx.cancel;
        ul();
        if (!current)
            e = LAP_IO_ECANCELLED;
        if (!e && p4_w.tx_tries >= 250) {
            be->tx_cancel();
            e = LAP_RX_EBUSY;
        }
        if (!e) {
            atomic_store(&p4_physical_tx, true);
            e = be->tx(tr, &tc, tf, tx_work, tn, rate, vga, amp);
            if (e < 0)
                atomic_store(&p4_physical_tx, false);
            if (e == LAP_RX_EAGAIN)
                p4_w.tx_tries++;
            else
                p4_w.tx_tries = 0;
        }
        if (!e)
            p4_w.tx_id = th;
        lk();
        if (valid(th, p4_tx.id)) {
            if (p4_tx.cancel)
                e = LAP_IO_ECANCELLED;
            p4_tx.status.error = e == LAP_RX_EAGAIN ? 0 : e;
            p4_tx.status.state = e == LAP_RX_EAGAIN       ? LAP_IO_QUEUED
                              : e == LAP_IO_ECANCELLED ? LAP_IO_CANCELLED
                              : e < 0                  ? LAP_IO_FAILED
                                                       : LAP_IO_RUNNING;
        }
        ul();
    }
    if (p4_w.tx_id) {
        int e = be->tx_poll();
        if (e) {
            lk();
            if (valid(p4_w.tx_id, p4_tx.id) && !p4_tx.cancel) {
                p4_tx.status.state = e < 0 ? LAP_IO_FAILED : LAP_IO_DONE;
                p4_tx.status.error = e < 0 ? e : 0;
            }
            ul();
            p4_w.tx_id = 0;
            atomic_store(&p4_physical_tx, false);
        }
    }
    int nh = 0;
#if LAP_NFC
    /* NFC backend events are copied; no app callback reaches the RF task. */
    lk();
    nh = p4_nfc.id;
    bool nw = p4_nfc.write;
    unsigned nl = p4_nfc.len;
    ul();
    if (p4_w.nfc_id != nh) {
        if (p4_w.nfc_id)
            be->nfc_close();
        p4_w.nfc_id = nh;
        if (nh) {
            int e = be->nfc_open();
            lk();
            if (valid(nh, p4_nfc.id)) {
                p4_nfc.status.state = e < 0 ? LAP_IO_FAILED : LAP_IO_RUNNING;
                p4_nfc.status.error = e;
            }
            ul();
        }
    }
    if (nh) {
        uint8_t data[2048];
        if (nw) {
            lk();
            memcpy(data, p4_nfc.pending, nl);
            p4_nfc.write = false;
            ul();
            int e = be->nfc_write(data, nl);
            if (e < 0) {
                lk();
                p4_nfc.status.error = e;
                ul();
            }
        }
        lap_nfc_event_t ev = {0};
        int e = be->nfc_read(&ev, data, sizeof(data));
        if (e < 0) {
            lk();
            if (valid(nh, p4_nfc.id)) {
                p4_nfc.status.error = e;
                p4_nfc.status.state = LAP_IO_FAILED;
            }
            ul();
        }
        if (e > 0 && ev.ndef_bytes <= 2048) {
            ev.size = sizeof(ev);
            ev.version = 1;
            ev.timestamp_us = now_us();
            lk();
            if (valid(nh, p4_nfc.id)) {
                if (p4_nfc.head - p4_nfc.tail == Q)
                    p4_nfc.status.dropped++;
                else {
                    unsigned i = p4_nfc.head++ % Q;
                    ev.dropped = p4_nfc.status.dropped;
                    p4_nfc.ev[i] = ev;
                    memcpy(p4_nfc.data[i], data, ev.ndef_bytes);
                }
            }
            ul();
        }
    }
    if(!nh && !p4_w.nfc_id) ls_lap_resource_claim(LS_LAP_LEASE_NFC,false);
#endif
    if(!p4_rx.id && !p4_w.opened && !p4_w.tx_id && !atomic_load(&p4_physical_tx) &&
        p4_tx.status.state!=LAP_IO_QUEUED && p4_tx.status.state!=LAP_IO_RUNNING)
        ls_lap_resource_claim(LS_LAP_LEASE_PACKET,false);
    return id || rh || th || nh || (dh && ds == LAP_IO_QUEUED) || p4_w.file || p4_w.opened;
}

#include "ls_lap_usage.h"
ls_lap_usage_t ls_lap_io_usage(unsigned s) {
    ls_lap_usage_t v={sizeof(p4_rx_slots[s])+sizeof(p4_tx_slots[s])+sizeof(p4_db_slots[s])+sizeof(p4_rec_slots[s]),
        (p4_rx_slots[s].id>0)+(p4_tx_slots[s].id>0)+(p4_db_slots[s].id>0)+(p4_rec_slots[s].id>0)};
#if LAP_NFC
    v.bytes += sizeof(p4_nfc_slots[s]); v.handles += p4_nfc_slots[s].id>0;
#endif
    return v;
}

#include "ls_field.h"
#include "ls_trail.h"
#include "ls_compass.h"
#include "ls_notes.h"
#include "ls_df_sources.h"
#include "ls_keypad.h"
#include "apps/fm/pocsag.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "ls_gps.h"
#include "ls_mesh.h"
#include "ls_wireless.h"
#include "radio/radio_health.h"
#include "radio/radio_endpoint.h"
#include "core/perf.h"
#include "core/ls_time.h"

#define LOG_LIMIT (16u * 1024u * 1024u)
#define QUEUE_CAP 4
typedef enum { CMD_CONFIG, CMD_MODE, CMD_TX, CMD_NOTE, CMD_CAL, CMD_CLEAR,
               CMD_FSK } command_kind;
typedef struct {
    command_kind kind;
    ls_lora_cfg_t cfg;
    ls_fsk_cfg_t fsk;
    ls_lab_mode_t mode;
    ls_journal_entry_t entry;
    int value;
} command;

static SemaphoreHandle_t s_lock;
static StaticSemaphore_t s_lock_memory;
#ifndef LS_FIELD_TEST
static TaskHandle_t s_task, s_io_task;
#define FIELD_STACK_WORDS (8192 / sizeof(StackType_t))
static EXT_RAM_BSS_ATTR StackType_t s_field_stack[FIELD_STACK_WORDS];
static EXT_RAM_BSS_ATTR StackType_t s_io_stack[FIELD_STACK_WORDS];
static DRAM_ATTR StaticTask_t s_field_tcb, s_io_tcb;
#endif
static bool s_started, s_stop, s_want, s_record, s_loaded, s_have_saved, s_saved_rx;
static bool s_watch;
static bool s_wireless_watch;
static uint32_t record_rows,record_errors,record_packets;
static int64_t record_saved_us;
EXT_RAM_BSS_ATTR static ls_wireless_snapshot_t s_wireless;
static ls_field_source_t s_source = LS_FIELD_MESH;
static ls_field_provider_t s_providers[LS_FIELD_SOURCES];
static ls_lora_cfg_t s_saved;
EXT_RAM_BSS_ATTR static ls_field_state_t s_live, s_public;
EXT_RAM_BSS_ATTR static command s_queue[QUEUE_CAP], s_command;
EXT_RAM_BSS_ATTR static ls_journal_entry_t s_entries[LS_JOURNAL_CAP], s_disk;
static unsigned s_qhead, s_qcount, s_count;
static unsigned s_note_head, s_note_count;
EXT_RAM_BSS_ATTR static ls_journal_entry_t s_notes[QUEUE_CAP];
static char s_storage[80];
static void io_step(void);
static void pocsag_stop(void);
static uint32_t s_next_id = 1;
static int64_t s_next_sample, s_next_record, s_hold_since, s_tx_deadline;
static ls_imu_sample_t s_last_imu;
static int64_t s_last_imu_us;
static uint32_t s_mesh_seen;
static bool s_mesh_baseline;
static int64_t s_next_peers;
EXT_RAM_BSS_ATTR static ls_compass_fit_t s_fit;
static ls_compass_guide_t s_guide;
static EXT_RAM_BSS_ATTR float s_scan_work[LS_FIELD_BINS];
typedef struct {
    ls_compass_cal_t cal;
    bool valid, saved;
    uint32_t version, written;
} compass_profile;
static compass_profile s_profiles[2]; /* standalone, keyboard attached */
static int s_profile; /* field worker owns selection; I/O addresses both by index */
typedef struct { uint32_t magic, generation; ls_compass_cal_t cal; uint32_t sum; } compass_record;
static long s_valid_bytes;
static bool s_damaged, s_incompatible;
static const char *s_directory = "/sdcard/journal";

static void lock(void) { xSemaphoreTake(s_lock, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(s_lock); }
static void message(const char *text) { snprintf(s_live.status, sizeof(s_live.status), "%s", text); }
static void storage(const char *text) { lock(); snprintf(s_storage, sizeof(s_storage), "%s", text); unlock(); }
static void publish(void) { lock(); s_live.record_rows=record_rows;s_live.record_errors=record_errors;s_live.record_packets=record_packets;s_live.record_saved_us=record_saved_us;s_live.journal_count = (int)s_count; memcpy(s_live.storage, s_storage, sizeof(s_storage)); s_public = s_live; unlock(); }

static void detected(int64_t now, uint32_t frequency, float rssi, float snr, bool mesh)
{
    memmove(s_live.detections+1,s_live.detections,
            sizeof(s_live.detections[0])*(LS_FIELD_DETECTIONS-1));
    float heading = s_live.sample.imu_valid && now>=s_live.sample.time_us &&
                    now-s_live.sample.time_us<350000 ? s_live.sample.heading : NAN;
    s_live.detections[0]=(ls_field_detection_t){now,frequency,rssi,snr,heading,mesh};
    if(s_live.detection_count<LS_FIELD_DETECTIONS)s_live.detection_count++;
    if(isfinite(heading) && isfinite(rssi)) {
        int bin=(int)(fmodf(heading+360,360)/10);
        s_live.bearing[bin]=rssi;
        if(s_live.bearing_count[bin]<UINT16_MAX)s_live.bearing_count[bin]++;
    }
}

const char *ls_field_source_name(ls_field_source_t source)
{
    static const char *const names[] = {"NOTES ONLY", "MESH", "LORA LABS", "RTL", "CC1101", "NRF24", "NFC", "WI-FI", "BLUETOOTH", "HACKRF"};
    return source >= 0 && source < LS_FIELD_SOURCES ? names[source] : "UNKNOWN";
}

static uint32_t checksum(const void *data, size_t size)
{
    const uint8_t *p = data;
    uint32_t hash = 2166136261u;
    while (size--) hash = (hash ^ *p++) * 16777619u;
    return hash;
}

static bool path(char *out, size_t cap, const char *name)
{
    const int n = snprintf(out, cap, "%s/%s", s_directory, name);
    return n > 0 && (size_t)n < cap;
}

static void remember(const ls_journal_entry_t *entry)
{
    lock();
    unsigned index;
    for (index = 0; index < s_count; index++) if (s_entries[index].id == entry->id) break;
    if (index == s_count) {
        if (s_count == LS_JOURNAL_CAP) {
            memmove(s_entries, s_entries + 1, sizeof(s_entries[0]) * (LS_JOURNAL_CAP - 1));
            index--;
        } else s_count++;
    }
    s_entries[index] = *entry;
    if (entry->id >= s_next_id) s_next_id = entry->id + 1;
    unlock();
}

static void load_notes(void)
{
    char name[160];
    if (!path(name, sizeof(name), "entries.bin")) return;
    FILE *f = fopen(name, "rb");
    if (!f) { storage("No saved notes; SD saves begin with your first note"); return; }
    uint32_t header[3];
    size_t count;
    while ((count = fread(header, 1, sizeof(header), f)) > 0) {
        if (count == sizeof(header) && !s_valid_bytes &&
            (header[0] != 0x4a4c5301u || header[1] != sizeof(s_disk))) {
            storage("Unrecognised journal format; archive kept read-only");
            s_incompatible = true;
            fclose(f); return;
        }
        if (count != sizeof(header) || header[0] != 0x4a4c5301u || header[1] != sizeof(s_disk) ||
            fread(&s_disk, sizeof(s_disk), 1, f) != 1 ||
            checksum(&s_disk, sizeof(s_disk)) != header[2]) {
            storage("Journal tail damaged; earlier notes recovered");
            s_damaged = true;
            fclose(f); return;
        }
        s_disk.title[sizeof(s_disk.title) - 1] = 0;
        s_disk.text[sizeof(s_disk.text) - 1] = 0;
        s_disk.saved = true;
        remember(&s_disk);
        s_valid_bytes = ftell(f);
    }
    fclose(f);
    storage("Saved notes loaded");
}

static bool save_note(ls_journal_entry_t *entry)
{
    char name[160];
    if (!path(name, sizeof(name), "entries.bin")) return false;
    struct stat st;
    bool exists = stat(name, &st) == 0;
    if (s_incompatible || (!s_damaged && exists && st.st_size != s_valid_bytes)) {
        storage("Different SD archive; note kept in RAM"); return false;
    }
    if (exists && (uint64_t)st.st_size + sizeof(*entry) + 12 > LOG_LIMIT) {
        storage("Journal full; export before starting a new archive"); return false;
    }
#ifdef _WIN32
    mkdir(s_directory);
#else
    mkdir(s_directory, 0775);
#endif
    FILE *f = fopen(name, s_damaged ? "r+b" : "ab");
    if (!f) { storage("SD unavailable; note kept in RAM"); return false; }
    if (s_damaged) {
        /* Only the incomplete suffix after verified records is discarded. */
        if (ftruncate(fileno(f), s_valid_bytes) || fseek(f, 0, SEEK_END)) {
            fclose(f); storage("Cannot repair journal tail; note kept in RAM"); return false;
        }
        s_damaged = false;
    }
    const uint32_t header[] = {0x4a4c5301u, sizeof(*entry), checksum(entry, sizeof(*entry))};
    bool ok = fwrite(header, sizeof(header), 1, f) == 1 && fwrite(entry, sizeof(*entry), 1, f) == 1;
    long end = ftell(f);
    if (fclose(f) != 0) ok = false;
    if (!ok) { s_damaged = true; storage("SD write failed; note kept in RAM"); return false; }
    s_valid_bytes = end;
    entry->saved = true;
    /* Human-readable exports are independent of the recovery log. */
    if (path(name, sizeof(name), "notes.md") && (f = fopen(name, "ab")) != NULL) {
        fprintf(f, "\n## %s\n\nEntry %lu | uptime %.3f s | %s\n\n%s\n\n",
                entry->title, (unsigned long)entry->id, entry->sample.time_us / 1e6,
                ls_field_source_name(entry->sample.source), entry->text);
        if (entry->sample.gps_valid)
            fprintf(f, "GPS: %.7f, %.7f; %.1f m\n\n", entry->sample.lat, entry->sample.lon, entry->sample.alt_m);
        if (entry->sample.signal_valid) {
            fprintf(f, "Radio: %lu Hz, %.1f dBm", (unsigned long)entry->sample.frequency, entry->sample.rssi);
            if (isfinite(entry->sample.snr)) fprintf(f, ", %.1f dB SNR", entry->sample.snr);
            fputs("\n\n", f);
        }
        if (fclose(f) != 0) { storage("Note saved; Markdown export failed"); return true; }
        storage("Saved to SD /journal; Markdown export ready");
    } else storage("Note saved; Markdown export unavailable");
    return true;
}

static bool save_sample(const ls_field_sample_t *p)
{
    char name[160];
    if (!path(name, sizeof(name), "samples.csv")) return false;
    struct stat st;
    bool empty = stat(name, &st) != 0;
    if (!empty && (uint64_t)st.st_size > LOG_LIMIT - 1024) { storage("Sample log full; recording stopped"); return false; }
#ifdef _WIN32
    mkdir(s_directory);
#else
    mkdir(s_directory, 0775);
#endif
    FILE *f = fopen(name, "ab");
    if (!f) { storage("SD unavailable; recording stopped"); return false; }
    if (empty) fputs("uptime_us,utc,source,gps_valid,lat,lon,alt_m,imu_valid,mag_valid,heading,ax,ay,az,gx,gy,gz,mx,my,mz,radio_valid,signal_valid,hz,rssi,snr,packets\n", f);
    int n = fprintf(f, "%lld,%s,%s,%d,%.7f,%.7f,%.2f,%d,%d,%.2f,%.4f,%.4f,%.4f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%d,%d,%lu,%.1f,%.1f,%lu\n",
        (long long)p->time_us, p->utc, ls_field_source_name(p->source), p->gps_valid, p->lat, p->lon, p->alt_m,
        p->imu_valid, p->imu.mag_valid, p->heading, p->imu.ax, p->imu.ay, p->imu.az, p->imu.gx, p->imu.gy, p->imu.gz,
        p->imu.mx, p->imu.my, p->imu.mz, p->radio_valid, p->signal_valid, (unsigned long)p->frequency, p->rssi, p->snr, (unsigned long)p->packets);
    bool ok = fclose(f) == 0 && n > 0;
    storage(ok ? "Recording GPS, motion and radio to SD" : "SD write failed; recording stopped");
    return ok;
}

/* Payloads, one row per packet. Separate from samples.csv because a packet
   arrives when it arrives and the sample log is a one-second heartbeat -
   folding them together would either lose packets or pad the file with
   repeats. Hex rather than raw bytes so the result is still a text CSV that
   opens anywhere, and the position fields come from the last sample so a
   capture can be placed on a map afterwards. */
static bool save_packet(const ls_field_sample_t *p, const uint8_t *data, int len)
{
    char name[160];
    if (len <= 0) return true;
    if (!path(name, sizeof(name), "packets.csv")) return false;
    struct stat st;
    bool empty = stat(name, &st) != 0;
    if (!empty && (uint64_t)st.st_size > LOG_LIMIT - 1024) {
        storage("Packet log full; recording stopped");
        return false;
    }
#ifdef _WIN32
    mkdir(s_directory);
#else
    mkdir(s_directory, 0775);
#endif
    FILE *f = fopen(name, "ab");
    if (!f) { storage("SD unavailable; recording stopped"); return false; }
    if (empty) fputs("uptime_us,utc,source,hz,rssi,snr,gps_valid,lat,lon,len,hex\n", f);
    int n = fprintf(f, "%lld,%s,%s,%lu,%.1f,%.1f,%d,%.7f,%.7f,%d,",
        (long long)p->time_us, p->utc, ls_field_source_name(p->source),
        (unsigned long)p->frequency, p->rssi, p->snr,
        p->gps_valid, p->lat, p->lon, len);
    for (int i = 0; i < len && n > 0; i++) n = fprintf(f, "%02X", data[i]);
    if (n > 0) n = fprintf(f, "\n");
    bool ok = fclose(f) == 0 && n > 0;
    if (!ok) storage("SD write failed; recording stopped");
    return ok;
}

static bool restore(void)
{
    if (s_live.transmitting && !ls_lora_send_done() && esp_timer_get_time() <= s_tx_deadline) return false;
    s_live.transmitting = false;
    esp_err_t err = ESP_OK;
    if (ls_lora_scanning()) err = ls_lora_scan_end();
    /* The FSK session put the part in a different modulation and saved the
       LoRa settings itself; ending it first means the restore below writes
       over a part that is back in a known state. */
    if (err == ESP_OK && ls_lora_fsk_active()) err = ls_lora_fsk_end();
    if (err == ESP_OK && s_have_saved) err = ls_lora_configure(&s_saved);
    if (err == ESP_OK && s_have_saved && s_saved_rx) err = ls_lora_receive();
    if (err != ESP_OK) { message("Restore failed; radio held. Toggle DIRECT off to retry"); return false; }
    pocsag_stop();
    ls_mesh_radio_hold(false);
    s_live.direct = s_live.busy = false;
    s_have_saved = false;
    message("Mesh control restored");
    return true;
}

/* POCSAG, with the numbers the console receiver already works with - see the
   'lora pocsag' command. Pinned rather than offered: a paging receiver with
   the wrong deviation or sync word looks exactly like a quiet channel, so
   there is nothing useful for a user to choose here.
   0x7cd215d8 is the POCSAG frame synchronisation word. Systems that transmit
   inverted need its complement, which is what the IQ switch selects - that
   control does nothing else in an FSK mode. */
#define POCSAG_SYNC      0x7cd215d8u
#define POCSAG_DEVIATION 4500u
/* Carson for 1200 baud at +-4.5 kHz is 2*(4500+1200) = 11.4 kHz, and 11700 is
   the narrowest the part offers above it. The console used 19500, which is
   nearly double the noise for no extra signal - worth about 2 dB of
   sensitivity, and at VHF on a board matched for 915 MHz every dB counts. */
#define POCSAG_BANDWIDTH 11700u
/* 64 payload bytes plus the 32 sync bits the hardware strips: 544 bits, which
   is the batch period the decoder's contiguity check is written against. */
#define POCSAG_BYTES     64u
#define POCSAG_BITS      544u

/* The paging decoder, alive only while POCSAG mode is. fm_state_t carries a
   sixteen-entry page ring and is far too big for internal RAM, which is why
   the console puts it in PSRAM too. */
static fm_state_t   *s_pocsag_state;
static pocsag_ctx_t *s_pocsag;
static int64_t       s_pocsag_last;

static void pocsag_stop(void)
{
    if (s_pocsag) { pocsag_destroy(s_pocsag); s_pocsag = NULL; }
    if (s_pocsag_state) { heap_caps_free(s_pocsag_state); s_pocsag_state = NULL; }
    s_pocsag_last = 0;
}

static bool pocsag_ready(int baud)
{
    if (s_pocsag) { pocsag_set_baud(s_pocsag, baud); return true; }
    s_pocsag_state = heap_caps_calloc(1, sizeof(*s_pocsag_state),
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_pocsag_state) { message("No PSRAM for the paging decoder"); return false; }
    s_pocsag = pocsag_create(s_pocsag_state, baud);
    if (!s_pocsag) { pocsag_stop(); message("Paging decoder unavailable"); return false; }
    s_pocsag_last = 0;
    /* The counts belong to this session, not the last one. */
    s_live.pages = 0; s_live.page_log_count = 0;
    return true;
}

/* One batch. `contiguous` tells the decoder whether this batch butts onto the
   previous one, which is how it keeps frame alignment across a transmission;
   the period is the 544 bits a batch takes at the current baud. */
static void pocsag_feed(const uint8_t *data, int len, int64_t now)
{
    if (!s_pocsag) return;
    const uint32_t baud = s_live.fsk.bitrate ? s_live.fsk.bitrate : 1200u;
    const int64_t period = (int64_t)POCSAG_BITS * 1000000 / baud;
    const bool contiguous = s_pocsag_last &&
        llabs(now - s_pocsag_last - period) <= period / 10 + 20000;
    s_pocsag_last = now;
    const uint32_t before = pocsag_n_pages(s_pocsag);
    pocsag_process_batch(s_pocsag, data, len, s_live.config.invert_iq, contiguous);
    const uint32_t after = pocsag_n_pages(s_pocsag);
    s_live.pages = after;
    if (after == before || s_pocsag_state->page_count <= 0) return;

    /* Copy the ring out newest-first, so a screen can page through recent
       messages the way the FM screen does without reaching into the FM app's
       state or taking this worker's lock. */
    int held = s_pocsag_state->page_count;
    if (held > FM_PAGE_LOG_MAX) held = FM_PAGE_LOG_MAX;
    if (held > LS_FIELD_PAGES) held = LS_FIELD_PAGES;
    for (int i = 0; i < held; i++) {
        const int idx = (s_pocsag_state->page_head - 1 - i +
                         2 * FM_PAGE_LOG_MAX) % FM_PAGE_LOG_MAX;
        const fm_page_t *p = &s_pocsag_state->pages[idx];
        ls_field_page_t *out = &s_live.page_log[i];
        out->ts_us = p->ts_us;
        out->address = p->address;
        out->baud = p->baud;
        out->function = p->function;
        snprintf(out->text, sizeof(out->text), "%s", p->text);
    }
    s_live.page_log_count = (uint8_t)held;
}
_Static_assert(sizeof(((ls_field_page_t *)0)->text) == FM_PAGE_TEXT_MAX,
               "ls_field_page_t.text must match FM_PAGE_TEXT_MAX");

static void fsk_params(ls_fsk_cfg_t *out)
{
    *out = s_live.fsk;
    out->freq_hz = s_live.config.freq_hz;   /* BAND moves every mode */
    if (s_live.mode == LS_LAB_POCSAG) {
        if (out->bitrate != 1200u && out->bitrate != 2400u) out->bitrate = 1200u;
        out->deviation_hz = POCSAG_DEVIATION;
        out->bandwidth_hz = POCSAG_BANDWIDTH;
        out->sync_word = s_live.config.invert_iq ? ~POCSAG_SYNC : POCSAG_SYNC;
        out->payload_bytes = POCSAG_BYTES;
    }
}

static bool configure(void)
{
    if (ls_lora_scanning() && ls_lora_scan_end() != ESP_OK) return false;
    if (ls_lora_fsk_active() && ls_lora_fsk_end() != ESP_OK) return false;
    s_live.spectrum_us=0;
    if (LS_LAB_IS_FSK(s_live.mode)) {
        /* The FSK session configures the part itself, so there is no
           ls_lora_configure first - that would set LoRa modulation and then
           immediately be replaced. */
        ls_fsk_cfg_t fsk;
        fsk_params(&fsk);
        if (s_live.mode == LS_LAB_POCSAG) {
            if (!pocsag_ready((int)fsk.bitrate)) return false;
        } else pocsag_stop();
        return ls_lora_fsk_begin(&fsk) == ESP_OK;
    }
    pocsag_stop();
    if (ls_lora_configure(&s_live.config) != ESP_OK) return false;
    if (s_live.mode == LS_LAB_SPECTRUM) {
        uint32_t hz = s_live.config.freq_hz;
        return ls_lora_scan_begin(hz - 1000000u, hz + 1000000u) == ESP_OK;
    }
    return ls_lora_receive() == ESP_OK;
}

static void finish_calibration(void)
{
    ls_compass_cal_t cal;
    bool fitted = s_guide.step == LS_COMPASS_STEPS && ls_compass_finish(&s_fit, &cal);
    /* A fit is judged by how far the corrected field strength strays from
       one value. A good one on this board is about 4 %; one at 11 % was
       accepted, replaced a 4 % fit only because it was newer, and left the
       interference check almost no margin. Refuse it, and never trade a
       saved fit for a clearly worse one. */
    lock(); const compass_profile prev = s_profiles[s_profile]; unlock();
    if (fitted && (cal.error > LS_FIELD_CAL_ERROR_MAX ||
                   (prev.valid && cal.error > prev.cal.error + .02f))) {
        s_live.calibration_failed = true;
        if (prev.valid && cal.error > prev.cal.error + .02f)
            snprintf(s_live.compass_status, sizeof(s_live.compass_status),
                     "Fit %.0f%%, worse than the saved %.0f%%; kept the saved one",
                     cal.error * 100, prev.cal.error * 100);
        else
            snprintf(s_live.compass_status, sizeof(s_live.compass_status),
                     "Fit %.0f%%: the field moved while turning. Retry off chargers and metal",
                     cal.error * 100);
        return;
    }
    if (fitted) {
        lock(); s_profiles[s_profile].cal = cal; s_profiles[s_profile].valid = true;
        s_profiles[s_profile].saved = false; s_profiles[s_profile].version++; unlock();
        s_live.calibrating = false;
        s_live.calibration_failed = false;
        memset(s_live.bearing_count, 0, sizeof(s_live.bearing_count));
        snprintf(s_live.compass_status, sizeof(s_live.compass_status), "Calibration complete; hold flat to use compass");
    } else if (s_guide.step == LS_COMPASS_STEPS) {
        s_live.calibration_failed = true;
        snprintf(s_live.compass_status, sizeof(s_live.compass_status), "Readings did not agree; move away from metal and retry");
    } else snprintf(s_live.compass_status, sizeof(s_live.compass_status), "Follow the picture; each position advances itself");
}

static void sample(int64_t now)
{
    ls_field_sample_t *p = &s_live.sample;
    memset(p, 0, sizeof(*p));
    p->time_us = now;
    p->snr = NAN;
    p->heading = NAN;
    lock(); p->source = s_source; ls_field_provider_t provider = s_providers[p->source]; unlock();
    ls_gps_state_t gps;
    ls_gps_get(&gps);
    p->gps_valid = gps.fix && gps.alive && gps.last_sentence_us > 0 && now >= gps.last_sentence_us &&
                   now - gps.last_sentence_us < 3000000 && isfinite(gps.lat_deg) && isfinite(gps.lon_deg) &&
                   fabs(gps.lat_deg) <= 90 && fabs(gps.lon_deg) <= 180;
    if (p->gps_valid) {
        p->lat = gps.lat_deg; p->lon = gps.lon_deg; p->alt_m = gps.alt_m;
        if (gps.year >= 2024 && gps.year <= 2099 && gps.month >= 1 && gps.month <= 12 && gps.day >= 1 && gps.hour <= 23 && gps.minute <= 59 && gps.second <= 59) {
            static const uint8_t days[]={31,28,31,30,31,30,31,31,30,31,30,31};
            const bool leap=gps.year%4==0 && (gps.year%100!=0 || gps.year%400==0);
            if(gps.day<=days[gps.month-1]+(gps.month==2 && leap))
                snprintf(p->utc, sizeof(p->utc), "%04u-%02u-%02uT%02u:%02u:%02uZ", gps.year,
                         gps.month, gps.day, gps.hour, gps.minute, gps.second);
        }
    }
    if(!p->utc[0] && ls_time_is_synced()) ls_time_render_stamp(p->utc,sizeof(p->utc));
    if (ls_imu_read(&p->imu)) {
        s_last_imu = p->imu;
        s_last_imu_us = now;
        p->imu_valid = true;
    } else if (s_last_imu_us > 0 && now >= s_last_imu_us && now - s_last_imu_us < 350000) {
        /* The touch/rotation reader can briefly own the sensor bus. */
        p->imu = s_last_imu;
        p->imu_valid = true;
    }
    if (s_live.calibrating && !s_live.calibration_failed) {
        const ls_imu_sample_t *fresh = p->imu_valid && s_last_imu_us == now ? &p->imu : NULL;
        ls_compass_collect(&s_fit, fresh);
        ls_compass_guide_sample(&s_guide, fresh);
        if (s_guide.step == LS_COMPASS_FACE_STEPS && !s_fit.tumble) {
            /* The faces pin the offset; the figure eight that follows fills
               in the directions soft iron needs. Faces that do not fit fail
               here rather than after another half minute of waving. */
            if (ls_compass_tumble_begin(&s_fit))
                snprintf(s_live.compass_status, sizeof(s_live.compass_status), "Wave it in a figure eight, turning it over as you go");
            else { s_guide.step = LS_COMPASS_STEPS; finish_calibration(); }
        } else if (ls_compass_tumble_done(&s_fit)) {
            s_guide.step = LS_COMPASS_STEPS; finish_calibration();
        }
    }
    lock(); ls_compass_cal_t calibration = s_profiles[s_profile].cal;
    s_live.calibrated = s_profiles[s_profile].valid;
    s_live.calibration_saved = s_profiles[s_profile].saved; unlock();
    if (p->imu_valid) {
        p->heading = ls_compass_heading(&p->imu, s_live.calibrated ? &calibration : NULL);
    }
    s_live.calibration_faces = s_fit.faces;
    s_live.calibration_samples = (uint16_t)s_fit.samples;
    s_live.calibration_cover = (uint8_t)ls_compass_tumble_cover(&s_fit);
    s_live.calibration_step = s_guide.step;
    s_live.calibration_hold = s_guide.hold;
    s_live.calibration_aligned = s_guide.aligned;
    if (provider) p->radio_valid = provider(p);
    else if (p->source == LS_FIELD_MESH) {
        ls_mesh_stats_t mesh; ls_mesh_get_stats(&mesh);
        ls_mesh_radio_t radio; ls_mesh_get_radio(&radio);
        p->radio_valid = mesh.running && !s_live.direct && mesh.rx_packets > 0;
        p->signal_valid = p->radio_valid;
        p->frequency = radio.freq_hz; p->packets = mesh.rx_packets;
        p->rssi = mesh.last_rssi; p->snr = mesh.last_snr;
    } else if (p->source == LS_FIELD_LORA) {
        p->radio_valid = s_live.direct;
        p->signal_valid = p->radio_valid && !s_live.transmitting && s_live.mode != LS_LAB_SPECTRUM;
        p->frequency = s_live.config.freq_hz; p->packets = s_live.rx;
        p->rssi = s_live.trace[LS_FIELD_BINS - 1];
    } else if (p->source == LS_FIELD_RTL || p->source == LS_FIELD_HACKRF) {
        const char *id = p->source == LS_FIELD_RTL ? LS_RADIO_ENDPOINT_RTL_USB : LS_RADIO_ENDPOINT_HACKRF_USB;
        radio_health_snapshot_t health;
        p->radio_valid = radio_health_get(id, &health) && health.state == RH_OK;
        p->packets = p->source == LS_FIELD_RTL ? (uint32_t)perf_get_msgs_total() : 0;
        ls_radio_endpoint_info_t endpoint;
        if (ls_radio_endpoint_get(id, &endpoint) == LS_RADIO_OK && endpoint.configured)
            p->frequency = (uint32_t)endpoint.actual_iq.center_hz;
    } else if (p->source == LS_FIELD_WIFI || p->source == LS_FIELD_BLE) {
        ls_wireless_get(&s_wireless);
        bool wifi = p->source == LS_FIELD_WIFI;
        p->radio_valid = wifi ? s_wireless.wifi_connected : s_wireless.bt_ready;
        p->signal_valid = p->radio_valid && (wifi ? s_wireless.wifi_signal : s_wireless.bt_signal);
        p->rssi = wifi ? s_wireless.wifi_rssi : s_wireless.bt_rssi;
        if (wifi && s_wireless.channel >= 1 && s_wireless.channel <= 13)
            p->frequency = (uint32_t)(2407 + 5 * s_wireless.channel) * 1000000u;
        if (!wifi) p->packets = s_wireless.rx;
    }
    s_live.sequence++;
}

void ls_field_step(void)
{
    ls_trail(LS_TRAIL_FIELD, "step");
    const int64_t now = esp_timer_get_time();
    const int profile=ls_keypad_present()?1:0;
    if(profile!=s_profile) {
        s_profile=profile;s_next_sample=0;
        if(s_live.calibrating) {
            s_live.calibrating=false;s_live.calibration_failed=false;
            ls_compass_begin(&s_fit);ls_compass_guide_begin(&s_guide);
            snprintf(s_live.compass_status,sizeof(s_live.compass_status),"Setup changed; calibration cancelled. Restart for this setup.");
        }
    }
    s_live.calibration_keyboard=profile!=0;
    lock(); bool want = s_want, stop = s_stop, watching = s_watch; s_stop = false; s_live.recording = s_record; unlock();
    lock(); bool wireless = (watching || s_record) && (s_source == LS_FIELD_WIFI || s_source == LS_FIELD_BLE); unlock();
    if (wireless != s_wireless_watch) { s_wireless_watch = wireless; ls_wireless_observe(wireless); }
    s_live.requested = want;
    if (stop || (!want && (s_live.direct || s_live.busy))) {
        if (!restore()) { publish(); return; }
    }
    if (want && !s_live.direct) {
        if (!s_live.busy) {
            if (!ls_lora_present() || ls_lora_fsk_active() || ls_lora_scanning()) {
                message("Radio absent or in use; DIRECT unavailable");
                lock(); s_want = false; unlock(); publish(); return;
            }
            s_live.busy = true; s_hold_since = now;
            ls_mesh_radio_hold(true);
            message("Waiting for mesh radio");
        }
        if (ls_mesh_radio_held()) {
            const ls_lora_cfg_t *previous = ls_lora_cfg();
            s_have_saved = previous != NULL;
            if (previous) s_saved = *previous;
            s_saved_rx = ls_lora_is_receiving();
            if (configure()) { s_live.direct = true; s_live.busy = false; message("Direct control; mesh paused"); }
            else { lock(); s_want = false; unlock(); message("Radio setup failed; restoring mesh"); restore(); }
        } else if (now - s_hold_since > 3000000) {
            lock(); s_want = false; unlock(); restore(); message("Mesh busy; direct request timed out");
        }
    }
    lock();
    bool have_command = s_qcount > 0;
    if (have_command) { s_command = s_queue[s_qhead]; s_qhead = (s_qhead + 1) % QUEUE_CAP; s_qcount--; }
    unlock();
    if (have_command) {
        command *c = &s_command;
        if (c->kind == CMD_CAL) {
            if (c->value == 0) {
                ls_compass_begin(&s_fit); s_live.calibrating = true;
                ls_compass_guide_begin(&s_guide);
                s_live.calibration_failed = false;
                s_live.calibration_step = s_live.calibration_hold = 0;
                s_live.calibration_aligned = false;
                snprintf(s_live.compass_status, sizeof(s_live.compass_status), "Follow the picture; each position advances itself");
            } else if (c->value == 2) {
                s_live.calibrating = false;
                snprintf(s_live.compass_status, sizeof(s_live.compass_status), "Calibration cancelled; previous values retained");
            } else if (s_live.calibrating) {
                if (s_fit.tumble) s_guide.step = LS_COMPASS_STEPS;
                finish_calibration();
            }
        } else if (c->kind == CMD_CLEAR) {
            memset(s_live.bearing_count, 0, sizeof(s_live.bearing_count));
            for (int i = 0; i < LS_FIELD_BINS; i++) s_live.trace[i] = s_live.spectrum[i] = -140;
            s_live.spectrum_us=0;message("Plot cleared; radio counters retained");
        } else if (s_live.transmitting) message("Transmission in progress; control unchanged");
        else if (c->kind == CMD_CONFIG || c->kind == CMD_MODE || c->kind == CMD_FSK) {
            ls_lora_cfg_t old = s_live.config; ls_lab_mode_t mode = s_live.mode;
            ls_fsk_cfg_t old_fsk = s_live.fsk;
            if (c->kind == CMD_CONFIG) s_live.config = c->cfg;
            else if (c->kind == CMD_FSK) s_live.fsk = c->fsk;
            else s_live.mode = c->mode;
            if (s_live.direct && !configure()) {
                s_live.config = old; s_live.mode = mode; s_live.fsk = old_fsk;
                if (!configure()) { lock(); s_want = false; unlock(); restore(); }
                message("Setting rejected; previous configuration retained");
            } else {
                memset(s_live.bearing_count, 0, sizeof(s_live.bearing_count));
                for (int i = 0; i < LS_FIELD_BINS; i++) s_live.trace[i] = s_live.spectrum[i] = -140;
                message(s_live.direct ? "Direct settings applied" : "Settings ready; enable DIRECT to apply");
            }
        } else if (c->kind == CMD_TX) {
            /* The FSK session is receive-only, so GFSK and POCSAG have no
               transmit path at all - not a refusal by policy, there is
               nothing to call. */
            if (!s_live.direct || !want || s_live.mode == LS_LAB_SPECTRUM ||
                LS_LAB_IS_FSK(s_live.mode))
                message("SEND needs DIRECT packet or bearing mode");
            else if (ls_lora_send((const uint8_t *)c->entry.text, strlen(c->entry.text)) == ESP_OK) {
                s_live.transmitting = true; s_live.tx++;
                s_tx_deadline = now + ((int64_t)ls_lora_airtime_ms((int)strlen(c->entry.text)) + 2000) * 1000;
                message("Sending one packet");
            } else message("Radio refused packet");
        }
    }
    if (s_live.transmitting) {
        if (ls_lora_send_done()) { s_live.transmitting = false; ls_lora_receive(); message("Packet finished; receiving"); }
        else if (now > s_tx_deadline) { s_live.transmitting = false; configure(); message("TX timeout; receiver reset"); }
    }
    if (s_live.direct && !s_live.transmitting && s_live.mode != LS_LAB_SPECTRUM) {
        uint8_t packet[255]; float rssi = NAN, snr = NAN;
        /* The FSK demodulator reports level but not signal-to-noise: it has
           no reference to measure one against. NAN rather than zero, because
           a zero here would be read as a real 0 dB. */
        const bool fsk = LS_LAB_IS_FSK(s_live.mode);
        int n = fsk ? ls_lora_fsk_poll(packet, sizeof(packet), &rssi)
                    : ls_lora_poll(packet, sizeof(packet), &rssi, &snr);
        if (n > 0) {
            s_live.rx++; s_live.packet_len = n < 64 ? n : 64;
            memcpy(s_live.packet, packet, s_live.packet_len);
            detected(now,s_live.config.freq_hz,rssi,snr,false);
            if (s_live.mode == LS_LAB_POCSAG) pocsag_feed(packet, n, now);
            /* The whole packet, not the 64 bytes the live view keeps. Its
               position comes from the last sample; its radio figures come
               from this reception rather than that heartbeat. */
            if (s_record) {
                ls_field_sample_t ps = s_live.sample;
                ps.time_us = now;
                ps.frequency = s_live.config.freq_hz;
                ps.rssi = rssi; ps.snr = snr;
                ps.radio_valid = ps.signal_valid = true;
                if (save_packet(&ps, packet, n)) record_packets++;
                else { record_errors++; s_record = false; s_live.recording = false; }
            }
        } else if (n < 0) s_live.bad++;
    }
    if ((watching || s_live.recording || s_live.direct || s_live.calibrating) && now >= s_next_sample) {
        s_next_sample = now + (s_live.direct && s_live.mode==LS_LAB_SPECTRUM?20000:100000);
        if (s_live.direct && !s_live.transmitting) {
            if (s_live.mode == LS_LAB_SPECTRUM) {
                bool done=false;int bins=ls_lora_scan_pass(s_scan_work,LS_FIELD_BINS,&done);
                if(bins!=LS_FIELD_BINS) {s_live.spectrum_errors++;s_live.spectrum_us=0;}
                else if(done) {memcpy(s_live.spectrum,s_scan_work,sizeof(s_scan_work));s_live.spectrum_us=esp_timer_get_time();s_live.spectrum_sweeps++;}
            } else {
                float rssi = -140;
                ls_lora_rssi_inst(&rssi);
                memmove(s_live.trace, s_live.trace + 1, sizeof(float) * (LS_FIELD_BINS - 1));
                s_live.trace[LS_FIELD_BINS - 1] = rssi;
            }
        }
        sample(now);
        if (!s_live.direct) {
            ls_mesh_stats_t mesh; ls_mesh_get_stats(&mesh);
            if (s_mesh_baseline && mesh.rx_packets>s_mesh_seen && mesh.running) {
                ls_mesh_radio_t radio; ls_mesh_get_radio(&radio);
                detected(now,radio.freq_hz,mesh.last_rssi,mesh.last_snr,true);
            }
            s_mesh_seen=mesh.rx_packets; s_mesh_baseline=true;
        } else s_mesh_baseline=false;
        if(now>=s_next_peers) {
            s_next_peers=now+1000000; s_live.peer_count=0;
            for(int rank=0;rank<LS_MESH_MAX_PEERS && s_live.peer_count<3;rank++) {
                ls_mesh_peer_t peer;
                if(!ls_mesh_peer_at(rank,&peer))break;
                if(!peer.adverts)continue;
                ls_field_peer_t *out=&s_live.peers[s_live.peer_count++];
                snprintf(out->name,sizeof(out->name),"%s",peer.name);
                snprintf(out->id,sizeof(out->id),"%s",peer.id);out->rssi=peer.rssi;
            }
        }
        if (!s_live.direct && s_live.sample.source == LS_FIELD_MESH && s_live.sample.signal_valid) {
            memmove(s_live.trace, s_live.trace + 1, sizeof(float) * (LS_FIELD_BINS - 1));
            s_live.trace[LS_FIELD_BINS - 1] = s_live.sample.rssi;
        }
    }
    publish();
#ifdef LS_FIELD_TEST
    io_step(); publish();
#endif
}

/* A mark kept by another app is a note too. Before NOTES has imported the
   journal once, the import will carry it; after, it is written here. */
static void mirror_to_notes(const ls_journal_entry_t *e)
{
    char marker[160];
    struct stat st;
    snprintf(marker, sizeof(marker), "%s/.journal-imported", ls_notes_directory());
    if (stat(marker, &st) != 0) return;
    EXT_RAM_BSS_ATTR static char body[LS_JOURNAL_TEXT + 400];
    const ls_field_sample_t *p = &e->sample;
    int n = snprintf(body, sizeof(body), "%s\n", e->text);
    if (p->gps_valid && n > 0 && (size_t)n < sizeof(body))
        n += snprintf(body + n, sizeof(body) - (size_t)n, "> GPS %.6f, %.6f  alt %.0f m\n", p->lat, p->lon, p->alt_m);
    if (p->radio_valid && n > 0 && (size_t)n < sizeof(body)) {
        char level[24] = "";
        if (p->signal_valid) snprintf(level, sizeof(level), "  %.0f dBm", p->rssi);
        n += snprintf(body + n, sizeof(body) - (size_t)n, "> RADIO %s  %.4f MHz%s\n",
                      ls_field_source_name(p->source), p->frequency / 1e6, level);
    }
    if (p->imu_valid && isfinite(p->heading) && n > 0 && (size_t)n < sizeof(body))
        snprintf(body + n, sizeof(body) - (size_t)n, "> HEADING %03.0f M\n", p->heading);
    ls_notes_mark(e->title, body);
}

static void io_step(void)
{
    if (!s_loaded) {
        load_notes();
        for(int profile=0;profile<2;profile++) for(int slot=0;slot<2;slot++) {
            char name[160],leaf[40];
            snprintf(leaf,sizeof(leaf),"compass-%s%d.cal",profile?"kbd":"solo",slot);
            if(!path(name,sizeof(name),leaf))continue;
            FILE *f=fopen(name,"rb"); compass_record record;
            bool valid=f && fread(&record,sizeof(record),1,f)==1 &&
                record.magic==0x43414c10u+(unsigned)profile && record.generation>0 &&
                record.sum==checksum(&record,sizeof(record)-sizeof(record.sum)) && ls_compass_cal_valid(&record.cal);
            if(f)fclose(f);
            if(valid) {
                lock();compass_profile *c=&s_profiles[profile];
                if(c->version==c->written && (!c->valid || (int32_t)(record.generation-c->version)>0)) {
                    c->cal=record.cal;c->valid=c->saved=true;c->version=c->written=record.generation;
                }
                unlock();
            }
        }
        /* Old compass0/1 files lack attachment identity. Preserve them without
           guessing which setup they describe. New profiles require calibration.
           Records before 0x43414c10 are hard iron only, with an unchecked
           magnetic basis, and are left unread for the same reason. */
        s_loaded=true;
    }
    for(int profile=0;profile<2;profile++) {
        lock();compass_profile c=s_profiles[profile];unlock();
        if(c.version==c.written)continue;
        char name[160],leaf[40];bool saved=false;
        snprintf(leaf,sizeof(leaf),"compass-%s%d.cal",profile?"kbd":"solo",(int)(c.version&1));
        if(path(name,sizeof(name),leaf)) {
#ifdef _WIN32
            mkdir(s_directory);
#else
            mkdir(s_directory,0775);
#endif
            FILE *f=fopen(name,"wb");
            if(f) {
                compass_record record={.magic=0x43414c10u+(unsigned)profile,.generation=c.version,.cal=c.cal};
                record.sum=checksum(&record,sizeof(record)-sizeof(record.sum));
                saved=fwrite(&record,sizeof(record),1,f)==1;
                if(fclose(f)!=0)saved=false;
            }
        }
        lock();
        if(c.version==s_profiles[profile].version)s_profiles[profile].saved=saved;
        s_profiles[profile].written=c.version;
        unlock();
    }
    lock(); bool have_note = s_note_count > 0;
    if (have_note) { s_disk = s_notes[s_note_head]; s_note_head = (s_note_head + 1) % QUEUE_CAP; s_note_count--; }
    bool record = s_record;
    ls_field_sample_t current = s_public.sample;
    bool full = s_count == LS_JOURNAL_CAP && !s_entries[0].saved;
    unlock();
    if (have_note) {
        if (!s_disk.id && full) storage("RAM full; existing unsaved notes retained");
        else {
            const bool fresh = !s_disk.id;
            if (!s_disk.id) s_disk.id = s_next_id++;
            save_note(&s_disk); remember(&s_disk);
            if (fresh) mirror_to_notes(&s_disk);
        }
    }
    const int64_t now = esp_timer_get_time();
    if (record && now >= s_next_record) {
        s_next_record = now + 1000000;
        bool saved=save_sample(&current);lock();
        if(saved){record_rows++;record_saved_us=current.time_us;snprintf(s_live.record_saved_utc,sizeof(s_live.record_saved_utc),"%s",current.utc);}else{record_errors++;s_record=false;s_live.recording=false;}
        unlock();
    }
}

#ifndef LS_FIELD_TEST
static void worker(void *arg)
{
    (void)arg;
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    if (!ls_gps_running()) ls_gps_start();
    for (;;) {
        ls_field_step();
        /* Direction finding reads its radio here, beside the heading it pairs with. */
        ls_dfs_step();
        lock(); bool active = s_watch || s_record || s_want || s_public.direct || s_public.calibrating || s_qcount; unlock();
        vTaskDelay(pdMS_TO_TICKS(active ? 25 : 200));
    }
}
static void io_worker(void *arg)
{
    (void)arg;
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    for (;;) { io_step(); ls_notes_io_step(); vTaskDelay(pdMS_TO_TICKS(100)); }
}
#endif

bool ls_field_start(void)
{
    if (s_started) return true;
    if (!s_lock) s_lock = xSemaphoreCreateMutexStatic(&s_lock_memory);
    if (!s_lock) return false;
    ls_lora_cfg_default(&s_live.config);
    /* The console's proven paging set, which is also a valid generic GFSK
       one: 1200 + 2*4500 fits inside 19500. freq_hz is filled from the LoRa
       config when a session starts, so BAND moves every mode together. */
    s_live.fsk = (ls_fsk_cfg_t){ .freq_hz = s_live.config.freq_hz,
        .bitrate = 1200, .deviation_hz = POCSAG_DEVIATION,
        .bandwidth_hz = POCSAG_BANDWIDTH, .sync_word = POCSAG_SYNC,
        .payload_bytes = POCSAG_BYTES };
    for (int i = 0; i < LS_FIELD_BINS; i++) s_live.trace[i] = s_live.spectrum[i] = -140;
    message("Mesh keeps control until DIRECT is enabled");
    storage("Journal ready");
    s_live.ready = true;
    publish();
#ifndef LS_FIELD_TEST
    /* Keep both large stacks in PSRAM and their TCBs statically reserved in
       DRAM.  Dynamic WithCaps creation still allocates each TCB internally;
       starting Journal beside the USB stream exhausted that heap before
       newlib could create fopen's lock. */
    s_io_task = xTaskCreateStaticPinnedToCore(
        io_worker, "journal", FIELD_STACK_WORDS, NULL, 1,
        s_io_stack, &s_io_tcb, 0);
    if (!s_io_task) {
        s_live.ready = false; message("Journal worker could not start"); publish(); return false;
    }
    s_task = xTaskCreateStaticPinnedToCore(
        worker, "field", FIELD_STACK_WORDS, NULL, 2,
        s_field_stack, &s_field_tcb, 0);
    if (!s_task) {
        vTaskDelete(s_io_task); s_io_task = NULL;
        s_live.ready = false; message("Field worker could not start"); publish(); return false;
    }
    xTaskNotifyGive(s_io_task);
    xTaskNotifyGive(s_task);
#endif
    s_started = true; return true;
}

/* A request is written straight into its queue slot, under the lock the
   worker takes to read it. Built on the caller's stack, a command (it holds
   a whole journal entry) took a kilobyte of the 6 KB TUI stack per call.
   reserve() returns with the lock held; commit() releases it. */
static command *reserve(void)
{
    if (!s_started) return NULL;
    lock();
    if (s_qcount == QUEUE_CAP) { s_live.drops++; s_public.drops = s_live.drops; unlock(); return NULL; }
    command *c = &s_queue[(s_qhead + s_qcount) % QUEUE_CAP];
    memset(c, 0, sizeof(*c));
    return c;
}

static bool commit(void) { s_qcount++; unlock(); return true; }

void ls_field_sample_snapshot(ls_field_sample_t *out) { if(!out) return; if(!s_lock) { memset(out,0,sizeof(*out)); return; } lock(); *out=s_public.sample; unlock(); }
void ls_field_snapshot(ls_field_state_t *out) { if (!out) return; if (!s_lock) { memset(out, 0, sizeof(*out)); return; } lock(); *out = s_public; unlock(); }
bool ls_field_owned(void) { if (!s_lock) return false; lock(); bool owned = s_want || s_public.direct || s_public.busy; unlock(); return owned; }
bool ls_field_direct(bool on) { if (!s_started) return false; lock(); s_want = on; if (!on) s_stop = true; unlock(); return true; }
bool ls_field_configure(const ls_lora_cfg_t *cfg)
{
    if (!cfg || cfg->freq_hz < 150000000 || cfg->freq_hz > 959000000 || cfg->sf < 5 || cfg->sf > 12 ||
        cfg->bw_hz < 7810 || cfg->bw_hz > 500000 || cfg->cr < 5 || cfg->cr > 8 || cfg->power_dbm < -9 || cfg->power_dbm > 22 || !cfg->preamble) return false;
    command *c = reserve(); if (!c) return false;
    c->kind = CMD_CONFIG; c->cfg = *cfg; return commit();
}
bool ls_field_mode(ls_lab_mode_t mode) { if (mode < LS_LAB_PACKETS || mode > LS_LAB_POCSAG) return false; command *c = reserve(); if (!c) return false; c->kind = CMD_MODE; c->mode = mode; return commit(); }
bool ls_field_configure_fsk(const ls_fsk_cfg_t *cfg)
{
    /* The same limits ls_lora_fsk_begin enforces, checked here so a bad set
       is refused at the button instead of by a session that will not start.
       The bandwidth rule is the demodulator's: the signal has to fit in it. */
    if (!cfg || cfg->bitrate < 600 || cfg->bitrate > 300000 ||
        cfg->deviation_hz < 600 || cfg->deviation_hz > 200000 ||
        cfg->bandwidth_hz < 4800 || cfg->bandwidth_hz > 467000 ||
        !cfg->payload_bytes ||
        cfg->bitrate + 2 * cfg->deviation_hz > cfg->bandwidth_hz) return false;
    command *c = reserve(); if (!c) return false;
    c->kind = CMD_FSK; c->fsk = *cfg; return commit();
}
bool ls_field_calibrate(int action) { if (action < 0 || action > 2) return false; command *c = reserve(); if (!c) return false; c->kind = CMD_CAL; c->value = action; return commit(); }
void ls_field_compass_report(void)
{
    if (!s_lock) { printf("compass: field worker not started (open COMPASS)\n"); return; }
    lock(); const int active = s_profile; compass_profile pr[2]; memcpy(pr, s_profiles, sizeof(pr));
    const bool loaded = s_loaded; unlock();
    printf("compass: active profile %s, files %s\n", active ? "keyboard" : "standalone",
           loaded ? "read" : "NOT read yet");
    for (int profile = 0; profile < 2; profile++) {
        printf("compass: %-10s valid %d saved %d version %u written %u\n", profile ? "keyboard" : "standalone",
               pr[profile].valid, pr[profile].saved, (unsigned)pr[profile].version, (unsigned)pr[profile].written);
        for (int slot = 0; slot < 2; slot++) {
            char name[160], leaf[40];
            snprintf(leaf, sizeof(leaf), "compass-%s%d.cal", profile ? "kbd" : "solo", slot);
            if (!path(name, sizeof(name), leaf)) continue;
            FILE *f = fopen(name, "rb"); compass_record record;
            if (!f) { printf("compass:   %s: absent\n", leaf); continue; }
            const bool whole = fread(&record, sizeof(record), 1, f) == 1;
            fclose(f);
            if (!whole) { printf("compass:   %s: short\n", leaf); continue; }
            const char *problem = ls_compass_cal_problem(&record.cal);
            printf("compass:   %s: magic %s gen %u sum %s cal %s  radius %.1f error %.3f basis %d %d %d\n", leaf,
                   record.magic == 0x43414c10u + (unsigned)profile ? "ok" : "OLD",
                   (unsigned)record.generation,
                   record.sum == checksum(&record, sizeof(record) - sizeof(record.sum)) ? "ok" : "BAD",
                   problem ? problem : "ok", record.cal.radius, record.cal.error,
                   record.cal.basis[0], record.cal.basis[1], record.cal.basis[2]);
        }
    }
}

bool ls_field_compass_cal(ls_compass_cal_t *out)
{
    if (!s_lock) return false;
    lock(); const bool ok = s_profiles[s_profile].valid; if (ok && out) *out = s_profiles[s_profile].cal; unlock();
    return ok;
}
bool ls_field_calibrating(void) { if (!s_lock) return false; lock(); bool active = s_public.calibrating; unlock(); return active; }
bool ls_field_clear_plot(void) { command *c = reserve(); if (!c) return false; c->kind = CMD_CLEAR; return commit(); }
bool ls_field_transmit(const char *text) { if (!text || !*text || strlen(text) > 64) return false; command *c = reserve(); if (!c) return false; c->kind = CMD_TX; snprintf(c->entry.text, sizeof(c->entry.text), "%s", text); return commit(); }
bool ls_field_source(ls_field_source_t source) { if (!s_started || source < 0 || source >= LS_FIELD_SOURCES) return false; lock(); if(s_record && source != s_source) { unlock(); return false; } s_source = source; unlock(); return true; }
bool ls_field_record(bool on) { if (!s_started) return false; lock(); if(on && !s_record){record_rows=record_errors=record_packets=0;record_saved_us=0;}s_record = on; unlock(); return true; }
bool ls_field_recording(void) { if(!s_lock) return false; lock(); bool active=s_record; unlock(); return active; }
void ls_field_watch(bool on) { if (!s_lock) return; lock(); s_watch = on; unlock(); }
bool ls_field_note(uint32_t id, const char *title, const char *text)
{
    if (!s_started || !title || !*title || !text || strlen(title) >= sizeof(s_disk.title) || strlen(text) >= LS_JOURNAL_TEXT) return false;
    /* Written in place, like reserve(): an entry is a kilobyte. */
    lock();
    const ls_field_sample_t *sample = &s_public.sample;
    if (id) {
        unsigned i;
        for (i = 0; i < s_count && s_entries[i].id != id; i++) {}
        if (i == s_count) { unlock(); return false; }
        sample = &s_entries[i].sample;
    }
    if (s_note_count == QUEUE_CAP || (!id && s_count == LS_JOURNAL_CAP && !s_entries[0].saved)) { unlock(); return false; }
    ls_journal_entry_t *e = &s_notes[(s_note_head + s_note_count) % QUEUE_CAP];
    memset(e, 0, sizeof(*e));      /* sample points elsewhere: never into s_notes */
    e->id = id;
    e->sample = *sample;
    snprintf(e->title, sizeof(e->title), "%s", title);
    snprintf(e->text, sizeof(e->text), "%s", text);
    s_note_count++;
    unlock(); return true;
}
bool ls_field_mark_lora(void)
{
    if (!s_started) return false;
    lock();
    if (s_note_count == QUEUE_CAP || (s_count == LS_JOURNAL_CAP && !s_entries[0].saved)) { unlock(); return false; }
    ls_journal_entry_t *entry = &s_notes[(s_note_head + s_note_count++) % QUEUE_CAP];
    memset(entry, 0, sizeof(*entry));
    snprintf(entry->title, sizeof(entry->title), "LoRa observation");
    snprintf(entry->text, sizeof(entry->text), "%s; SF%u, BW %lu Hz, CR 4/%u", s_public.status,
             s_public.config.sf, (unsigned long)s_public.config.bw_hz, s_public.config.cr);
    entry->sample = s_public.sample;
    if (s_public.direct) {
        entry->sample.source = LS_FIELD_LORA;
        entry->sample.frequency = s_public.config.freq_hz;
        entry->sample.packets = s_public.rx;
        entry->sample.radio_valid = true;
        entry->sample.signal_valid = !s_public.transmitting && s_public.mode != LS_LAB_SPECTRUM;
        entry->sample.rssi = s_public.trace[LS_FIELD_BINS - 1];
        entry->sample.snr = NAN;
    }
    unlock(); return true;
}
bool ls_field_mark_radio(const char *title, const char *text,
                         ls_field_source_t source, uint32_t frequency, uint32_t count)
{
    if(!s_started || !title || !*title || !text || strlen(title)>=sizeof(s_disk.title) ||
       strlen(text)>=LS_JOURNAL_TEXT || source<=LS_FIELD_NONE || source>=LS_FIELD_SOURCES || !frequency) return false;
    lock();
    if(s_note_count==QUEUE_CAP || (s_count==LS_JOURNAL_CAP && !s_entries[0].saved)) {unlock();return false;}
    ls_journal_entry_t *entry=&s_notes[(s_note_head+s_note_count++)%QUEUE_CAP];
    memset(entry,0,sizeof(*entry));
    snprintf(entry->title,sizeof(entry->title),"%s",title);
    snprintf(entry->text,sizeof(entry->text),"%s",text);
    entry->sample=s_public.sample;
    int64_t now=esp_timer_get_time();
    if(now-entry->sample.time_us>350000 || now<entry->sample.time_us) {
        entry->sample.gps_valid=entry->sample.imu_valid=false;
        entry->sample.utc[0]=0;
        entry->sample.time_us=now;
    }
    entry->sample.source=source;entry->sample.frequency=frequency;entry->sample.packets=count;
    entry->sample.radio_valid=true;entry->sample.signal_valid=false;
    entry->sample.rssi=entry->sample.snr=NAN;
    unlock();return true;
}
bool ls_field_entry(int index, ls_journal_entry_t *out)
{
    if (!s_lock || !out || index < 0) return false;
    lock(); bool ok = (unsigned)index < s_count; if (ok) *out = s_entries[s_count - 1 - index]; unlock(); return ok;
}
bool ls_field_provider(ls_field_source_t source, ls_field_provider_t provider)
{
    if (!s_started || source <= LS_FIELD_NONE || source >= LS_FIELD_SOURCES) return false;
    lock(); s_providers[source] = provider; unlock(); return true;
}
#ifdef LS_FIELD_TEST
void ls_field_test_reset(const char *directory)
{
    s_directory = directory; s_started = s_loaded = s_stop = s_want = s_record = s_have_saved = false;
    s_qhead = s_qcount = s_count = s_note_head = s_note_count = 0; s_next_id = 1; s_next_sample = s_next_record = 0;
    s_valid_bytes = 0; s_damaged = s_incompatible = false;
    s_wireless_watch = false;record_rows=record_errors=record_packets=0;record_saved_us=0;
    s_last_imu_us = 0;
    s_mesh_baseline=false;s_next_peers=0;s_mesh_seen=0;
    memset(s_profiles,0,sizeof(s_profiles));s_profile=ls_keypad_present()?1:0;
    ls_compass_begin(&s_fit);
    ls_compass_guide_begin(&s_guide);
    memset(&s_live, 0, sizeof(s_live)); memset(s_providers, 0, sizeof(s_providers));
    ls_field_start(); ls_field_watch(true); io_step(); publish();
}
#endif

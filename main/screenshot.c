/* Screenshot to a file, not down the console. */

#include "screenshot.h"

#include <dirent.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"
#include "rec_state.h"      /* rec_dir() - same storage fallback as captures */
#include "rec_unique_name.h"
/**/
#include "rec_space.h"
/**/
#include "ls_time.h"

static const char *TAG = "shot";

#if LV_COLOR_DEPTH != 16
#error "screenshot BMP conversion requires LVGL RGB565"
#endif

/* the console task has a measured ~4 KiB stack; asking it to recurse
   through a full P25 object tree reached lv_font_get_glyph_dsc_fmt_txt with SP
   already below that task's bounds.  The handoff below puts only the render
   on taskLVGL, whose existing 7168-byte stack is already sized for LVGL.  Both
   semaphores and the request are static internal data: no screenshot task (or
   dynamic internal task stack) competes with the RTL USB path. */
typedef struct {
    lv_img_dsc_t image;
    void        *pixels;
    size_t       pixel_capacity;
    char         path[128];
    char         part_path[160];
    screenshot_result_t render_result;
    bool         notify;
} screenshot_work_t;

static DRAM_ATTR StaticSemaphore_t s_gate_storage;
static DRAM_ATTR StaticSemaphore_t s_done_storage;
static SemaphoreHandle_t s_gate;
static SemaphoreHandle_t s_done;
static screenshot_work_t s_work;

bool screenshot_init(void)
{
    if (s_gate && s_done) return true;
    s_gate = xSemaphoreCreateMutexStatic(&s_gate_storage);
    s_done = xSemaphoreCreateBinaryStatic(&s_done_storage);
    return s_gate != NULL && s_done != NULL;
}

/* 24-bit BMP: bottom-up rows, BGR, each row padded to 4 bytes. Chosen over
   PNG because it needs no compressor on the device and every viewer opens it. */
static bool write_bmp(const char *path, const uint16_t *px, int w, int h,
                      uint32_t *bytes_out, uint32_t *sum_out)
{
    FILE *f = fopen(path, "wx");
    if (!f) { ESP_LOGE(TAG, "cannot open %s", path); return false; }

    const uint32_t row_bytes = (uint32_t)((w * 3 + 3) & ~3);
    const uint32_t img_bytes = row_bytes * (uint32_t)h;
    const uint32_t off       = 14 + 40;
    const uint32_t total     = off + img_bytes;

    uint8_t hdr[54];
    memset(hdr, 0, sizeof(hdr));
    hdr[0] = 'B'; hdr[1] = 'M';
    memcpy(hdr + 2,  &total, 4);
    memcpy(hdr + 10, &off,   4);
    uint32_t dib = 40;            memcpy(hdr + 14, &dib, 4);
    int32_t  iw  = w;             memcpy(hdr + 18, &iw,  4);
    int32_t  ih  = h;             memcpy(hdr + 22, &ih,  4);
    uint16_t planes = 1, bpp = 24;
    memcpy(hdr + 26, &planes, 2); memcpy(hdr + 28, &bpp, 2);
    memcpy(hdr + 34, &img_bytes, 4);
    if (fwrite(hdr, 1, sizeof(hdr), f) != sizeof(hdr)) { fclose(f); return false; }

    /* at 480 pixels this is already 1440 bytes.  Plain malloc keeps
       allocations below SPIRAM_MALLOC_ALWAYSINTERNAL in scarce internal RAM,
       exactly where the SD/RTL DMA paths need headroom.  BMP conversion is
       ordinary task code with caches enabled, so external RAM is valid. */
    uint8_t *row = heap_caps_malloc(row_bytes,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!row) { fclose(f); ESP_LOGE(TAG, "row buffer alloc failed"); return false; }
    memset(row, 0, row_bytes);

    uint32_t sum = 0;
    for (int y = h - 1; y >= 0; y--) {                 /* BMP is bottom-up */
        const uint16_t *src = px + (size_t)y * w;
        for (int x = 0; x < w; x++) {
            uint16_t v = src[x];
            uint8_t r = (uint8_t)(((v >> 11) & 0x1F) << 3);
            uint8_t g = (uint8_t)(((v >> 5)  & 0x3F) << 2);
            uint8_t b = (uint8_t)(( v        & 0x1F) << 3);
            row[x * 3 + 0] = b;                        /* BMP is BGR */
            row[x * 3 + 1] = g;
            row[x * 3 + 2] = r;
            sum += (uint32_t)r + g + b;
        }
        if (fwrite(row, 1, row_bytes, f) != row_bytes) {
            heap_caps_free(row); fclose(f);
            ESP_LOGE(TAG, "short write - disk full?");
            return false;
        }
    }
    heap_caps_free(row);
    int flush_result = fflush(f);
    int file_error = ferror(f);
    int close_result = fclose(f);
    if (flush_result != 0 || file_error || close_result != 0) {
        ESP_LOGE(TAG, "flush/close failed - disk full?");
        return false;
    }

    if (bytes_out) *bytes_out = total;
    if (sum_out)   *sum_out   = sum;
    return true;
}

static screenshot_result_t screenshot_dimensions(bool take_lock,
                                                 uint32_t *width,
                                                 uint32_t *height)
{
    if (take_lock && !lvgl_port_lock(2000))
        return SCREENSHOT_ERR_LVGL_LOCK;

    lv_disp_t *disp = lv_disp_get_default();
    if (disp) {
        *width = (uint32_t)lv_disp_get_hor_res(disp);
        *height = (uint32_t)lv_disp_get_ver_res(disp);
    } else {
        *width = 0;
        *height = 0;
    }

    if (take_lock) lvgl_port_unlock();
    return (*width && *height) ? SCREENSHOT_OK : SCREENSHOT_ERR_DIMENSIONS;
}

static screenshot_result_t screenshot_prepare(const char *name,
                                               bool take_lvgl_lock)
{
    uint32_t width = 0, height = 0;
    screenshot_result_t result = screenshot_dimensions(take_lvgl_lock,
                                                       &width, &height);
    if (result != SCREENSHOT_OK) return result;

    screenshot_plan_t plan;
    const uint64_t storage_available = rec_dir_free_bytes();
    const size_t psram_available =
        heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    const size_t psram_largest =
        heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    result = screenshot_plan_make(width, height, storage_available,
                                  psram_available, psram_largest, &plan);
    if (result != SCREENSHOT_OK) {
        ESP_LOGE(TAG, "%s (bmp=%llu B free=%s, frame=%u B psram=%u/%u B)",
                 screenshot_result_message(result),
                 (unsigned long long)plan.bmp_bytes,
                 storage_available == UINT64_MAX ? "?" : "known",
                 (unsigned)plan.snapshot_bytes, (unsigned)psram_available,
                 (unsigned)psram_largest);
        return result;
    }

    char clean[32];
    snprintf(clean, sizeof(clean), "%s", (name && *name) ? name : "screen");
    for (char *p = clean; *p; p++)
        if (*p == '/' || *p == 92 || *p == ' ') *p = '_';   /* 92 = backslash */

    /* Use the same filename-safe time component as recordings so a
       file browser sorts both from the time they were made. Before SNTP the
       up- prefix remains an unmistakable uptime marker. */
    char base[80];
    ls_time_render_filename(base, sizeof(base), clean);
    char unique[96];
    if (rec_pick_unique_name(rec_dir(), base, ".bmp",
                             unique, sizeof(unique)) != 0) {
        ESP_LOGE(TAG, "no free screenshot name for %s in %s", base, rec_dir());
        return SCREENSHOT_ERR_NAME;
    }

    int n = snprintf(s_work.path, sizeof(s_work.path), "%s/%s.bmp",
                     rec_dir(), unique);
    if (n < 0 || (size_t)n >= sizeof(s_work.path))
        return SCREENSHOT_ERR_NAME;
    n = snprintf(s_work.part_path, sizeof(s_work.part_path), "%s%s",
                 s_work.path, REC_CAPTURE_PART_EXT);
    if (n < 0 || (size_t)n >= sizeof(s_work.part_path))
        return SCREENSHOT_ERR_NAME;

    s_work.pixels = heap_caps_malloc(plan.snapshot_bytes,
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_work.pixels) {
        ESP_LOGE(TAG, "external RAM allocation failed for %u-byte frame",
                 (unsigned)plan.snapshot_bytes);
        return SCREENSHOT_ERR_PSRAM_SHORT;
    }
    s_work.pixel_capacity = plan.snapshot_bytes;
    memset(&s_work.image, 0, sizeof(s_work.image));
    return SCREENSHOT_OK;
}

static void screenshot_render(void *unused)
{
    (void)unused;
    lv_obj_t *scr = lv_scr_act();
    uint32_t needed = scr ? lv_snapshot_buf_size_needed(
                                scr, LV_IMG_CF_TRUE_COLOR) : 0;
    if (!scr || needed == 0 || needed > s_work.pixel_capacity) {
        s_work.render_result = SCREENSHOT_ERR_RENDER;
    } else if (lv_snapshot_take_to_buf(scr, LV_IMG_CF_TRUE_COLOR,
                                       &s_work.image, s_work.pixels,
                                       (uint32_t)s_work.pixel_capacity) !=
               LV_RES_OK) {
        s_work.render_result = SCREENSHOT_ERR_RENDER;
    } else {
        s_work.render_result = SCREENSHOT_OK;
    }
    if (s_work.notify) xSemaphoreGive(s_done);
}

static screenshot_result_t screenshot_write(void)
{
    if (s_work.render_result != SCREENSHOT_OK) {
        ESP_LOGE(TAG, "%s", screenshot_result_message(s_work.render_result));
        return s_work.render_result;
    }

    /**/

    uint32_t bytes = 0, sum = 0;
    bool ok = write_bmp(s_work.part_path,
                        (const uint16_t *)(const void *)s_work.image.data,
                        s_work.image.header.w, s_work.image.header.h,
                        &bytes, &sum);

    if (ok && rename(s_work.part_path, s_work.path) != 0) {
        ESP_LOGE(TAG, "rename %s -> %s failed", s_work.part_path, s_work.path);
        (void)unlink(s_work.part_path);
        ok = false;
    } else if (!ok) {
        (void)unlink(s_work.part_path);
    }

    ESP_LOGW(TAG, "%s %s %ux%u %u B checksum=%08x",
             ok ? "wrote" : "FAILED", s_work.path,
             (unsigned)s_work.image.header.w,
             (unsigned)s_work.image.header.h,
             (unsigned)bytes, (unsigned)sum);
    return ok ? SCREENSHOT_OK : SCREENSHOT_ERR_WRITE;
}

static void screenshot_release(void)
{
    if (s_work.pixels) heap_caps_free(s_work.pixels);
    memset(&s_work, 0, sizeof(s_work));
    xSemaphoreGive(s_gate);
}

static screenshot_result_t screenshot_save_impl(const char *name,
                                                char *path_out,
                                                size_t path_len,
                                                bool dispatch_render)
{
    if (path_out && path_len) path_out[0] = '\0';
    if (!s_gate || !s_done) return SCREENSHOT_ERR_NOT_READY;
    if (xSemaphoreTake(s_gate, 0) != pdTRUE) return SCREENSHOT_ERR_BUSY;

    screenshot_result_t result = screenshot_prepare(name, dispatch_render);
    if (result != SCREENSHOT_OK) {
        screenshot_release();
        return result;
    }

    s_work.render_result = SCREENSHOT_ERR_RENDER;
    s_work.notify = dispatch_render;
    if (dispatch_render) {
        /* Drain the binary semaphore from a prior completed request before
           publishing this work item. */
        (void)xSemaphoreTake(s_done, 0);
        if (!lvgl_port_lock(2000)) {
            result = SCREENSHOT_ERR_LVGL_LOCK;
        } else {
            lv_res_t queued = lv_async_call(screenshot_render, NULL);
            lvgl_port_unlock();
            if (queued != LV_RES_OK) result = SCREENSHOT_ERR_QUEUE;
            else {
                /* Once LVGL accepted a pointer to the static work item it owns
                   that item until the callback completes.  Waiting here is
                   safer than timing out and letting a late callback render
                   into a buffer the caller has freed. */
                (void)xSemaphoreTake(s_done, portMAX_DELAY);
                result = s_work.render_result;
            }
        }
    } else {
        /* Called from an LVGL event: taskLVGL already owns the recursive port
           lock, and queueing then waiting here would deadlock that task. */
        screenshot_render(NULL);
        result = s_work.render_result;
    }

    if (result == SCREENSHOT_OK) result = screenshot_write();
    if (result == SCREENSHOT_OK && path_out && path_len)
        snprintf(path_out, path_len, "%s", s_work.path);

    screenshot_release();
    return result;
}

bool screenshot_save(const char *name, char *path_out, size_t path_len)
{
    screenshot_result_t result = screenshot_save_impl(name, path_out, path_len,
                                                      false);
    if (result != SCREENSHOT_OK)
        ESP_LOGE(TAG, "%s", screenshot_result_message(result));
    return result == SCREENSHOT_OK;
}

screenshot_result_t screenshot_save_from_task(const char *name,
                                               char *path_out,
                                               size_t path_len)
{
    screenshot_result_t result = screenshot_save_impl(name, path_out, path_len,
                                                      true);
    if (result != SCREENSHOT_OK)
        ESP_LOGE(TAG, "%s", screenshot_result_message(result));
    return result;
}

int screenshot_list(char *out, size_t len)
{
    DIR *d = opendir(rec_dir());
    if (!d) return -1;
    int n = 0, off = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        const char *dot = strrchr(e->d_name, '.');
        if (!dot || strcmp(dot, ".bmp") != 0) continue;
        char full[128];
        snprintf(full, sizeof(full), "%s/%s", rec_dir(), e->d_name);
        struct stat st;
        long sz = (stat(full, &st) == 0) ? (long)st.st_size : -1;
        if (out && off < (int)len - 40)
            off += snprintf(out + off, len - off, "%s (%ld B)\n", e->d_name, sz);
        n++;
    }
    closedir(d);
    return n;
}

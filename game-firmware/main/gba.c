#include "gba.h"
#include "display.h"
#include "e3d.h"
#include "game_storage.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_private/esp_cache_private.h"
#include "esp_vfs.h"
#include "gpsp_rv32_platform.h"
#include "hal/cache_ll.h"
#include "libretro.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>

enum { WIDTH = 240, HEIGHT = 160, ROM_BYTES = 32768 };
static const char *TAG = "gba";
static const char *const demo_path = "/demo/signal_catch.gba";
static const char *const sd_path = "/sd/game.gba";
extern const uint8_t demo_start[] asm("_binary_signal_catch_gba_start");
extern const uint8_t demo_end[] asm("_binary_signal_catch_gba_end");
static bool loaded, demo_opened, demo_mounted;
static size_t demo_position;
static uint16_t *canvas, pad;
static const uint16_t *last_canvas;
static unsigned frames, changes;
static uint32_t last_hash;

static int rom_open(const char *name, int flags, int mode)
{
    if (strcmp(name, "/signal_catch.gba") || (flags & O_ACCMODE) != O_RDONLY || demo_opened) {
        errno = ENOENT;
        return -1;
    }
    demo_opened = true;
    demo_position = 0;
    return 0;
}

static int rom_close(int fd)
{
    demo_opened = false;
    return 0;
}

static ssize_t rom_read(int fd, void *buffer, size_t length)
{
    size_t available = ROM_BYTES - demo_position;
    if (length > available) length = available;
    size_t bytes = demo_end - demo_start;
    size_t code = demo_position < bytes ? bytes - demo_position : 0;
    if (code > length) code = length;
    if (code) memcpy(buffer, demo_start + demo_position, code);
    memset((uint8_t *)buffer + code, 0xFF, length - code);
    demo_position += length;
    return length;
}

static off_t rom_seek(int fd, off_t offset, int origin)
{
    int64_t absolute = offset;
    if (origin == SEEK_CUR) absolute += demo_position;
    else if (origin == SEEK_END) absolute += ROM_BYTES;
    if (absolute < 0 || absolute > ROM_BYTES) {
        errno = EINVAL;
        return -1;
    }
    return demo_position = absolute;
}

static int rom_stat(int fd, struct stat *out)
{
    *out = (struct stat){ .st_mode = S_IFREG | S_IRUSR, .st_size = ROM_BYTES };
    return 0;
}

static bool mount_demo(void)
{
    const esp_vfs_t fs = {
        .open = rom_open, .close = rom_close, .read = rom_read, .lseek = rom_seek, .fstat = rom_stat,
    };
    if (!demo_mounted) demo_mounted = esp_vfs_register("/demo", &fs, NULL) == ESP_OK;
    return demo_mounted;
}

static void *code_alloc(size_t alignment, size_t length)
{
    return heap_caps_aligned_alloc(alignment > 256 ? alignment : 256, length, MALLOC_CAP_SPIRAM);
}

static bool code_publish(void *code, size_t size)
{
    if (!size) return true;
    size_t line;
    if (esp_cache_get_alignment(MALLOC_CAP_SPIRAM, &line) != ESP_OK || !line) return false;
    uintptr_t start = (uintptr_t)code & ~(line - 1);
    uintptr_t end = ((uintptr_t)code + size + line - 1) & ~(line - 1);
    /* The P4's shared L2 serves its split L1 data and instruction caches. */
    esp_cache_sync_ops_enter_critical_section();
    cache_ll_l1_writeback_dcache_addr(CACHE_LL_ID_ALL, start, end - start);
    cache_ll_l1_invalidate_icache_addr(CACHE_LL_ID_ALL, start, end - start);
    esp_cache_sync_ops_exit_critical_section();
    asm volatile("fence.i" ::: "memory");
    return true;
}

static void core_log(enum retro_log_level level, const char *format, ...)
{
    if (level < RETRO_LOG_INFO) return;
    va_list args;
    va_start(args, format);
    esp_log_writev(level >= RETRO_LOG_WARN ? ESP_LOG_WARN : ESP_LOG_INFO, TAG, format, args);
    va_end(args);
}

static bool environment(unsigned command, void *data)
{
    switch (command) {
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
        return *(enum retro_pixel_format *)data == RETRO_PIXEL_FORMAT_RGB565;
    case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS:
        return true;
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
        ((struct retro_log_callback *)data)->log = core_log;
        return true;
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
        *(const char **)data = "/sd";
        return true;
    default:
        return false;
    }
}

static int16_t controller(unsigned port, unsigned device, unsigned index, unsigned id)
{
    if (port || device != RETRO_DEVICE_JOYPAD) return 0;
    if (id == RETRO_DEVICE_ID_JOYPAD_MASK) return pad;
    return id < 16 && (pad & (1u << id));
}

static void poll_input(void) {}

static size_t audio_sink(const int16_t *samples, size_t n)
{
    return n;
}

static void video(const void *data, unsigned width, unsigned height, size_t bytes_per_row)
{
    if (!data || width != WIDTH || height != HEIGHT) return;
    enum { SCALED_WIDTH = WIDTH * 3 / 2, LEFT = (GAME_WIDTH - SCALED_WIDTH) / 2 };
    uint32_t hash = 2166136261;
    for (int y = 0; y < HEIGHT; y += 2) {
        const uint16_t *a = (const uint16_t *)((const uint8_t *)data + y * bytes_per_row);
        const uint16_t *b = (const uint16_t *)((const uint8_t *)a + bytes_per_row);
        uint16_t *out = canvas + (y * 3 / 2) * GAME_WIDTH + LEFT;
        for (int row = 0; row < 3; ++row) {
            memset(out + row * GAME_WIDTH - LEFT, 0, LEFT * sizeof(*out));
            memset(out + row * GAME_WIDTH + SCALED_WIDTH, 0, LEFT * sizeof(*out));
        }
        for (int x = 0; x < WIDTH; x += 2) {
            int u = x * 3 / 2;
            out[u] = out[u + 1] = out[GAME_WIDTH + u] = out[GAME_WIDTH + u + 1] = a[x];
            out[u + 2] = out[GAME_WIDTH + u + 2] = a[x + 1];
            out[2 * GAME_WIDTH + u] = out[2 * GAME_WIDTH + u + 1] = b[x];
            out[2 * GAME_WIDTH + u + 2] = b[x + 1];
        }
        /* A cheap row sample catches changing video without hashing each pixel. */
        hash = (hash ^ a[18] ^ ((uint32_t)a[110] << 16) ^ b[218]) * 16777619u;
    }
    changes += hash != last_hash;
    last_hash = hash;
    last_canvas = canvas;
    ++frames;
}

bool gba_open(bool from_sd)
{
    const struct retro_game_info game = { .path = from_sd ? sd_path : demo_path };
    if (loaded) gba_close();
    if (!(from_sd ? game_storage_mount() == ESP_OK : mount_demo())) return false;

    static const struct gpsp_rv32_platform_ops platform = {
        .exec_alloc = code_alloc, .exec_free = heap_caps_free, .exec_publish = code_publish,
    };
    gpsp_rv32_set_platform(&platform);
    retro_set_environment(environment);
    retro_set_video_refresh(video);
    retro_set_audio_sample_batch(audio_sink);
    retro_set_input_poll(poll_input);
    retro_set_input_state(controller);
    retro_init();
    loaded = retro_load_game(&game);
    if (!loaded) retro_deinit();
    frames = changes = last_hash = 0;
    last_canvas = NULL;
    ESP_LOGI(TAG, "%s %s", loaded ? "Loaded" : "Failed to load", game.path);
    return loaded;
}

void gba_close(void)
{
    if (!loaded) return;
    retro_unload_game();
    retro_deinit();
    loaded = false;
}

void gba_discard_frame(void)
{
    last_canvas = NULL;
}

void gba_frame(uint16_t *frame, tk21_state input)
{
    static const uint8_t joypad[] = {RETRO_DEVICE_ID_JOYPAD_A, RETRO_DEVICE_ID_JOYPAD_B,
        RETRO_DEVICE_ID_JOYPAD_START, RETRO_DEVICE_ID_JOYPAD_SELECT,
        RETRO_DEVICE_ID_JOYPAD_L, RETRO_DEVICE_ID_JOYPAD_R};
    canvas = frame;
    pad = 0;
    if (input.x < -.35f) pad |= 1u << RETRO_DEVICE_ID_JOYPAD_LEFT;
    if (input.x >  .35f) pad |= 1u << RETRO_DEVICE_ID_JOYPAD_RIGHT;
    if (input.y < -.35f) pad |= 1u << RETRO_DEVICE_ID_JOYPAD_DOWN;
    if (input.y >  .35f) pad |= 1u << RETRO_DEVICE_ID_JOYPAD_UP;
    for (int i = 0; i < 6; ++i)
        if (input.held & (1u << i)) pad |= 1u << joypad[i];
    retro_run();
    if (last_canvas && last_canvas != canvas) {
        memcpy(canvas, last_canvas, GAME_WIDTH * GAME_HEIGHT * sizeof(*canvas));
        last_canvas = canvas;
    } else if (!last_canvas) {
        memset(canvas, 0, GAME_WIDTH * GAME_HEIGHT * sizeof(*canvas));
    }
}

void gba_log(void)
{
    ESP_LOGI(TAG, "video_frames=%u sampled_changes=%u hash=%08lx pad=%04x", frames, changes,
             (unsigned long)last_hash, pad);
}

/* C6 SETUP: put ESP-Hosted 2.12.9 on a T-Display-P4's ESP32-C6 from the P4,
   with no USB-to-UART adapter.

   The P4 reaches the C6's ROM downloader over the SDIO lines ESP-Hosted uses
   and resets it through the XL9535 (IO14 = C6_ESP_EN). The C6's BOOT pin is
   not wired to the P4, so the user holds the C6 BOOT button while the P4
   resets the C6 over and over until the ROM answers.

   It writes the four files of c6_firmware/ (the same images the adapter
   method writes), blanks the nvs and phy_init areas of that layout so nothing
   from the factory firmware is left in them, checks every area by reading it
   back, then lets the C6 boot. The C6's ROM cannot be overwritten, so a run
   that fails part way is fixed by running it again.

   Uses LakeShark's own partition table: this app sits in ota_0 at 0x10000 and
   installing LakeShark afterwards puts LakeShark back in the same place. */
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "driver/i2c_master.h"
#include "esp_loader.h"
#include "esp32_sdio_port.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"

#define SETUP_VERSION     "1.0"
#define HOSTED_VERSION    "2.12.9"

/* T-Display-P4: XL9535 on I2C port 1 (SDA 7, SCL 8) at 0x20. */
#define XL_ADDR           0x20
#define XL_SDA            7
#define XL_SCL            8
#define XL_OUT0           2              /* output port 0 register           */
#define XL_OUT1           3
#define XL_CFG0           6              /* direction, 1 = input             */
#define XL_CFG1           7
#define XL_IO0_3V3_EN     0x01u          /* port 0 bit 0, active low         */
#define XL_IO14_C6_EN     0x10u          /* port 1 bit 4                     */

/* ESP-Hosted's SDIO slot 1 pins on this board. */
#define SDIO_CLK          18
#define SDIO_CMD          19
#define SDIO_D0           14
#define SDIO_D1           15
#define SDIO_D2           16
#define SDIO_D3           17
#define SDIO_KHZ          4000           /* what the ROM test ran at         */

#define C6_FLASH_BYTES    (4u * 1024u * 1024u)   /* ESP32-C6FH4            */
#define BLOCK             1024u

extern const uint8_t boot_start[]  asm("_binary_bootloader_bin_start");
extern const uint8_t boot_end[]    asm("_binary_bootloader_bin_end");
extern const uint8_t table_start[] asm("_binary_partition_table_bin_start");
extern const uint8_t table_end[]   asm("_binary_partition_table_bin_end");
extern const uint8_t ota_start[]   asm("_binary_ota_data_initial_bin_start");
extern const uint8_t ota_end[]     asm("_binary_ota_data_initial_bin_end");
extern const uint8_t app_start[]   asm("_binary_network_adapter_bin_start");
extern const uint8_t app_end[]     asm("_binary_network_adapter_bin_end");

typedef struct {
    const char *name;
    uint32_t addr;
    const uint8_t *data;   /* NULL: write 0xFF (blank the area)           */
    uint32_t size;
    const char *sha256;    /* of data; NULL for a blank area               */
} area_t;

/* Hashes from c6_firmware/provenance.json. */
static area_t s_areas[] = {
    {"bootloader",      0x0000,  NULL, 0, "8a1259ac0885ace2aa8197d930f9b7e7f41e272310bab705922f8c90947084c1"},
    {"partition table", 0x8000,  NULL, 0, "dbbebb5ee220599f4b12fb5c43c01c38b59b2720b5df0d14f7d1b857f6524c92"},
    {"nvs (blank)",     0x9000,  NULL, 0x4000, NULL},
    {"otadata",         0xd000,  NULL, 0, "7d2c7ac4888bfd75cd5f56e8d61f69595121183afc81556c876732fd3782c62f"},
    {"phy_init (blank)",0xf000,  NULL, 0x1000, NULL},
    {"ESP-Hosted app",  0x10000, NULL, 0, "e27231545b8fe220c573321dfbfd10b79de88cd42e7f01169f0bca967c45197b"},
};
#define N_AREAS (sizeof(s_areas) / sizeof(s_areas[0]))

static i2c_master_dev_handle_t s_xl;
static esp_loader_port_ops_t s_ops;
static esp32_sdio_port_t s_port;
static esp_loader_t s_loader;
static uint8_t s_page[4096];
static uint8_t s_ff[BLOCK];
static esp_loader_error_t s_last_err;

/* One line for people, and a "C6SETUP <STATE>" token a web page can follow. */
#define SAY(state, ...) do { printf("C6SETUP %s | ", state); printf(__VA_ARGS__); printf("\n"); } while (0)

static esp_err_t xl_read(uint8_t reg, uint8_t *v) { return i2c_master_transmit_receive(s_xl, &reg, 1, v, 1, 100); }
static esp_err_t xl_write(uint8_t reg, uint8_t v) { const uint8_t b[] = {reg, v}; return i2c_master_transmit(s_xl, b, 2, 100); }

static esp_err_t xl_bits(uint8_t reg, uint8_t mask, bool set)
{
    uint8_t v;
    esp_err_t e = xl_read(reg, &v);
    if (e != ESP_OK) return e;
    return xl_write(reg, set ? (uint8_t)(v | mask) : (uint8_t)(v & ~mask));
}

/* EN low as an output holds the C6 in reset; released, EN is an input and the
   board's pull-up lets the C6 run (as LakeShark leaves it). */
static esp_err_t c6_hold(bool hold)
{
    if (hold) {
        esp_err_t e = xl_bits(XL_OUT1, XL_IO14_C6_EN, false);
        return e != ESP_OK ? e : xl_bits(XL_CFG1, XL_IO14_C6_EN, false);
    }
    esp_err_t e = xl_bits(XL_OUT1, XL_IO14_C6_EN, true);
    return e != ESP_OK ? e : xl_bits(XL_CFG1, XL_IO14_C6_EN, true);
}

static void c6_reset(esp_loader_port_t *unused)
{
    (void)unused;
    c6_hold(true);
    vTaskDelay(pdMS_TO_TICKS(150));
    c6_hold(false);
    vTaskDelay(pdMS_TO_TICKS(100));
}

static esp_loader_error_t port_init(esp_loader_port_t *base)
{
    esp32_sdio_port_t *p = (esp32_sdio_port_t *)base;   /* port is the first member */
    p->_card_config = (sdmmc_host_t)SDMMC_HOST_DEFAULT();
    p->_card_config.slot = SDMMC_HOST_SLOT_1;
    p->_card_config.max_freq_khz = SDIO_KHZ;
    p->_card_config.flags = SDMMC_HOST_FLAG_4BIT | SDMMC_HOST_FLAG_ALLOC_ALIGNED_BUF;
    if (sdmmc_host_init() != ESP_OK) return ESP_LOADER_ERROR_FAIL;
    p->_host_driver_needs_deinit = true;
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 4;
    slot.clk = SDIO_CLK; slot.cmd = SDIO_CMD;
    slot.d0 = SDIO_D0; slot.d1 = SDIO_D1; slot.d2 = SDIO_D2; slot.d3 = SDIO_D3;
    slot.flags = SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    if (sdmmc_host_init_slot(SDMMC_HOST_SLOT_1, &slot) == ESP_OK) return ESP_LOADER_SUCCESS;
    /* the next try builds a fresh port: undo the host init here, or every
       later sdmmc_host_init() fails as already initialised */
    sdmmc_host_deinit();
    p->_host_driver_needs_deinit = false;
    return ESP_LOADER_ERROR_FAIL;
}

static void port_deinit(esp_loader_port_t *base)
{
    esp32_sdio_port_t *p = (esp32_sdio_port_t *)base;
    free(p->_card.host.dma_aligned_buffer);
    p->_card.host.dma_aligned_buffer = NULL;
    if (p->_host_driver_needs_deinit) sdmmc_host_deinit();
    p->_host_driver_needs_deinit = false;
}

static void hex(const uint8_t d[32], char out[65])
{
    static const char h[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) { out[2 * i] = h[d[i] >> 4]; out[2 * i + 1] = h[d[i] & 15]; }
    out[64] = 0;
}

/* The images inside this app are the shipped ones, or nothing is written. */
static bool images_ok(void)
{
    s_areas[0].data = boot_start;  s_areas[0].size = (uint32_t)(boot_end - boot_start);
    s_areas[1].data = table_start; s_areas[1].size = (uint32_t)(table_end - table_start);
    s_areas[3].data = ota_start;   s_areas[3].size = (uint32_t)(ota_end - ota_start);
    s_areas[5].data = app_start;   s_areas[5].size = (uint32_t)(app_end - app_start);
    for (unsigned i = 0; i < N_AREAS; i++) {
        const area_t *a = &s_areas[i];
        if (!a->data) continue;
        uint8_t d[32]; char h[65];
        if (mbedtls_sha256(a->data, a->size, d, 0)) return false;
        hex(d, h);
        if (strcmp(h, a->sha256)) { SAY("FAIL", "the %s inside this setup image is damaged", a->name); return false; }
    }
    return true;
}

static bool try_connect(void)
{
    s_ops = esp32_sdio_ops;
    s_ops.init = port_init;
    s_ops.deinit = port_deinit;
    s_ops.enter_bootloader = c6_reset;
    s_ops.reset_target = c6_reset;
    s_port = (esp32_sdio_port_t){.port.ops = &s_ops};
    s_loader = (esp_loader_t){0};
    if (esp_loader_init_sdio(&s_loader, &s_port.port) != ESP_LOADER_SUCCESS) return false;
    esp_loader_connect_args_t args = {.sync_timeout = 1000, .trials = 3};
    s_last_err = esp_loader_connect(&s_loader, &args);
    if (s_last_err == ESP_LOADER_SUCCESS) {
        if (esp_loader_get_target(&s_loader) == ESP32C6_CHIP) return true;
        SAY("FAIL", "the co-processor answered but is not an ESP32-C6 - wrong board?");
    }
    esp_loader_deinit(&s_loader);
    return false;
}

/* Reads an area back and checks it is exactly what was written. */
static bool area_matches(const area_t *a)
{
    mbedtls_sha256_context sha;
    uint8_t d[32];
    bool blank = !a->data, ok = true;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);
    for (uint32_t off = 0; ok && off < a->size;) {
        const uint32_t n = a->size - off < sizeof(s_page) ? a->size - off : sizeof(s_page);
        if (esp_loader_flash_read(&s_loader, s_page, a->addr + off, n) != ESP_LOADER_SUCCESS) { ok = false; break; }
        if (blank) { for (uint32_t i = 0; i < n; i++) if (s_page[i] != 0xff) { ok = false; break; } }
        else mbedtls_sha256_update(&sha, s_page, n);
        off += n;
        vTaskDelay(1);
    }
    mbedtls_sha256_finish(&sha, d);
    mbedtls_sha256_free(&sha);
    if (ok && !blank) { char h[65]; hex(d, h); ok = !strcmp(h, a->sha256); }
    return ok;
}

static uint32_t s_done, s_total;
static int s_shown = -1;

static void progress(uint32_t add)
{
    s_done += add;
    const int pct = (int)((uint64_t)s_done * 100 / s_total);
    if (pct / 5 != s_shown / 5 || pct == 100) { s_shown = pct; SAY("PROGRESS", "%d%%", pct); }
}

static bool write_area(const area_t *a)
{
    esp_loader_flash_cfg_t cfg = {.offset = a->addr, .image_size = a->size, .block_size = BLOCK, .skip_verify = false};
    esp_loader_error_t e = esp_loader_flash_start(&s_loader, &cfg);
    for (uint32_t off = 0; e == ESP_LOADER_SUCCESS && off < a->size;) {
        const uint32_t n = a->size - off < BLOCK ? a->size - off : BLOCK;
        e = esp_loader_flash_write(&s_loader, &cfg, a->data ? a->data + off : s_ff, n);
        off += n;
        progress(n);
        vTaskDelay(1);
    }
    if (e == ESP_LOADER_SUCCESS) e = esp_loader_flash_finish(&s_loader, &cfg);   /* the C6 checks the MD5 */
    if (e != ESP_LOADER_SUCCESS) { SAY("FAIL", "writing the %s failed (error %d)", a->name, (int)e); return false; }
    if (!area_matches(a)) { SAY("FAIL", "the %s did not read back the same", a->name); return false; }
    printf("    %-17s 0x%05" PRIx32 "  %7" PRIu32 " bytes  written and checked\n", a->name, a->addr, a->size);
    return true;
}

static void finish(bool ok)
{
    if (ok) {
        SAY("DONE", "The C6 now runs ESP-Hosted " HOSTED_VERSION ". Now install LakeShark.");
    } else {
        SAY("FAILED", "Nothing is broken for good: the C6's ROM is untouched. Reset the board and run C6 SETUP again.");
    }
    for (;;) vTaskDelay(pdMS_TO_TICKS(60000));
}

void app_main(void)
{
    esp_log_level_set("*", ESP_LOG_WARN);
    memset(s_ff, 0xff, sizeof(s_ff));
    printf("\n");
    SAY("START", "C6 SETUP " SETUP_VERSION " for the LilyGO T-Display-P4: installs ESP-Hosted " HOSTED_VERSION " on the C6");
    if (!images_ok()) finish(false);

    const i2c_master_bus_config_t bus_cfg = {.i2c_port = I2C_NUM_0, .sda_io_num = XL_SDA, .scl_io_num = XL_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7, .flags.enable_internal_pullup = true};
    const i2c_device_config_t dev_cfg = {.dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = XL_ADDR,
        .scl_speed_hz = 100000};
    i2c_master_bus_handle_t bus;
    uint8_t probe;
    if (i2c_new_master_bus(&bus_cfg, &bus) != ESP_OK || i2c_master_bus_add_device(bus, &dev_cfg, &s_xl) != ESP_OK ||
        xl_read(XL_OUT0, &probe) != ESP_OK) {
        SAY("FAIL", "no XL9535 at 0x20: this is not a T-Display-P4");
        finish(false);
    }
    /* The vendor keeps IO0 (3.3 V rail, active low) driven low; do the same so
       the C6 is powered on a board that has never run LakeShark. */
    xl_bits(XL_OUT0, XL_IO0_3V3_EN, false);
    xl_bits(XL_CFG0, XL_IO0_3V3_EN, false);
    vTaskDelay(pdMS_TO_TICKS(10));

    SAY("HOLD_BOOT", "Press and HOLD the C6 BOOT button now. Keep holding it.");
    /* No time limit: it waits as long as the person needs to find the button. */
    bool found = false;
    for (int tries = 1; !found; tries++) {
        found = try_connect();
        if (!found && tries % 4 == 0)
            SAY("HOLD_BOOT", "Still waiting: hold the C6 BOOT button (the small one next to the C6). [try %d, %d]",
                tries, (int)s_last_err);
    }
    s_loader._target_flash_size = C6_FLASH_BYTES;   /* C6FH4; the ROM cannot autodetect over SDIO */
    SAY("RELEASE_BOOT", "Found the C6. Let go of BOOT now. Do not unplug the board.");
    vTaskDelay(pdMS_TO_TICKS(2000));

    s_total = 0;
    for (unsigned i = 0; i < N_AREAS; i++) s_total += s_areas[i].size;
    SAY("WRITING", "Writing %" PRIu32 " bytes to the C6...", s_total);
    const int64_t t0 = esp_timer_get_time();
    bool ok = true;
    for (unsigned i = 0; ok && i < N_AREAS; i++) ok = write_area(&s_areas[i]);
    esp_loader_deinit(&s_loader);
    c6_reset(NULL);                  /* let the C6 boot what was written */
    if (ok) printf("    took %d s\n", (int)((esp_timer_get_time() - t0) / 1000000));
    finish(ok);
}

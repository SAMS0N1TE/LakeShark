#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_check.h"
#include "esp_memory_utils.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "settings.h"
#include "lvgl.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "bsp_board_extra.h"

#include "shell/ls_shell.hpp"
/*LS-602*/
#include "shell/ls_hub.h"
/*LS-606*/
#include "sdr_ui/sdr_ui.h"
/*LS-990*/
#include "ui/ls_shade.h"
#include "screenshot.h"
#include "ls_wifi.h"
#include "rec_state.h"
/*LS-604*/
#include "home/AppHome.hpp"
#include "p25_gui/AppP25.hpp"
#include "fm_gui/AppFM.hpp"
#include "adsb_gui/AppADSB.hpp"
#include "acars_gui/AppACARS.hpp"
/*LS-020*/
#include "rec_gui/AppREC.hpp"
/*LS-737*/
#include "map_gui/AppMap.hpp"
/*LS-741*/
#include "media_gui/AppMedia.hpp"
#include "file_browser/FileBrowser.hpp"
#include "settings/AppSettings.hpp"

#include "lakeshark_backend.h"
#include "ls_board.h"
/*LS-017*/
#include "esp_hosted.h"
#include "display_ctl.h"
#include "ls_ctl.h"
/*LS-210*/
#include "ls_crash.h"
#include "ls_nvs_safe.h"
/*LS-994*/
#include "ls_safe_mode.h"
#include "ui/ls_safe_screen.h"
/*LS-019*/
#include "gui_link.h"
#include "boot_splash.h"
#include "bsod.h"

extern "C" esp_lcd_panel_handle_t bsp_get_dsi_panel(void);

static const char *TAG = "main";


/*LS-721*/
/* The BOOT button is no longer read. It duplicated a gesture that already
   exists - horizontal swipe on HOME cycles apps - and the button is worth
   more as hardware: it is now wired to the K (on/off) pin of the external
   boost module, because the housing has no room for a switch of its own.
   The pin is left COMPLETELY UNCONFIGURED on purpose. The old init enabled
   the P4 internal pull-up on GPIO35, which would fight the module's K line
   (measured idling at ~1.3 V) and could hold it where the module misreads
   it. Nothing here may drive, pull or poll GPIO35 again while it is wired
   to K. */

/*LS-017*/
static void c6_probe(void)
{
    int e = esp_hosted_connect_to_slave();
    if (e != 0) {
        /*LS-019*/
        ESP_LOGW(TAG, "C6 co-processor link FAILED (%d) - BLE will not start", e);
        /*LS-603*/
        ls_hub_set_c6(0);
        return;
    }

    esp_hosted_coprocessor_fwver_t v = {};
    if (esp_hosted_get_coprocessor_fwversion(&v) != 0) {
        ESP_LOGW(TAG, "C6 link up but the slave will not report a version - "
                      "that is the pre-2.5.2 factory image, it must be reflashed");
        /*LS-603*/
        ls_hub_set_c6(0);
        return;
    }

    /*LS-603*/
    ls_hub_set_c6(1);

    ESP_LOGI(TAG, "C6 esp_hosted: host %d.%d.%d, co-processor %lu.%lu.%lu",
             ESP_HOSTED_VERSION_MAJOR_1, ESP_HOSTED_VERSION_MINOR_1,
             ESP_HOSTED_VERSION_PATCH_1,
             (unsigned long)v.major1, (unsigned long)v.minor1,
             (unsigned long)v.patch1);

    if ((uint32_t)ESP_HOSTED_VERSION_MAJOR_1 != v.major1 ||
        (uint32_t)ESP_HOSTED_VERSION_MINOR_1 != v.minor1) {
        ESP_LOGE(TAG, "C6 MAJOR.MINOR MISMATCH - RPC will time out and BLE will "
                      "not start. Reflash the C6 from c6_firmware/ (LS-013).");
    }
}

/*LS-990  Pull-down shade actions. The shade UI lives under components/apps/
   and cannot see main/'s WiFi and screenshot symbols directly - a headless
   build does not compile them at all - so it accepts function pointers and
   the GUI path (this file) is what wires them up. Kept static: nothing else
   should be calling these. */
static bool shade_capture(char *path, size_t len)
{
    return screenshot_save("shade", path, len);
}

/*LS-815  Wi-Fi is on when EITHER radio role is up.

   This asked ls_wifi_running(), which reports the SoftAP alone, so the shade
   read WIFI OFF while the board was associated to the operator's network and
   the settings screen - which asks ls_wifi_status() - said it was on. Two
   screens disagreeing about the same radio is worse than either answer. */
static bool shade_wifi_running(void)
{
    return ls_wifi_running() || ls_wifi_sta_connected();
}

static void shade_wifi_toggle(void)
{
    /*LS-815  Only the AP is ours to toggle. The station is joined with
       `wifi join` and rejoins on its own at boot, so tearing it down from a
       shade tap - and taking the file server with it - is not what the tap
       means. With the station up the toggle stops the AP if one is running,
       and otherwise does nothing rather than raising a second radio role. */
    if (ls_wifi_running())             ls_wifi_stop();
    else if (!ls_wifi_sta_connected()) (void)ls_wifi_start();
}

static void shade_fetch_hint(char *out, size_t cap)
{
    if (!out || cap == 0) return;
    /*LS-815  The station address is the one the operator can reach from
       their own network; 192.168.4.1 exists only while the AP is up. Prefer
       the AP when both are running - joining "LakeShark" is the
       self-contained path. */
    if (ls_wifi_running()) {
        snprintf(out, cap, "LakeShark  http://192.168.4.1/");
        return;
    }
    char ip[16] = "";
    ls_wifi_sta_ip(ip, sizeof(ip));
    if (ip[0]) snprintf(out, cap, "http://%s/", ip);
    else       out[0] = '\0';
}

static void shade_home(void) { LsShell::instance().home(); }

/*LS-015*/
static void vbus_init(void)
{
#if LS_HAS_VBUS_CTRL
    gpio_config_t io = {};
    io.pin_bit_mask = 1ULL << LS_BOARD_VBUS_EN_GPIO;
    io.mode = GPIO_MODE_OUTPUT;
    io.pull_up_en = GPIO_PULLUP_DISABLE;
    io.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&io);
    gpio_set_level((gpio_num_t)LS_BOARD_VBUS_EN_GPIO, 1);
    ESP_LOGI(TAG, "USB host VBUS switch on GPIO%d - enabled",
             (int)LS_BOARD_VBUS_EN_GPIO);
#else
    ESP_LOGI(TAG, "no USB host VBUS switch on this board - port is hard-powered");
#endif
}

/*LS-727*/
/* Say which board profile is COMPILED IN, first thing, every boot.
   Nothing at runtime can detect the wrong profile - the pin maps differ but
   a misconfigured pin just misbehaves quietly - so the only defence is
   printing it where it cannot be missed. The failure this catches: an
   sdkconfig with no board line resolves to the NANO default,
   which puts VBUS control on GPIO46. That is fine on a NANO and wrong on
   the 4.3 LCD board, where LS-904 settled that there is no software VBUS
   control at all and lcd43_gui.defaults warns GPIO46 is the audio PA
   enable in Waveshare's own I2S example. Read this line before believing
   any USB-power or audio symptom. */
static void board_profile_announce(void)
{
    ESP_LOGW(TAG, "board profile: %s", LS_BOARD_NAME);
#if LS_HAS_VBUS_CTRL
    ESP_LOGW(TAG, "  VBUS control: GPIO%d", (int)LS_BOARD_VBUS_EN_GPIO);
#else
    ESP_LOGW(TAG, "  VBUS control: none (port hard-powered)");
#endif
#if !LS_BOARD_SELECTED
    ESP_LOGE(TAG, "  *** NO BOARD SELECTED IN sdkconfig - fell back to the NANO");
    ESP_LOGE(TAG, "  *** build with -DSDKCONFIG_DEFAULTS=\"sdkconfig.defaults;boards/<board>.defaults\"");
#endif
}

/*LS-994  Safe mode. Everything the normal path does that can fault is
   absent here by construction rather than by a flag: no C6 probe, no NVS
   (so nothing can auto-erase a partition), no SPIFFS/SD mount, no codec, no
   backend, no BLE, no boot chime, no app shell and no last-app restore. What
   is left is the panel, touch, and a console carrying four commands.

   The report and recovery console go to serial FIRST. The configured Waveshare
   BSP uses ESP_ERROR_CHECK internally, so several nominal error returns abort;
   its lower LCD calls may also block. The retained one-shot is armed before
   entering it, making the next safe boot serial-only after either reset path. */
static void safe_mode_main(const ls_safe_boot_t *boot,
                           const ls_safe_boot_plan_t *plan)
{
    /* Presence probe only. LS-683/LS-982: initialization must never enter the
       coredump parser, and safe mode is exactly where a stored dump is most
       likely to be the thing that faults it. */
    ls_crash_boot_setup();
    ls_safe_note_dump(ls_crash_present() ? LS_SAFE_DUMP_PRESENT
                                         : LS_SAFE_DUMP_NONE);

    static char report[LS_SAFE_REPORT_MAX];
    ls_safe_report(report, sizeof(report));
    ESP_LOGE(TAG, "SAFE MODE report follows");
    fputs(report, stdout);
    fflush(stdout);

    /* LS-707: create the independent, higher-priority CLI task before any
       optional panel call. It remains schedulable if a driver blocks, and an
       abort is caught on the next boot by the retained display latch below. */
    if (plan->console) ls_ctl_start_recovery_repl();

    bool painted = false;
    if (plan->display && ls_safe_display_attempt()) {
        bsp_display_cfg_t cfg = {
            .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
            .buffer_size = BSP_LCD_H_RES * BSP_LCD_V_RES,
            .double_buffer = BSP_LCD_DRAW_BUFF_DOUBLE,
            .flags = {
                .buff_dma = false,
                .buff_spiram = true,
                .sw_rotate = true,
            }};
        cfg.lvgl_port_cfg.task_affinity = 0;

        auto *disp = bsp_display_start_with_config(&cfg);
        if (!disp) {
            ESP_LOGE(TAG, "safe mode: the display did not start - serial "
                          "diagnostics only, no reset attempted");
        } else if (bsp_display_backlight_on() != ESP_OK) {
            ESP_LOGE(TAG, "safe mode: display backlight failed - serial "
                          "diagnostics only");
        } else if (!bsp_display_lock(LS_SAFE_DISPLAY_LOCK_MS)) {
            ESP_LOGE(TAG, "safe mode: LVGL lock timed out after %lu ms - "
                          "serial diagnostics only",
                     (unsigned long)LS_SAFE_DISPLAY_LOCK_MS);
        } else {
            ls_safe_screen_cfg_t sc = {};
            sc.title     = "SAFE MODE";
            sc.headline  = ls_safe_entry_name(boot->entry);
            sc.body      = report;
            sc.footer    = "Serial console: 'safemode', 'crash', 'version'. "
                           "NVS, SD and any stored coredump are untouched.";
            sc.primary   = "TRY NORMAL BOOT";
            sc.secondary = "STAY SAFE";
            sc.on_primary   = ls_safe_retry_normal_boot;
            sc.on_secondary = ls_safe_stay_safe;
            painted = ls_safe_screen_create(lv_scr_act(), &sc) != nullptr;
            bsp_display_unlock();
            if (!painted) {
                ESP_LOGE(TAG, "safe mode: recovery screen allocation failed - "
                              "serial diagnostics only");
            }
        }
        ls_safe_display_done(painted);
    } else if (plan->display) {
        ESP_LOGE(TAG, "safe mode: prior safe-display attempt failed or reset; "
                      "retained serial-only fallback active until an explicit "
                      "normal-boot retry");
    }

    ESP_LOGW(TAG, "safe mode ready (display %s). Nothing else was started.",
             painted ? "up" : "unavailable");
}

extern "C" void app_main(void)
{
    /*LS-994  First, before anything that can fault. */
    const ls_safe_boot_t *boot = ls_safe_boot_begin();
    ls_safe_boot_plan_t plan;
    ls_safe_boot_plan(boot->safe, &plan);

    ESP_LOGW(TAG, "boot: reset_reason=%d", (int)esp_reset_reason());
    /*LS-727*/
    board_profile_announce();

    if (boot->safe) {
        safe_mode_main(boot, &plan);
        return;
    }

    /*LS-015*/
    vbus_init();
    /*LS-017*/
    ls_safe_stage(LS_SAFE_STAGE_C6);
    if (plan.c6) c6_probe();

    ls_safe_stage(LS_SAFE_STAGE_NVS);
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        /*LS-994  Erasing a settings partition is the one irreversible thing
           this boot does. It stays a normal-boot behaviour; safe mode never
           reaches here, so a recovery session cannot destroy the state an
           operator might want read back. */
        if (plan.storage_autorepair) {
            ESP_ERROR_CHECK(nvs_flash_erase());
            err = nvs_flash_init();
        }
    }
    ESP_ERROR_CHECK(err);

    /* LS-686: prepare the statically reserved cache-safe NVS dispatcher before
       display, radio and BLE startup consume DMA-capable DRAM. */
    esp_err_t nvs_worker_err = ls_nvs_init();
    if (nvs_worker_err != ESP_OK)
        ESP_LOGE(TAG, "NVS dispatcher init failed: %s",
                 esp_err_to_name(nvs_worker_err));

    /*LS-210*/
    ls_crash_boot_setup();
    /*LS-994*/
    ls_safe_note_dump(ls_crash_present() ? LS_SAFE_DUMP_PRESENT
                                         : LS_SAFE_DUMP_NONE);

    ls_safe_stage(LS_SAFE_STAGE_STORAGE);
    ESP_ERROR_CHECK(bsp_spiffs_mount());
    ESP_LOGI(TAG, "SPIFFS mount successfully");

#if CONFIG_EXAMPLE_ENABLE_SD_CARD
    /*LS-016*/
    {
        esp_err_t sd = bsp_sdcard_mount();
        /*LS-603*/
        ls_hub_set_sd(sd == ESP_OK);
        if (sd == ESP_OK) {
            ESP_LOGI(TAG, "SD card mounted at %s", BSP_SD_MOUNT_POINT);
        } else {
            ESP_LOGW(TAG, "no SD card (%s) - running without it",
                     esp_err_to_name(sd));
        }
    }
#endif

    /* LS-735: rec_dir() used to make its one-time SD/SPIFFS choice when the
       first capture happened.  In the crash log that was much later, after
       RTL had consumed DMA memory: an SD metadata read returned ESP_ERR_NO_MEM
       and permanently redirected the boot to SPIFFS.  Resolve the mounted
       filesystem here, before radio startup, so `shot` neither retries SD in
       the low-DMA window nor mutates storage choice as a side effect. */
    (void)rec_dir();

    ls_safe_stage(LS_SAFE_STAGE_CODEC);
    ESP_ERROR_CHECK(bsp_extra_codec_init());

    ls_safe_stage(LS_SAFE_STAGE_DISPLAY);
    bsp_display_cfg_t cfg = {
        .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
        .buffer_size = BSP_LCD_H_RES * BSP_LCD_V_RES,
        .double_buffer = BSP_LCD_DRAW_BUFF_DOUBLE,
        .flags = {
            .buff_dma = false,
            .buff_spiram = true,
            .sw_rotate = true,
        }};

    cfg.lvgl_port_cfg.task_affinity = 0;
    bsp_display_start_with_config(&cfg);

    if (!screenshot_init())
        ESP_LOGE(TAG, "screenshot handoff init failed");

    bsod_init(bsp_get_dsi_panel());

    bsp_display_backlight_on();

    const char *recover_app = lakeshark_recovery_take_app();
    const bool  recovering  = (recover_app != NULL);

    if (!recovering) {
        bsp_display_lock(0);
        lakeshark_boot_splash_show();
        bsp_display_unlock();
        vTaskDelay(pdMS_TO_TICKS(2200));
    }

    bsp_display_lock(0);

    ls_safe_stage(LS_SAFE_STAGE_SHELL);
    LsShell &shell = LsShell::instance();
    shell.begin();

    /*LS-604*/
    shell.registerApp(new AppHome());
    shell.registerApp(new AppP25());
    shell.registerApp(new AppFM());
    shell.registerApp(new AppADSB());
    shell.registerApp(new AppACARS());
    /*LS-020*/
    shell.registerApp(new AppREC());
    /*LS-737*/
    shell.registerApp(new AppMap());
    shell.registerApp(new LsSettings());
    /*LS-741*/
    /* Hidden from the rail on request - the rail is tight and a music player
       is not a radio function. Reached from FILES, same as the browser. */
    shell.registerApp(new AppMedia(), false);
    shell.registerApp(new AppFileBrowser(), false);

    bsp_display_unlock();

    ls_safe_stage(LS_SAFE_STAGE_BACKEND);
    lakeshark_backend_start();
    display_ctl_init();
    ls_ctl_start_repl();

    if (!recovering) lakeshark_boot_sound();

    bsp_display_lock(0);
    /*LS-606*/
    sdr_theme_set((sdr_theme_t)settings_get_theme());
    /*LS-990  Wire the shade to the GUI-build symbols before the first app
       runs. Registering after start() is fine too, but doing it here means
       the shade is already usable the moment the first frame paints. */
    {
        ls_shade_hooks_t hooks = {};
        hooks.capture      = shade_capture;
        hooks.wifi_running = shade_wifi_running;
        hooks.wifi_toggle  = shade_wifi_toggle;
        hooks.fetch_hint   = shade_fetch_hint;
        hooks.go_home      = shade_home;
        ls_shade_configure(&hooks);
    }
    if (!recovering) lakeshark_boot_splash_hide();
    ls_safe_stage(LS_SAFE_STAGE_APPS);
    shell.start(recover_app);
    bsp_display_unlock();

    /*LS-019*/
    gui_link_start();

    /*LS-994  Reaching here is not yet "healthy" - most of what crashes this
       board crashes a few seconds into the first app, after app_main has
       returned. The counter is cleared by a one-shot timer at
       LS_SAFE_HEALTHY_MS, not here. */
    ls_safe_stage(LS_SAFE_STAGE_RUNNING);
    ls_safe_healthy_arm();
}

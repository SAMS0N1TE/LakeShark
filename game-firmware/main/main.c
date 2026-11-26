#include "display.h"
#include "flight.h"
#include "menu.h"
#include "tk21.h"
#ifdef LAKESHARK_GBA
#include "gba.h"
#endif
#include "driver/uart.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_timer.h"

static const char *TAG = "lakeshark_game";

static void close_game(menu_action app)
{
    (void)app;
#ifdef LAKESHARK_GBA
    if (app == MENU_GBA_DEMO || app == MENU_GBA_SD) gba_close();
#endif
}

static const char *open_game(menu_action app, unsigned brightness)
{
    switch (app) {
    case MENU_RELAY:
    case MENU_SALVAGE:
        flight_start(app == MENU_SALVAGE);
        return NULL;
#ifdef LAKESHARK_GBA
    case MENU_GBA_DEMO:
    case MENU_GBA_SD:
        if (gba_open(app == MENU_GBA_SD)) return NULL;
        return app == MENU_GBA_SD ? "Could not open game.gba on microSD." : "Could not start Signal Catch.";
#endif
    default:
        (void)brightness;
        return "Game unavailable.";
    }
}

static void update_game(menu_action app, uint16_t *pixels, const game_settings *settings,
                        tk21_state input, float dt)
{
    switch (app) {
    case MENU_RELAY:
    case MENU_SALVAGE:
        flight_update(settings_game_buttons(settings, input, false), dt);
        flight_draw(pixels);
        break;
#ifdef LAKESHARK_GBA
    case MENU_GBA_DEMO:
    case MENU_GBA_SD:
        gba_frame(pixels, settings_game_buttons(settings, input, true));
        break;
#endif
    default: break;
    }
}

static void log_status(menu_action app, const game_settings *settings, bool overlay)
{
    tk21_log();
    if (overlay) menu_log(settings);
    else if (app == MENU_RELAY || app == MENU_SALVAGE) flight_log();
#ifdef LAKESHARK_GBA
    else if (app == MENU_GBA_DEMO || app == MENU_GBA_SD) gba_log();
#endif
}

static void snapshot(const uint16_t *pixels)
{
    static const char header[] = "RGB565 400 240\n";
    uart_write_bytes(UART_NUM_0, header, sizeof(header) - 1);
    uart_write_bytes(UART_NUM_0, pixels, GAME_WIDTH * GAME_HEIGHT * sizeof(*pixels));
}

void app_main(void)
{
    const tk21_pinout controller = {
        .x_gpio = 21, .y_gpio = 22, .buttons_gpio = 49, .key_gpio = 5,
    };
    ESP_ERROR_CHECK(tk21_init(&controller));
    game_settings settings;
    settings_load(&settings);
    ESP_ERROR_CHECK(game_display_init());
    ESP_ERROR_CHECK(game_display_brightness(settings.brightness));
    unsigned brightness = settings.brightness;
    ESP_ERROR_CHECK(flight_init());
    ESP_ERROR_CHECK(uart_driver_install(UART_NUM_0, 256, 0, 0, NULL, 0));
    menu_home();
    ESP_LOGI(TAG, "LakeShark Game landscape; ? prints status");

    int64_t previous = esp_timer_get_time(), epoch = previous, serial_until = 0;
    float serial_x = 0, serial_y = 0;
    uint32_t frames = 0, render_sum = 0, render_max = 0, display_sum = 0;
    menu_action running = MENU_NONE;
    bool overlay = true;
    for (;;) {
        uint16_t *pixels = game_display_buffer();
        int64_t start = esp_timer_get_time();
        float dt = (start - previous) / 1000000.0f;
        previous = start;
        if (dt > .05f) dt = .05f;

        tk21_state physical = tk21_poll();
        tk21_raw_state raw = tk21_read_raw();
        bool capture = false;
        menu_action action = MENU_NONE;
        menu_direction nav = NAV_NONE;
        uint8_t command;
        while (uart_read_bytes(UART_NUM_0, &command, 1, 0) == 1) {
            switch (command) {
            case '?': log_status(running, &settings, overlay); break;
            case 'g': case '\r': physical.pressed |= overlay ? PAD_A : PAD_MENU; break;
            case 'q': physical.pressed |= overlay ? PAD_B : PAD_MENU; break;
            case 'p': physical.pressed |= PAD_MENU; break;
            case 'k': physical.pressed |= TK21_KEY; break;
            case 'm': action = MENU_HOME; break;
            case '1': action = MENU_RELAY; break;
            case '2': action = MENU_SALVAGE; break;
            case 'b': action = MENU_GBA_DEMO; break;
            case 'r': action = MENU_GBA_SD; break;
            case 'W': nav = NAV_UP; break;
            case 'S': nav = NAV_DOWN; break;
            case 'A': nav = NAV_LEFT; break;
            case 'D': nav = NAV_RIGHT; break;
            case '4': case '5': case '6': case '7': case '8': case '9':
                physical.pressed |= 1u << (command - '4');
                break;
            case '!': capture = true; break;
            case 'a': case 'd': case 'w': case 's':
                serial_x = (command == 'd') - (command == 'a');
                serial_y = (command == 'w') - (command == 's');
                serial_until = start + 400000;
                break;
            }
        }
        tk21_state input = settings_input(&settings, physical, raw);
        if (start < serial_until) {
            input.x = serial_x;
            input.y = serial_y;
        }

        if (action == MENU_NONE && overlay) {
            action = menu_update(&settings, input, raw, nav, start);
        } else if (action == MENU_NONE && (physical.pressed & PAD_MENU)) {
#ifdef LAKESHARK_GBA
            if (running == MENU_GBA_DEMO || running == MENU_GBA_SD) gba_discard_frame();
#endif
            overlay = true;
            menu_pause(running);
        }
        if (action == MENU_RESUME) {
            overlay = false;
        } else if (action == MENU_HOME) {
            close_game(running);
            running = MENU_NONE;
            overlay = true;
            menu_home();
        } else if (action != MENU_NONE) {
            menu_action requested = action == MENU_RESTART ? running : action;
            close_game(running);
            const char *error = open_game(requested, brightness);
            running = error ? MENU_NONE : requested;
            overlay = error != NULL;
            if (error) {
                menu_home();
                menu_notice(error);
            }
            start = esp_timer_get_time();
            dt = 0;
            frames = render_sum = render_max = display_sum = 0;
            epoch = start;
        }
        if (settings.brightness != brightness) {
            ESP_ERROR_CHECK(game_display_brightness(settings.brightness));
            brightness = settings.brightness;
        }
        if (overlay) menu_draw(pixels, &settings, raw, physical, start);
        else update_game(running, pixels, &settings, input, dt);

        int64_t drawn = esp_timer_get_time();
        ESP_ERROR_CHECK(game_display_present());
        int64_t end = esp_timer_get_time();
        if (capture) {
            snapshot(pixels);
            frames = render_sum = render_max = display_sum = 0;
            epoch = esp_timer_get_time();
            previous = epoch;
            continue;
        }

        uint32_t render = drawn - start;
        render_sum += render;
        if (render > render_max) render_max = render;
        display_sum += end - drawn;
        ++frames;
        if (end - epoch >= 5000000) {
            ESP_LOGI(TAG, "fps=%.1f render_avg=%luus render_max=%luus queue_avg=%luus ppa=%uus heap_int=%u heap_psram=%u",
                     1000000.0 * frames / (end - epoch), (unsigned long)(render_sum / frames),
                     (unsigned long)render_max, (unsigned long)(display_sum / frames),
                     game_display_scale_us(), heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
            frames = render_sum = render_max = display_sum = 0;
            epoch = end;
        }
    }
}

/* Real retained trail, including full native ids, clear-after-return and reset classes. */
#include "../../components/lakeshark/core/ls_trail.c"
#include <assert.h>
static esp_reset_reason_t reset;
esp_reset_reason_t esp_reset_reason(void) { return reset; }
uint32_t esp_log_timestamp(void) { return 100; }
static void boot(esp_reset_reason_t why) {
    memset(&s_prev, 0, sizeof(s_prev));
    s_prev_reset = -1;
    reset = why;
    ls_trail_boot();
}
int main(void) {
    const char *id = "abcdefghijklmnopqrstuvwxyz01234";
    boot(ESP_RST_POWERON);
    ls_trail_native_callback(id);
    boot(ESP_RST_PANIC);
    assert(ls_trail_last_native() && !strcmp(ls_trail_last_native(), id));
    ls_trail_native_callback(id);
    ls_trail_native_callback(NULL);
    boot(ESP_RST_TASK_WDT);
    assert(!ls_trail_last_native());
    esp_reset_reason_t faults[] = {ESP_RST_PANIC, ESP_RST_WDT, ESP_RST_INT_WDT, ESP_RST_TASK_WDT,
                                   ESP_RST_CPU_LOCKUP};
    for (unsigned i = 0; i < 5; i++) {
        ls_trail_native_callback(id);
        boot(faults[i]);
        assert(ls_trail_last_native() && !strcmp(ls_trail_last_native(), id));
    }
    esp_reset_reason_t clean[] = {ESP_RST_POWERON, ESP_RST_SW, ESP_RST_BROWNOUT, ESP_RST_DEEPSLEEP,
                                  ESP_RST_EXT};
    for (unsigned i = 0; i < 5; i++) {
        ls_trail_native_callback(id);
        boot(clean[i]);
        assert(!ls_trail_last_native());
    }
    s_live.magic = 0x5452414c;
    boot(ESP_RST_PANIC);
    assert(!ls_trail_last_native());
    puts("test_lap_crumb: 13 cases passed (31-character id, callback return, reset classes, old "
         "record)");
    return 0;
}

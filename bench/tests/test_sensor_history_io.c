#include "ls_test.h"
#include "experiments/lr433_history.h"
#include <stdlib.h>
#include <time.h>

void lr433_history_test_reset(void);
void ls_shim_time_advance(int64_t us);
bool ls_exp_hw_wake(void) { return false; }
static sh_sensor_t D;

static void clean(void)
{
    char p[sizeof(LS_SENSOR_ROOT) + 48];
    for (int i = 0; i < SH_SENSORS; i++) for (int j = 0; j < 3; j++) {
        snprintf(p, sizeof(p), LS_SENSOR_ROOT "/sensor-%02d.bin%s", i, j == 0 ? "" : j == 1 ? ".tmp" : ".bak"); remove(p);
    }
    remove(LS_SENSOR_ROOT "/options.bin"); remove(LS_SENSOR_ROOT "/options.bin.tmp"); remove(LS_SENSOR_ROOT "/options.bin.bak");
    const long long day = time(NULL) / SH_DAY;
    for (int i = -31; i <= 1; i++) { snprintf(p, sizeof(p), LS_SENSOR_ROOT "/day-%lld.csv", day + i); remove(p); }
    lr433_history_test_reset();
}
static void drain(void)
{
    for (int i = 0; i < 80; i++) { ls_shim_time_advance(200000); if (!lr433_history_service()) break; }
}

LS_CASE(sd_snapshot_replacement_restores_names_options_and_rings)
{
    clean(); lr433_history_start();
    lr433_msg_t m = { .proto = LR433_P_ACURITE_TOWER, .channel = 1, .battery_ok = 0,
        .temp_c = -18, .humidity = 70, .kpa = NAN, .rain_mm = NAN, .wind_ms = NAN };
    strcpy(m.id, "001"); lr433_history_receive(&m);
    uint8_t order[SH_SENSORS]; LS_EQ_INT(lr433_history_list(order), 1);
    lr433_history_options(order[0]); lr433_history_name("Cold, \"box\""); drain();
    m.temp_c = -17; lr433_history_receive(&m); drain();
    const ls_opt_ctx_t *ctx = lr433_history_options(-1);
    ctx->opt[0].set(&ctx->opt[0], 0);
    ctx->opt[1].set_num(&ctx->opt[1], 3);
    ctx->opt[2].set(&ctx->opt[2], 1); drain();
    LS_EQ_STR(lr433_history_status(), "History: SD saved");
    lr433_history_test_reset(); lr433_history_start();
    LS_EQ_INT(lr433_history_list(order), 1);
    LS_CHECK(lr433_history_copy(order[0], &D));
    LS_EQ_STR(D.name, "Cold, \"box\""); LS_EQ_INT(D.count, 2); LS_NEAR(D.last.temp_c, -17, .01);
    LS_CHECK(lr433_history_fahrenheit());
    ctx = lr433_history_options(-1); LS_NEAR(ctx->opt[1].num(&ctx->opt[1]), 3, .01);
    char p[sizeof(LS_SENSOR_ROOT) + 48];
    snprintf(p, sizeof(p), LS_SENSOR_ROOT "/day-%lld.csv", (long long)(time(NULL) / SH_DAY));
    FILE *f = fopen(p, "rb"); LS_CHECK(f != NULL);
    if (f) { char text[2048] = {0}; fread(text, 1, sizeof(text) - 1, f); fclose(f);
        LS_CHECK(strstr(text, "\"Cold, \"\"box\"\"\"") != NULL); LS_CHECK(strstr(text, "utc_seconds") != NULL); }
    clean();
}

LS_CASE(sd_retention_and_tpms_identity_only_snapshot)
{
    clean(); lr433_history_start();
    lr433_msg_t m = { .proto = LR433_P_FORD, .channel = -1, .battery_ok = 1,
        .temp_c = 30, .humidity = NAN, .kpa = 250, .rain_mm = NAN, .wind_ms = NAN };
    strcpy(m.id, "wheel"); lr433_history_receive(&m); drain();
    lr433_history_test_reset(); lr433_history_start();
    uint8_t order[SH_SENSORS]; LS_EQ_INT(lr433_history_list(order), 1);
    LS_CHECK(lr433_history_copy(order[0], &D)); LS_EQ_INT(D.count, 0); LS_CHECK(isnan(D.last.kpa));
    char old[sizeof(LS_SENSOR_ROOT) + 48];
    snprintf(old, sizeof(old), LS_SENSOR_ROOT "/day-%lld.csv", (long long)(time(NULL) / SH_DAY - 9));
    FILE *f = fopen(old, "wb"); LS_CHECK(f != NULL); if (f) fclose(f);
    const ls_opt_ctx_t *ctx = lr433_history_options(-1); ctx->opt[1].set_num(&ctx->opt[1], 1); drain();
    f = fopen(old, "rb"); LS_CHECK(f == NULL); if (f) fclose(f);
    clean();
}

/* LS_TEST_SOURCES: ${FW}/components/lakeshark/core/p25_demod_pref_cache.c */
/* LS_TEST_INCLUDE: ${FW}/components/lakeshark/core */

#include "ls_test.h"
#include "p25_demod_pref_cache.h"

#include <stdbool.h>
#include <stdint.h>

static int     s_flash_reads;
static bool    s_flash_found;
static uint8_t s_flash_value;

static void boot_load_cache(void)
{
    s_flash_reads++;
    p25_demod_pref_cache_init(s_flash_found, s_flash_value);
}

LS_CASE(receive_startup_reads_only_the_boot_cache)
{
    s_flash_reads = 0;
    s_flash_found = true;
    s_flash_value = 2;
    boot_load_cache();
    LS_EQ_INT(s_flash_reads, 1);

    for (int restart = 0; restart < 20; ++restart)
        LS_EQ_INT(p25_demod_pref_cache_get(), 2);

    LS_EQ_INT(s_flash_reads, 1);
}

LS_CASE(storage_schema_defaults_absent_auto_and_rejects_malformed)
{
    p25_demod_pref_cache_init(false, 0);
    LS_EQ_INT(p25_demod_pref_cache_get(), -1);

    p25_demod_pref_cache_init(true, 4);
    LS_EQ_INT(p25_demod_pref_cache_get(), -1);

    p25_demod_pref_cache_init(true, 255);
    LS_EQ_INT(p25_demod_pref_cache_get(), -1);

    for (uint8_t manual = 0; manual <= 3; ++manual) {
        p25_demod_pref_cache_init(true, manual);
        LS_EQ_INT(p25_demod_pref_cache_get(), manual);
    }
}

LS_CASE(manual_and_auto_updates_survive_receive_restart)
{
    p25_demod_pref_cache_init(false, 0);

    for (int manual = 0; manual <= 3; ++manual) {
        uint8_t stored = 99;
        LS_CHECK(p25_demod_pref_cache_update(manual, &stored));
        LS_EQ_INT(stored, manual);
        LS_EQ_INT(p25_demod_pref_cache_get(), manual);

        /* A receive-task restart is another get, not another flash load. */
        LS_EQ_INT(p25_demod_pref_cache_get(), manual);
    }

    uint8_t stored = 99;
    LS_CHECK(p25_demod_pref_cache_update(-1, &stored));
    LS_EQ_INT(stored, 4);
    LS_EQ_INT(p25_demod_pref_cache_get(), -1);
}

LS_CASE(invalid_updates_do_not_change_cached_preference)
{
    uint8_t stored = 0;
    p25_demod_pref_cache_init(true, 1);

    LS_CHECK(!p25_demod_pref_cache_update(-2, &stored));
    LS_EQ_INT(p25_demod_pref_cache_get(), 1);
    LS_CHECK(!p25_demod_pref_cache_update(4, &stored));
    LS_EQ_INT(p25_demod_pref_cache_get(), 1);
    LS_CHECK(!p25_demod_pref_cache_update(2, NULL));
    LS_EQ_INT(p25_demod_pref_cache_get(), 1);
}

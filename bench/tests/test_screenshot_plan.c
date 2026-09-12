/* LS_TEST_SOURCES: ${FW}/main/screenshot_plan.c */
/* the screenshot command must decide that a full filesystem or an
   unavailable RGB565 frame buffer cannot work before entering LVGL's deeply
   nested font renderer. */

#include "ls_test.h"
#include "screenshot_plan.h"

#include <stdint.h>
#include <string.h>

LS_CASE(normal_480_by_800_frame_has_exact_resource_sizes)
{
    screenshot_plan_t plan = {0};
    LS_EQ_INT(screenshot_plan_make(480, 800,
                                   2ull * 1024ull * 1024ull,
                                   2u * 1024u * 1024u,
                                   2u * 1024u * 1024u, &plan),
              SCREENSHOT_OK);
    LS_EQ_UINT(plan.snapshot_bytes, 480u * 800u * 2u);
    LS_EQ_UINT(plan.bmp_bytes, 54u + 1440u * 800u);
}

LS_CASE(unknown_storage_is_a_failure_not_permission_to_try)
{
    screenshot_plan_t plan;
    LS_EQ_INT(screenshot_plan_make(480, 800, UINT64_MAX,
                                   2u * 1024u * 1024u,
                                   2u * 1024u * 1024u, &plan),
              SCREENSHOT_ERR_STORAGE_UNKNOWN);
}

LS_CASE(short_storage_is_refused_at_the_exact_bmp_boundary)
{
    screenshot_plan_t plan;
    const uint64_t need = 54u + 1440u * 800u;
    LS_EQ_INT(screenshot_plan_make(480, 800, need - 1,
                                   2u * 1024u * 1024u,
                                   2u * 1024u * 1024u, &plan),
              SCREENSHOT_ERR_STORAGE_SHORT);
    LS_EQ_INT(screenshot_plan_make(480, 800, need,
                                   2u * 1024u * 1024u,
                                   2u * 1024u * 1024u, &plan),
              SCREENSHOT_OK);
}

LS_CASE(fragmented_psram_is_refused_even_when_total_is_large)
{
    screenshot_plan_t plan;
    const size_t frame = 480u * 800u * 2u;
    LS_EQ_INT(screenshot_plan_make(480, 800, 2ull * 1024ull * 1024ull,
                                   frame + SCREENSHOT_PSRAM_RESERVE_BYTES,
                                   frame - 1, &plan),
              SCREENSHOT_ERR_PSRAM_SHORT);
}

LS_CASE(psram_reserve_is_kept_for_lvg_draw_context)
{
    screenshot_plan_t plan;
    const size_t frame = 480u * 800u * 2u;
    LS_EQ_INT(screenshot_plan_make(480, 800, 2ull * 1024ull * 1024ull,
                                   frame + SCREENSHOT_PSRAM_RESERVE_BYTES - 1,
                                   frame, &plan),
              SCREENSHOT_ERR_PSRAM_SHORT);
}

LS_CASE(invalid_dimensions_do_not_overflow_size_arithmetic)
{
    screenshot_plan_t plan;
    LS_EQ_INT(screenshot_plan_make(0, 800, UINT64_MAX, 0, 0, &plan),
              SCREENSHOT_ERR_DIMENSIONS);
    LS_EQ_INT(screenshot_plan_make(480, 800, UINT64_MAX, 0, 0, NULL),
              SCREENSHOT_ERR_DIMENSIONS);
    LS_EQ_INT(screenshot_plan_make(UINT32_MAX, UINT32_MAX, UINT64_MAX,
                                   SIZE_MAX, SIZE_MAX, &plan),
              SCREENSHOT_ERR_DIMENSIONS);
}

LS_CASE(resource_failures_have_operator_facing_reasons)
{
    LS_CHECK(strstr(screenshot_result_message(SCREENSHOT_ERR_STORAGE_SHORT),
                    "storage") != NULL);
    LS_CHECK(strstr(screenshot_result_message(SCREENSHOT_ERR_PSRAM_SHORT),
                    "RAM") != NULL);
    LS_CHECK(strstr(screenshot_result_message(SCREENSHOT_ERR_RENDER),
                    "render") != NULL);
}

/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef LS_IMU_STATUS_POLICY_H
#define LS_IMU_STATUS_POLICY_H
#include <stdbool.h>
#include <stdint.h>

/* UI health only: capture measurements retain their own validity checks.
 * A busy reader is not a missing device. Bound both polling and retention. */
typedef struct {
    int64_t poll_us, good_us;
    bool polled, good, mag_valid;
} ls_imu_status_policy_t;

static inline bool ls_imu_status_poll_due(ls_imu_status_policy_t *s, int64_t now)
{
    if (s->polled && now >= s->poll_us && now - s->poll_us < 250000)
        return false;
    s->poll_us = now;
    s->polled = true;
    return true;
}

static inline void ls_imu_status_record(ls_imu_status_policy_t *s, int64_t now,
                                        bool ok, bool mag_valid)
{
    if (!ok) return;
    s->good_us = now;
    s->good = true;
    s->mag_valid = mag_valid;
}

static inline bool ls_imu_status_recent(const ls_imu_status_policy_t *s, int64_t now)
{
    return s->good && now >= s->good_us && now - s->good_us < 750000;
}
#endif

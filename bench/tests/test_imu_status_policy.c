/* LS_TEST_INCLUDE: ${FW}/components/lakeshark/board */
#include "ls_test.h"
#include "ls_imu_status_policy.h"

LS_CASE(never_ready_before_a_successful_sample)
{
    ls_imu_status_policy_t s={0};
    ls_imu_status_record(&s,1000,false,true);
    LS_CHECK(!ls_imu_status_recent(&s,1000));
    LS_CHECK(!s.mag_valid);
}
LS_CASE(transient_contention_does_not_erase_recent_health)
{
    ls_imu_status_policy_t s={0};
    ls_imu_status_record(&s,1000,true,true);
    ls_imu_status_record(&s,251000,false,false);
    LS_CHECK(ls_imu_status_recent(&s,251000));
    LS_CHECK(s.mag_valid);
    LS_CHECK(ls_imu_status_recent(&s,750999));
    LS_CHECK(!ls_imu_status_recent(&s,751000));
}
LS_CASE(sustained_failure_expires_and_success_recovers)
{
    ls_imu_status_policy_t s={0};
    ls_imu_status_record(&s,1,true,true);
    for(int64_t now=250001;now<=1000001;now+=250000)
        ls_imu_status_record(&s,now,false,true);
    LS_CHECK(!ls_imu_status_recent(&s,1000001));
    ls_imu_status_record(&s,1000002,true,false);
    LS_CHECK(ls_imu_status_recent(&s,1000002));
    LS_CHECK(!s.mag_valid);
}
LS_CASE(polling_is_bounded_and_clock_reset_does_not_keep_stale_health)
{
    ls_imu_status_policy_t s={0};
    LS_CHECK(ls_imu_status_poll_due(&s,0));
    LS_CHECK(!ls_imu_status_poll_due(&s,249999));
    LS_CHECK(ls_imu_status_poll_due(&s,250000));
    ls_imu_status_record(&s,250000,true,true);
    LS_CHECK(!ls_imu_status_recent(&s,249999));
    LS_CHECK(ls_imu_status_poll_due(&s,0));
}

/* LS_TEST_INCLUDE: ${FW}/components/lakeshark/board */
#include "ls_test.h"
#include "ls_imu_pose_policy.h"

LS_CASE(diagonal_grip_does_not_flip_axes)
{
    ls_imu_sample_t s = {.ax=.70f,.ay=.71f};
    LS_CHECK(ls_imu_classify_pose(&s) == LS_IMU_FLAT);
    s.ay=.69f;
    LS_CHECK(ls_imu_classify_pose(&s) == LS_IMU_FLAT);
}
LS_CASE(deliberate_cardinal_holds_are_recognized)
{
    ls_imu_sample_t s = {.ay=1};
    LS_CHECK(ls_imu_classify_pose(&s) == LS_IMU_UP);
    s.ay=-1; LS_CHECK(ls_imu_classify_pose(&s) == LS_IMU_DOWN);
    s.ay=0; s.ax=1; LS_CHECK(ls_imu_classify_pose(&s) == LS_IMU_RIGHT);
    s.ax=-1; LS_CHECK(ls_imu_classify_pose(&s) == LS_IMU_LEFT);
}
LS_CASE(flat_invalid_and_moving_samples_do_not_rotate)
{
    ls_imu_sample_t s = {.az=1};
    LS_CHECK(ls_imu_classify_pose(&s) == LS_IMU_FLAT);
    s.az=0; s.ax=2; LS_CHECK(ls_imu_classify_pose(&s) == LS_IMU_FLAT);
    s.ax=NAN; LS_CHECK(ls_imu_classify_pose(&s) == LS_IMU_FLAT);
    s.ax=1; s.gy=90; LS_CHECK(ls_imu_classify_pose(&s) == LS_IMU_FLAT);
}

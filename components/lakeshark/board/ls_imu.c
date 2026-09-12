/* See ls_imu.h. Register map from the InvenSense ICM-20948 datasheet
   (DS-000189 v1.3) and the AK09916 datasheet, not from memory. */
#include "ls_imu.h"

#include <math.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "ls_i2c.h"

static const char *TAG = "ls_imu";

#if LS_HAS_IMU

/* THE ICM20948 HAS BANKED REGISTERS, and that is the one thing about
   this part that catches people out. Register 0x7F is the bank select and it
   is the SAME register in all four banks - everything else means a different
   thing depending on which bank is current. A driver that forgets a bank
   switch does not fail, it reads a plausible number from the wrong place. */
#define REG_BANK_SEL     0x7F

/* Bank 0 */
#define B0_WHO_AM_I      0x00
#define B0_USER_CTRL     0x03
#define B0_PWR_MGMT_1    0x06
#define B0_PWR_MGMT_2    0x07
#define B0_INT_PIN_CFG   0x0F
#define B0_ACCEL_XOUT_H  0x2D    /* 6 accel, 6 gyro, 2 temp, contiguous */

/* Bank 2 */
#define B2_GYRO_SMPLRT   0x00
#define B2_GYRO_CONFIG_1 0x01
#define B2_ACCEL_CFG     0x14

#define WHO_AM_I_ICM20948 0xEA

/* PWR_MGMT_1 */
#define PWR1_DEVICE_RESET 0x80
#define PWR1_SLEEP        0x40
#define PWR1_CLK_AUTO     0x01

/* USER_CTRL */
#define USER_I2C_MST_EN   0x20

/* INT_PIN_CFG */
#define INTPIN_BYPASS_EN  0x02

/* The magnetometer, once bypass has exposed it.

   AK09916 in the ICM's package. Its address is fixed at 0x0C and it is only
   reachable while the ICM's own I2C master is off and BYPASS_EN is set. */
#define MAG_ADDR         0x0C
#define MAG_WIA2         0x01    /* device id, reads 0x09 */
#define MAG_ST1          0x10
#define MAG_HXL          0x11
#define MAG_ST2          0x18
#define MAG_CNTL2        0x31
#define MAG_CNTL3        0x32

#define MAG_WIA2_AK09916 0x09
#define MAG_MODE_100HZ   0x08
#define MAG_SRST         0x01

/* Full-scale defaults after reset: accel +-2 g, gyro +-250 dps. Both are the
   right choice here - orientation and a slow heading, not a crash recorder -
   so the sensitivity constants below are the datasheet's for those ranges. */
#define ACCEL_LSB_PER_G   16384.0f
#define GYRO_LSB_PER_DPS    131.0f
/* The AK09916 is fixed at 0.15 uT per count. */
#define MAG_UT_PER_LSB       0.15f

/* The bus is shared with the touch controller, which is polled at 10 ms and
   matters more. Fifty milliseconds is twenty reads a second, which is far
   more than a screen rotation or a compass needle needs. */
#define CACHE_US  50000

static i2c_master_dev_handle_t s_dev;
static i2c_master_dev_handle_t s_mag;
static bool s_present;
static bool s_mag_present;
static uint8_t s_bank = 0xFF;          /* unknown until the first select */

static ls_imu_sample_t s_cache;
static int64_t         s_cache_us;

/* ------------------------------------------------------------------- bus -- */

static bool wr(uint8_t reg, uint8_t val)
{
    if (!s_dev) return false;
    const uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(s_dev, buf, sizeof(buf), 100) == ESP_OK;
}

static bool rd(uint8_t reg, uint8_t *out, size_t n)
{
    if (!s_dev) return false;
    return i2c_master_transmit_receive(s_dev, &reg, 1, out, n, 100) == ESP_OK;
}

static bool bank(uint8_t n)
{
    if (s_bank == n) return true;
    /* The bank number sits in bits 5:4, not in the low bits. Writing the
       plain number selects bank 0 every time and every read after it comes
       from the wrong place while looking entirely reasonable. */
    if (!wr(REG_BANK_SEL, (uint8_t)(n << 4))) return false;
    s_bank = n;
    return true;
}

static bool mag_wr(uint8_t reg, uint8_t val)
{
    if (!s_mag) return false;
    const uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(s_mag, buf, sizeof(buf), 100) == ESP_OK;
}

static bool mag_rd(uint8_t reg, uint8_t *out, size_t n)
{
    if (!s_mag) return false;
    return i2c_master_transmit_receive(s_mag, &reg, 1, out, n, 100) == ESP_OK;
}

/* ------------------------------------------------------------------ init -- */

static void mag_start(void)
{
    /* Bypass first, or 0x0C is not on this bus at all.

       The ICM's own I2C master has to be off before the aux bus can be
       shorted onto the main one, and both bits live in bank 0. Doing this in
       the wrong order leaves the magnetometer invisible and looks exactly
       like a part that is not fitted. */
    if (!bank(0)) return;
    uint8_t uc = 0;
    if (rd(B0_USER_CTRL, &uc, 1))
        wr(B0_USER_CTRL, (uint8_t)(uc & ~USER_I2C_MST_EN));
    wr(B0_INT_PIN_CFG, INTPIN_BYPASS_EN);
    vTaskDelay(pdMS_TO_TICKS(10));

    if (ls_i2c_device(LS_I2C_SECONDARY, MAG_ADDR, 100 * 1000, &s_mag) != ESP_OK)
        return;

    uint8_t id = 0;
    if (!mag_rd(MAG_WIA2, &id, 1) || id != MAG_WIA2_AK09916) {
        ESP_LOGW(TAG, "no AK09916 behind the IMU (id 0x%02X) - accelerometer "
                      "and gyroscope only", id);
        i2c_master_bus_rm_device(s_mag);
        s_mag = NULL;
        return;
    }

    mag_wr(MAG_CNTL3, MAG_SRST);
    vTaskDelay(pdMS_TO_TICKS(10));
    mag_wr(MAG_CNTL2, MAG_MODE_100HZ);
    s_mag_present = true;
    ESP_LOGI(TAG, "AK09916 magnetometer up at 0x%02X", MAG_ADDR);
}

esp_err_t ls_imu_start(void)
{
    if (s_present) return ESP_OK;

    esp_err_t err = ls_i2c_device(LS_I2C_SECONDARY, LS_BOARD_IMU_I2C_ADDR,
                                  100 * 1000, &s_dev);
    if (err != ESP_OK) return err;

    s_bank = 0xFF;
    if (!bank(0)) return ESP_ERR_INVALID_STATE;

    uint8_t who = 0;
    if (!rd(B0_WHO_AM_I, &who, 1)) {
        ESP_LOGW(TAG, "no ACK at 0x%02X", LS_BOARD_IMU_I2C_ADDR);
        return ESP_ERR_NOT_FOUND;
    }
    if (who != WHO_AM_I_ICM20948) {
        /* Something answers, and it is not this part. Say what it said: an
           address is not a part number, and guessing is what put two
           addresses in the handoff notes that were never there. */
        ESP_LOGW(TAG, "0x%02X answered 0x%02X, expected 0x%02X for an ICM20948",
                 LS_BOARD_IMU_I2C_ADDR, who, WHO_AM_I_ICM20948);
        return ESP_ERR_NOT_FOUND;
    }

    /* Reset, then wake. The datasheet asks for 100 ms after a device reset
       and the part answers its address long before it is ready to be
       configured, so this delay is not optional. */
    wr(B0_PWR_MGMT_1, PWR1_DEVICE_RESET);
    vTaskDelay(pdMS_TO_TICKS(100));
    s_bank = 0xFF;
    if (!bank(0)) return ESP_ERR_INVALID_STATE;

    wr(B0_PWR_MGMT_1, PWR1_CLK_AUTO);   /* clears SLEEP, best clock source */
    vTaskDelay(pdMS_TO_TICKS(10));
    wr(B0_PWR_MGMT_2, 0x00);            /* accelerometer and gyroscope on */

    /* Bank 2: leave both at their reset full scales and let the internal low
       pass filters run. Nothing here is measuring a transient. */
    if (bank(2)) {
        wr(B2_GYRO_SMPLRT, 10);         /* about 100 Hz */
        wr(B2_GYRO_CONFIG_1, 0x01);     /* +-250 dps, filter on */
        wr(B2_ACCEL_CFG, 0x01);         /* +-2 g, filter on */
    }
    bank(0);

    s_present = true;
    ESP_LOGI(TAG, "ICM20948 up at 0x%02X", LS_BOARD_IMU_I2C_ADDR);

    mag_start();

    /* Let the first samples exist before anybody asks for one. */

    vTaskDelay(pdMS_TO_TICKS(60));
    ls_imu_read(NULL);
    s_cache_us = 0;          /* that sample was the warm-up, not a reading */
    return ESP_OK;
}

bool ls_imu_present(void) { return s_present; }

/* ------------------------------------------------------------------ read -- */

static void mount_xy(float x, float y, float *sx, float *sy)
{
#if LS_BOARD_IMU_MOUNT_DEG == 90
    *sx = -y; *sy =  x;
#elif LS_BOARD_IMU_MOUNT_DEG == 180
    *sx = -x; *sy = -y;
#elif LS_BOARD_IMU_MOUNT_DEG == 270
    *sx =  y; *sy = -x;
#else
    *sx =  x; *sy =  y;
#endif
}

static int16_t be16(const uint8_t *p) { return (int16_t)((p[0] << 8) | p[1]); }

bool ls_imu_read(ls_imu_sample_t *out)
{
    if (!s_present) return false;

    const int64_t now = esp_timer_get_time();
    if (s_cache_us && (now - s_cache_us) < CACHE_US) {
        if (out) *out = s_cache;
        return true;
    }

    if (!bank(0)) return false;
    uint8_t b[14];
    if (!rd(B0_ACCEL_XOUT_H, b, sizeof(b))) return false;

    /* Turned into SCREEN axes on the way out, once, here.

       Everything downstream - pose, heading, anything added later - is then
       already speaking the coordinates the screen is drawn in, and exactly
       one place knows how the part is glued down. See mount_xy. */
    ls_imu_sample_t s;
    memset(&s, 0, sizeof(s));
    mount_xy((float)be16(b + 0) / ACCEL_LSB_PER_G,
             (float)be16(b + 2) / ACCEL_LSB_PER_G, &s.ax, &s.ay);
    s.az = (float)be16(b + 4) / ACCEL_LSB_PER_G;
    mount_xy((float)be16(b + 6)  / GYRO_LSB_PER_DPS,
             (float)be16(b + 8)  / GYRO_LSB_PER_DPS, &s.gx, &s.gy);
    s.gz = (float)be16(b + 10) / GYRO_LSB_PER_DPS;
    /* Datasheet: degrees C = (raw / 333.87) + 21. */
    s.temp_c = ((float)be16(b + 12) / 333.87f) + 21.0f;

    if (s_mag_present) {
        uint8_t st1 = 0;
        if (mag_rd(MAG_ST1, &st1, 1) && (st1 & 0x01)) {
            uint8_t m[8];
            /* Six data bytes AND ST2, in one read.

               ST2 is the measurement-complete register and reading it is what
               releases the AK09916 to take the next sample. A driver that
               reads only the data gets one reading and then the same reading
               for ever, which looks like a compass that has frozen rather
               than like a missing register read. */
            if (mag_rd(MAG_HXL, m, sizeof(m))) {
                /* The magnetometer is little endian where the ICM is big -
                   they are different silicon in one package. */
                const int16_t hx = (int16_t)(m[0] | (m[1] << 8));
                const int16_t hy = (int16_t)(m[2] | (m[3] << 8));
                const int16_t hz = (int16_t)(m[4] | (m[5] << 8));
                const uint8_t st2 = m[7];
                /* Bit 3 is the overflow flag; the sample is meaningless. */
                if (!(st2 & 0x08)) {

                    mount_xy((float)hy * MAG_UT_PER_LSB,
                             (float)hx * MAG_UT_PER_LSB, &s.mx, &s.my);
                    s.mz = (float)-hz * MAG_UT_PER_LSB;
                    s.mag_valid = true;
                }
            }
        } else if (s_cache_us) {
            /* No new sample this time round: keep the last good one rather
               than reporting a field of zero, which reads as "pointing at
               nothing" instead of "nothing new yet". */
            s.mx = s_cache.mx; s.my = s_cache.my; s.mz = s_cache.mz;
            s.mag_valid = s_cache.mag_valid;
        }
    }

    s_cache = s;
    s_cache_us = now;
    if (out) *out = s;
    return true;
}

/* ------------------------------------------------------------------ pose -- */

/* Hysteresis, and why the numbers are what they are. */

#define POSE_ENTER  0.60f
#define POSE_FLAT_Z 0.80f
#define POSE_HOLD_US 500000

static ls_imu_pose_t s_pose = LS_IMU_FLAT;
static ls_imu_pose_t s_pose_pending = LS_IMU_FLAT;
static int64_t       s_pose_since;

static ls_imu_pose_t pose_of(const ls_imu_sample_t *s)
{
    if (fabsf(s->az) > POSE_FLAT_Z) return LS_IMU_FLAT;
    if (s->ay >  POSE_ENTER) return LS_IMU_UP;
    if (s->ay < -POSE_ENTER) return LS_IMU_DOWN;
    if (s->ax >  POSE_ENTER) return LS_IMU_RIGHT;
    if (s->ax < -POSE_ENTER) return LS_IMU_LEFT;
    return LS_IMU_FLAT;
}

ls_imu_pose_t ls_imu_pose(void)
{
    ls_imu_sample_t s;
    if (!ls_imu_read(&s)) return LS_IMU_FLAT;

    const ls_imu_pose_t now_pose = pose_of(&s);
    const int64_t now = esp_timer_get_time();

    /* FLAT is never adopted. It is the absence of an answer, not an answer,
       and adopting it would mean a screen laid flat on a bench forgets which
       way it was being read a moment ago. */
    if (now_pose == LS_IMU_FLAT) {
        s_pose_pending = s_pose;
        s_pose_since = now;
        return s_pose;
    }
    if (now_pose != s_pose_pending) {
        s_pose_pending = now_pose;
        s_pose_since = now;
        return s_pose;
    }
    if (now_pose != s_pose && (now - s_pose_since) >= POSE_HOLD_US)
        s_pose = now_pose;
    return s_pose;
}

float ls_imu_heading(void)
{
    ls_imu_sample_t s;
    if (!ls_imu_read(&s) || !s.mag_valid) return -1.0f;

    /* Flat-earth heading off X and Y. Tilt compensation needs the gravity
       vector folded in and is worth doing when something asks for a heading
       while the board is not level; saying so in the header is honest, and
       inventing the correction here would not be. */
    float deg = atan2f(s.my, s.mx) * 180.0f / (float)M_PI;
    if (deg < 0.0f) deg += 360.0f;
    return deg;
}

#else  /* !LS_HAS_IMU */

esp_err_t ls_imu_start(void) { return ESP_ERR_NOT_SUPPORTED; }
bool ls_imu_present(void) { return false; }
bool ls_imu_read(ls_imu_sample_t *out) { (void)out; return false; }
ls_imu_pose_t ls_imu_pose(void) { return LS_IMU_FLAT; }
float ls_imu_heading(void) { return -1.0f; }

#endif /* LS_HAS_IMU */

/* SPDX-License-Identifier: GPL-3.0-or-later
   LakeShark original. Not librtlsdr - see UPSTREAM.md in this
   directory for which files here are third-party and which are ours. */
#include "tx_plan.h"

#include <string.h>

#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "ls_board.h"
#include "tx_broker_private.h"
#include "tx_driver_private.h"
#include "tx_policy_private.h"

#define TX_DRIVER_MAX 4
#define TX_PLAN_MAX 4
#define TX_TOKEN_LIFETIME_US UINT64_C(5000000)

typedef struct {
    bool used;
    unsigned in_flight;
    char endpoint_id[LS_RADIO_ENDPOINT_ID_MAX];
    ls_radio_range_t ranges[LS_RADIO_MAX_FREQUENCY_RANGES];
    ls_radio_tx_driver_t driver;
} tx_driver_slot_t;

typedef struct {
    bool active;
    bool authorized;
    ls_radio_tx_plan_t plan;
    ls_radio_tx_token_t token;
    uint64_t token_expires_us;
} tx_plan_slot_t;

static tx_driver_slot_t s_drivers[TX_DRIVER_MAX];
static tx_plan_slot_t s_plans[TX_PLAN_MAX];
static uint64_t s_next_plan = 1;
static uint8_t s_token_secret[32];
static bool s_secret_ready;
static uint64_t s_last_gesture_sequence;
static SemaphoreHandle_t s_lock;
static volatile int s_lock_init;

static bool ensure_lock(void)
{
    if (__atomic_load_n(&s_lock_init, __ATOMIC_ACQUIRE) != 2) {
        int expected = 0;
        if (__atomic_compare_exchange_n(&s_lock_init, &expected, 1, false,
                                        __ATOMIC_ACQ_REL,
                                        __ATOMIC_ACQUIRE)) {
            s_lock = xSemaphoreCreateMutex();
            __atomic_store_n(&s_lock_init, s_lock ? 2 : 0,
                             __ATOMIC_RELEASE);
        } else {
            while (__atomic_load_n(&s_lock_init, __ATOMIC_ACQUIRE) == 1)
                vTaskDelay(pdMS_TO_TICKS(1));
        }
    }
    return s_lock != NULL;
}

/* Small self-contained SHA-256 keeps plan digests identical in firmware and
 * the host gate without making policy correctness depend on a crypto shim. */
typedef struct {
    uint32_t h[8];
    uint64_t bits;
    uint8_t block[64];
    size_t used;
} sha256_t;

static const uint32_t s_sha_k[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2,
};

static uint32_t rotr(uint32_t v, unsigned n)
{
    return (v >> n) | (v << (32u - n));
}

static void sha_transform(sha256_t *s)
{
    uint32_t w[64];
    for (unsigned i = 0; i < 16; ++i) {
        const uint8_t *p = &s->block[i * 4];
        w[i] = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
               ((uint32_t)p[2] << 8) | p[3];
    }
    for (unsigned i = 16; i < 64; ++i) {
        uint32_t a = w[i - 15], b = w[i - 2];
        uint32_t s0 = rotr(a, 7) ^ rotr(a, 18) ^ (a >> 3);
        uint32_t s1 = rotr(b, 17) ^ rotr(b, 19) ^ (b >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a=s->h[0], b=s->h[1], c=s->h[2], d=s->h[3];
    uint32_t e=s->h[4], f=s->h[5], g=s->h[6], h=s->h[7];
    for (unsigned i = 0; i < 64; ++i) {
        uint32_t s1 = rotr(e,6) ^ rotr(e,11) ^ rotr(e,25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + s1 + ch + s_sha_k[i] + w[i];
        uint32_t s0 = rotr(a,2) ^ rotr(a,13) ^ rotr(a,22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = s0 + maj;
        h=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
    }
    s->h[0]+=a; s->h[1]+=b; s->h[2]+=c; s->h[3]+=d;
    s->h[4]+=e; s->h[5]+=f; s->h[6]+=g; s->h[7]+=h;
}

static void sha_init(sha256_t *s)
{
    static const uint32_t initial[8] = {
        0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
        0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19,
    };
    memset(s, 0, sizeof(*s));
    memcpy(s->h, initial, sizeof(initial));
}

static void sha_update(sha256_t *s, const void *data_, size_t bytes)
{
    const uint8_t *data = (const uint8_t *)data_;
    s->bits += (uint64_t)bytes * 8;
    while (bytes != 0) {
        size_t amount = 64 - s->used;
        if (amount > bytes) amount = bytes;
        memcpy(&s->block[s->used], data, amount);
        s->used += amount;
        data += amount;
        bytes -= amount;
        if (s->used == 64) {
            sha_transform(s);
            s->used = 0;
        }
    }
}

static void sha_final(sha256_t *s, uint8_t out[32])
{
    uint64_t bits = s->bits;
    const uint8_t one = 0x80, zero = 0;
    sha_update(s, &one, 1);
    while (s->used != 56) sha_update(s, &zero, 1);
    uint8_t length[8];
    for (unsigned i = 0; i < 8; ++i)
        length[7 - i] = (uint8_t)(bits >> (i * 8));
    sha_update(s, length, sizeof(length));
    for (unsigned i = 0; i < 8; ++i) {
        out[i * 4] = (uint8_t)(s->h[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(s->h[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(s->h[i] >> 8);
        out[i * 4 + 3] = (uint8_t)s->h[i];
    }
}

static void hash_u32(sha256_t *hash, uint32_t value)
{
    uint8_t b[4] = {(uint8_t)value, (uint8_t)(value >> 8),
                    (uint8_t)(value >> 16), (uint8_t)(value >> 24)};
    sha_update(hash, b, sizeof(b));
}

static void digest_plan(const ls_radio_tx_plan_t *plan, uint8_t out[32])
{
    sha256_t hash;
    sha_init(&hash);
    sha_update(&hash, &plan->id, sizeof(plan->id));
    sha_update(&hash, plan->endpoint_id, strlen(plan->endpoint_id) + 1);
    hash_u32(&hash, plan->policy_profile_id);
    hash_u32(&hash, plan->regulatory_domain_id);
    hash_u32(&hash, plan->policy_band_id);
    hash_u32(&hash, plan->frequency_hz);
    hash_u32(&hash, plan->modulation);
    hash_u32(&hash, plan->bandwidth_hz);
    hash_u32(&hash, plan->bitrate);
    hash_u32(&hash, plan->deviation_hz);
    hash_u32(&hash, (uint32_t)plan->power_tenths_dbm);
    hash_u32(&hash, (uint32_t)plan->antenna_gain_tenths_dbi);
    hash_u32(&hash, plan->antenna_gain_known ? 1u : 0u);
    hash_u32(&hash, plan->time_on_air_us);
    hash_u32(&hash, plan->max_dwell_us);
    hash_u32(&hash, plan->duty_window_us);
    hash_u32(&hash, plan->duty_remaining_us);
    hash_u32(&hash, (uint32_t)plan->payload_bytes);
    sha_update(&hash, plan->payload, plan->payload_bytes);
    sha_final(&hash, out);
}

static tx_driver_slot_t *find_driver_locked(const char *endpoint_id)
{
    for (size_t i = 0; i < TX_DRIVER_MAX; ++i)
        if (s_drivers[i].used &&
            strcmp(s_drivers[i].endpoint_id, endpoint_id) == 0)
            return &s_drivers[i];
    return NULL;
}

static bool in_ranges(uint32_t hz, const ls_radio_range_t *ranges,
                      size_t count)
{
    for (size_t i = 0; i < count; ++i)
        if (hz >= ranges[i].min_hz && hz <= ranges[i].max_hz) return true;
    return false;
}

ls_radio_tx_err_t ls_radio_tx_driver_register(
    const ls_radio_tx_driver_t *driver)
{
    if (!driver || !driver->endpoint_id || !driver->endpoint_id[0] ||
        strlen(driver->endpoint_id) >= LS_RADIO_ENDPOINT_ID_MAX ||
        !driver->frequency_ranges || driver->frequency_range_count == 0 ||
        driver->frequency_range_count > LS_RADIO_MAX_FREQUENCY_RANGES ||
        driver->modulation_mask == 0 || driver->min_bandwidth_hz == 0 ||
        driver->min_bandwidth_hz > driver->max_bandwidth_hz ||
        driver->min_power_tenths_dbm > driver->max_power_tenths_dbm ||
        driver->max_payload_bytes == 0 ||
        driver->max_payload_bytes > LS_RADIO_TX_PAYLOAD_MAX ||
        driver->max_time_on_air_us == 0 || !driver->ops ||
        !driver->ops->estimate_airtime || !driver->ops->transmit ||
        !ensure_lock())
        return LS_RADIO_TX_ERR_INVALID;
    for (size_t i = 0; i < driver->frequency_range_count; ++i)
        if (driver->frequency_ranges[i].min_hz >
            driver->frequency_ranges[i].max_hz)
            return LS_RADIO_TX_ERR_INVALID;

    ls_radio_endpoint_info_t endpoint;
    if (ls_radio_endpoint_get(driver->endpoint_id, &endpoint) != LS_RADIO_OK ||
        !endpoint.present ||
        (endpoint.capabilities & LS_RADIO_TX_PACKET) == 0)
        return LS_RADIO_TX_ERR_ENDPOINT;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (find_driver_locked(driver->endpoint_id)) {
        xSemaphoreGive(s_lock);
        return LS_RADIO_TX_ERR_INVALID;
    }
    tx_driver_slot_t *slot = NULL;
    for (size_t i = 0; i < TX_DRIVER_MAX; ++i)
        if (!s_drivers[i].used) { slot = &s_drivers[i]; break; }
    if (!slot) {
        xSemaphoreGive(s_lock);
        return LS_RADIO_TX_ERR_NO_SPACE;
    }
    memset(slot, 0, sizeof(*slot));
    slot->used = true;
    strncpy(slot->endpoint_id, driver->endpoint_id,
            sizeof(slot->endpoint_id) - 1);
    memcpy(slot->ranges, driver->frequency_ranges,
           driver->frequency_range_count * sizeof(slot->ranges[0]));
    slot->driver = *driver;
    slot->driver.endpoint_id = slot->endpoint_id;
    slot->driver.frequency_ranges = slot->ranges;
    xSemaphoreGive(s_lock);
    return LS_RADIO_TX_OK;
}

ls_radio_tx_err_t ls_radio_tx_driver_unregister(const char *endpoint_id)
{
    if (!endpoint_id || !ensure_lock()) return LS_RADIO_TX_ERR_INVALID;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    tx_driver_slot_t *slot = find_driver_locked(endpoint_id);
    if (!slot) {
        xSemaphoreGive(s_lock);
        return LS_RADIO_TX_ERR_ENDPOINT;
    }
    if (slot->in_flight != 0) {
        xSemaphoreGive(s_lock);
        return LS_RADIO_TX_ERR_DRIVER;
    }
    memset(slot, 0, sizeof(*slot));
    xSemaphoreGive(s_lock);
    return LS_RADIO_TX_OK;
}

static ls_radio_tx_err_t endpoint_and_driver_check(
    const ls_radio_tx_plan_t *plan, const tx_driver_slot_t *slot,
    uint32_t *airtime_us)
{
    ls_radio_endpoint_info_t endpoint;
    if (!slot || ls_radio_endpoint_get(plan->endpoint_id, &endpoint) !=
                     LS_RADIO_OK || !endpoint.present ||
        (endpoint.capabilities & LS_RADIO_TX_PACKET) == 0 ||
        !in_ranges(plan->frequency_hz, endpoint.frequency_ranges,
                   endpoint.frequency_range_count) ||
        !in_ranges(plan->frequency_hz, slot->driver.frequency_ranges,
                   slot->driver.frequency_range_count))
        return LS_RADIO_TX_ERR_ENDPOINT;
    const ls_radio_tx_driver_t *driver = &slot->driver;
    if (plan->modulation == 0 ||
        (plan->modulation & (plan->modulation - 1u)) != 0 ||
        (driver->modulation_mask & plan->modulation) == 0)
        return LS_RADIO_TX_ERR_MODULATION;
    if (plan->bandwidth_hz < driver->min_bandwidth_hz ||
        plan->bandwidth_hz > driver->max_bandwidth_hz)
        return LS_RADIO_TX_ERR_BANDWIDTH;
    if (plan->power_tenths_dbm < driver->min_power_tenths_dbm ||
        plan->power_tenths_dbm > driver->max_power_tenths_dbm)
        return LS_RADIO_TX_ERR_POWER;
    if (plan->payload_bytes == 0 ||
        plan->payload_bytes > driver->max_payload_bytes)
        return LS_RADIO_TX_ERR_INVALID;
    ls_radio_tx_packet_t packet = {
        .frequency_hz = plan->frequency_hz,
        .modulation = plan->modulation,
        .bandwidth_hz = plan->bandwidth_hz,
        .bitrate = plan->bitrate,
        .deviation_hz = plan->deviation_hz,
        .power_tenths_dbm = plan->power_tenths_dbm,
        .payload = plan->payload,
        .payload_bytes = plan->payload_bytes,
    };
    uint32_t estimate = 0;
    if (driver->ops->estimate_airtime(driver->driver_ctx, &packet, &estimate)
            != LS_RADIO_OK || estimate == 0 ||
        estimate > driver->max_time_on_air_us)
        return LS_RADIO_TX_ERR_AIRTIME;
    *airtime_us = estimate;
    return LS_RADIO_TX_OK;
}

static ls_radio_tx_policy_check_t policy_check_for(
    const ls_radio_tx_plan_t *plan, uint32_t airtime_us)
{
    ls_radio_tx_policy_check_t check = {
        .profile_id = plan->policy_profile_id,
        .regulatory_domain_id = plan->regulatory_domain_id,
        .endpoint_id = plan->endpoint_id,
        .frequency_hz = plan->frequency_hz,
        .modulation = plan->modulation,
        .bandwidth_hz = plan->bandwidth_hz,
        .power_tenths_dbm = plan->power_tenths_dbm,
        .antenna_gain_tenths_dbi = plan->antenna_gain_tenths_dbi,
        .antenna_gain_known = plan->antenna_gain_known,
        .airtime_us = airtime_us,
    };
    return check;
}

ls_radio_tx_err_t ls_radio_tx_plan(const ls_radio_tx_request_t *request,
                                   ls_radio_tx_plan_t *out)
{
    if (!request || !out || !request->endpoint_id ||
        !request->endpoint_id[0] ||
        strlen(request->endpoint_id) >= LS_RADIO_ENDPOINT_ID_MAX ||
        !request->payload || request->payload_bytes == 0 ||
        request->payload_bytes > LS_RADIO_TX_PAYLOAD_MAX || !ensure_lock())
        return LS_RADIO_TX_ERR_INVALID;

    ls_radio_tx_plan_t candidate;
    memset(&candidate, 0, sizeof(candidate));
    strncpy(candidate.endpoint_id, request->endpoint_id,
            sizeof(candidate.endpoint_id) - 1);
    candidate.policy_profile_id = request->policy_profile_id;
    candidate.regulatory_domain_id = request->regulatory_domain_id;
    candidate.frequency_hz = request->frequency_hz;
    candidate.modulation = request->modulation;
    candidate.bandwidth_hz = request->bandwidth_hz;
    candidate.bitrate = request->bitrate;
    candidate.deviation_hz = request->deviation_hz;
    candidate.power_tenths_dbm = request->power_tenths_dbm;
    candidate.antenna_gain_tenths_dbi = request->antenna_gain_tenths_dbi;
    candidate.antenna_gain_known = request->antenna_gain_known;
    candidate.payload_bytes = request->payload_bytes;
    memcpy(candidate.payload, request->payload, request->payload_bytes);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    tx_driver_slot_t *driver = find_driver_locked(candidate.endpoint_id);
    uint32_t airtime = 0;
    ls_radio_tx_err_t error =
        endpoint_and_driver_check(&candidate, driver, &airtime);
    if (error == LS_RADIO_TX_OK) {
        ls_radio_tx_policy_check_t check = policy_check_for(&candidate, airtime);
        error = ls_radio_tx_policy_check(&check,
            (uint64_t)esp_timer_get_time(), &candidate.policy_band_id,
            &candidate.max_dwell_us, &candidate.duty_window_us,
            &candidate.duty_remaining_us);
    }
    if (error != LS_RADIO_TX_OK) {
        xSemaphoreGive(s_lock);
        return error;
    }
    candidate.time_on_air_us = airtime;

    tx_plan_slot_t *slot = NULL;
    for (size_t i = 0; i < TX_PLAN_MAX; ++i)
        if (!s_plans[i].active) { slot = &s_plans[i]; break; }
    if (!slot) {
        xSemaphoreGive(s_lock);
        return LS_RADIO_TX_ERR_NO_SPACE;
    }
    candidate.id = s_next_plan++;
    if (candidate.id == 0) candidate.id = s_next_plan++;
    digest_plan(&candidate, candidate.digest);
    memset(slot, 0, sizeof(*slot));
    slot->active = true;
    slot->plan = candidate;
    *out = candidate;
    xSemaphoreGive(s_lock);
    return LS_RADIO_TX_OK;
}

static bool token_zero(const ls_radio_tx_token_t *token)
{
    uint8_t any = 0;
    for (size_t i = 0; i < sizeof(token->bytes); ++i) any |= token->bytes[i];
    return any == 0;
}

ls_radio_tx_err_t ls_radio_tx_authorize_physical(
    const ls_radio_tx_plan_t *displayed_plan,
    uint64_t local_gesture_sequence,
    ls_radio_tx_token_t *one_shot)
{
#if !LS_USE_DISPLAY
    (void)displayed_plan;
    (void)local_gesture_sequence;
    (void)one_shot;
    return LS_RADIO_TX_ERR_HEADLESS;
#else
    if (!displayed_plan || !one_shot || local_gesture_sequence == 0 ||
        !ensure_lock())
        return LS_RADIO_TX_ERR_AUTH;
    uint8_t displayed_digest[32];
    digest_plan(displayed_plan, displayed_digest);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    tx_plan_slot_t *slot = NULL;
    for (size_t i = 0; i < TX_PLAN_MAX; ++i)
        if (s_plans[i].active &&
            s_plans[i].plan.id == displayed_plan->id) {
            slot = &s_plans[i];
            break;
        }
    /*LS-190  A token previously represented only an app's intent. Bind random
      one-shot material to the exact displayed plan and endpoint so restored
      state, edits, retries, and a different endpoint cannot key the radio. */
    if (!slot || local_gesture_sequence <= s_last_gesture_sequence ||
        memcmp(displayed_digest, slot->plan.digest, 32) != 0 ||
        memcmp(displayed_plan->digest, slot->plan.digest, 32) != 0 ||
        memcmp(displayed_plan, &slot->plan, sizeof(*displayed_plan)) != 0) {
        xSemaphoreGive(s_lock);
        return LS_RADIO_TX_ERR_AUTH;
    }
    s_last_gesture_sequence = local_gesture_sequence;
    if (!s_secret_ready) {
        esp_fill_random(s_token_secret, sizeof(s_token_secret));
        s_secret_ready = true;
    }
    uint64_t now = (uint64_t)esp_timer_get_time();
    sha256_t hash;
    sha_init(&hash);
    sha_update(&hash, s_token_secret, sizeof(s_token_secret));
    sha_update(&hash, slot->plan.digest, sizeof(slot->plan.digest));
    sha_update(&hash, &local_gesture_sequence, sizeof(local_gesture_sequence));
    sha_update(&hash, &now, sizeof(now));
    sha_update(&hash, &slot->plan.id, sizeof(slot->plan.id));
    sha_final(&hash, slot->token.bytes);
    if (token_zero(&slot->token)) slot->token.bytes[0] = 1;
    slot->token_expires_us = now + TX_TOKEN_LIFETIME_US;
    slot->authorized = true;
    *one_shot = slot->token;
    xSemaphoreGive(s_lock);
    return LS_RADIO_TX_OK;
#endif
}

ls_radio_tx_err_t ls_radio_tx_commit(ls_radio_tx_plan_id_t plan_id,
                                     ls_radio_tx_token_t one_shot)
{
    if (plan_id == 0 || token_zero(&one_shot) || !ensure_lock())
        return LS_RADIO_TX_ERR_AUTH;
    xSemaphoreTake(s_lock, portMAX_DELAY);

    tx_plan_slot_t *authorized = NULL;
    for (size_t i = 0; i < TX_PLAN_MAX; ++i)
        if (s_plans[i].active && s_plans[i].authorized &&
            memcmp(s_plans[i].token.bytes, one_shot.bytes,
                   sizeof(one_shot.bytes)) == 0) {
            authorized = &s_plans[i];
            break;
        }
    if (!authorized) {
        xSemaphoreGive(s_lock);
        return LS_RADIO_TX_ERR_AUTH;
    }
    authorized->authorized = false; /* consume before every other check */
    authorized->active = false;
    uint64_t now = (uint64_t)esp_timer_get_time();
    if (now > authorized->token_expires_us) {
        xSemaphoreGive(s_lock);
        return LS_RADIO_TX_ERR_EXPIRED;
    }
    if (authorized->plan.id != plan_id) {
        xSemaphoreGive(s_lock);
        return LS_RADIO_TX_ERR_AUTH;
    }

    ls_radio_tx_plan_t plan = authorized->plan;
    tx_driver_slot_t *driver = find_driver_locked(plan.endpoint_id);
    uint32_t airtime = 0;
    ls_radio_tx_err_t error = endpoint_and_driver_check(&plan, driver, &airtime);
    if (error == LS_RADIO_TX_OK && airtime != plan.time_on_air_us)
        error = LS_RADIO_TX_ERR_AIRTIME;
    ls_radio_tx_reservation_t reservation = 0;
    if (error == LS_RADIO_TX_OK) {
        ls_radio_tx_policy_check_t check = policy_check_for(&plan, airtime);
        error = ls_radio_tx_policy_reserve(&check, now, &reservation);
    }
    if (error != LS_RADIO_TX_OK) {
        xSemaphoreGive(s_lock);
        return error;
    }
    ++driver->in_flight;
    ls_radio_tx_driver_t driver_copy = driver->driver;
    xSemaphoreGive(s_lock);

    ls_radio_tx_packet_t packet = {
        .frequency_hz = plan.frequency_hz,
        .modulation = plan.modulation,
        .bandwidth_hz = plan.bandwidth_hz,
        .bitrate = plan.bitrate,
        .deviation_hz = plan.deviation_hz,
        .power_tenths_dbm = plan.power_tenths_dbm,
        .payload = plan.payload,
        .payload_bytes = plan.payload_bytes,
    };
    ls_radio_tx_report_t report;
    memset(&report, 0, sizeof(report));
    ls_radio_err_t driver_error = driver_copy.ops->transmit(
        driver_copy.driver_ctx, &packet, &report);
    ls_radio_tx_policy_finish(reservation, &report, driver_error,
                              (uint64_t)esp_timer_get_time());

    xSemaphoreTake(s_lock, portMAX_DELAY);
    tx_driver_slot_t *live = find_driver_locked(plan.endpoint_id);
    if (live && live->in_flight != 0) --live->in_flight;
    xSemaphoreGive(s_lock);
    return driver_error == LS_RADIO_OK ? LS_RADIO_TX_OK
                                       : LS_RADIO_TX_ERR_DRIVER;
}

void ls_radio_tx_cancel(ls_radio_tx_plan_id_t plan_id)
{
    if (!plan_id || !ensure_lock()) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (size_t i = 0; i < TX_PLAN_MAX; ++i)
        if (s_plans[i].active && s_plans[i].plan.id == plan_id) {
            memset(&s_plans[i], 0, sizeof(s_plans[i]));
            break;
        }
    xSemaphoreGive(s_lock);
}

void ls_radio_tx_broker_reset_for_test(void)
{
    if (!ensure_lock()) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memset(s_drivers, 0, sizeof(s_drivers));
    memset(s_plans, 0, sizeof(s_plans));
    memset(s_token_secret, 0, sizeof(s_token_secret));
    s_secret_ready = false;
    s_last_gesture_sequence = 0;
    s_next_plan = 1;
    xSemaphoreGive(s_lock);
    ls_radio_tx_policy_clear();
}

const char *ls_radio_tx_err_name(ls_radio_tx_err_t error)
{
    switch (error) {
    case LS_RADIO_TX_OK: return "ok";
    case LS_RADIO_TX_ERR_INVALID: return "invalid";
    case LS_RADIO_TX_ERR_NO_POLICY: return "no-policy";
    case LS_RADIO_TX_ERR_ENDPOINT: return "endpoint";
    case LS_RADIO_TX_ERR_DOMAIN: return "domain";
    case LS_RADIO_TX_ERR_BAND: return "band";
    case LS_RADIO_TX_ERR_MODULATION: return "modulation";
    case LS_RADIO_TX_ERR_BANDWIDTH: return "bandwidth";
    case LS_RADIO_TX_ERR_POWER: return "power";
    case LS_RADIO_TX_ERR_AIRTIME: return "airtime";
    case LS_RADIO_TX_ERR_DWELL: return "dwell";
    case LS_RADIO_TX_ERR_DUTY: return "duty";
    case LS_RADIO_TX_ERR_AUTH: return "authorization";
    case LS_RADIO_TX_ERR_EXPIRED: return "expired";
    case LS_RADIO_TX_ERR_CANCELLED: return "cancelled";
    case LS_RADIO_TX_ERR_DRIVER: return "driver";
    case LS_RADIO_TX_ERR_NO_SPACE: return "no-space";
    case LS_RADIO_TX_ERR_HEADLESS: return "headless";
    default: return "unknown";
    }
}

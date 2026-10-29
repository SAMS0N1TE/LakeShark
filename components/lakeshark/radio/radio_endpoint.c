/* SPDX-License-Identifier: GPL-3.0-or-later
   LakeShark original. Not librtlsdr - see UPSTREAM.md in this
   directory for which files here are third-party and which are ours. */
#include "radio_endpoint.h"

#include <string.h>
#include "esp_attr.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "ls_cold.h"

#define LS_RADIO_MAX_ENDPOINTS 8
#define LS_RADIO_MAX_WAITERS   4
#define LS_RADIO_MAX_SUBSCRIBERS 8

typedef struct endpoint_slot endpoint_slot_t;

struct ls_radio_session {
    endpoint_slot_t *endpoint;
    bool active;
    bool closing;
    ls_radio_requirements_t requirements;
};

struct endpoint_slot {
    bool used;
    bool present;
    bool removing;
    bool leased;
    bool configured;
    uint8_t config_kind;
    bool streaming;
    bool paused;
    bool pause_pending;
    bool stopping;
    uint8_t stream_kind;
    unsigned in_flight;
    unsigned waiters;
    char endpoint_id[LS_RADIO_ENDPOINT_ID_MAX];
    char name[LS_RADIO_ENDPOINT_NAME_MAX];
    char owner[LS_RADIO_OWNER_MAX];
    uint32_t capabilities;
    ls_radio_duplex_t duplex;
    uint32_t iq_formats;
    uint32_t packet_formats;
    ls_radio_range_t frequency_ranges[LS_RADIO_MAX_FREQUENCY_RANGES];
    size_t frequency_range_count;
    bool tuner_model_known;
    ls_radio_range_t sample_rate_ranges[LS_RADIO_MAX_RATE_RANGES];
    size_t sample_rate_range_count;
    ls_radio_driver_ops_t ops;
    void *driver_ctx;
    ls_radio_iq_config_t actual_iq;
    ls_radio_packet_config_t actual_packet;
    uint64_t bytes_read;
    uint64_t packets_read;
    uint64_t diagnostic_context_epoch, diagnostic_counter_epoch;
    unsigned diagnostic_controls;
    bool diagnostic_counter_overflow;
    ls_radio_err_t last_error;
    uint32_t control_io_errors;
    SemaphoreHandle_t control_lock;
    SemaphoreHandle_t quiesced;
    ls_radio_session_t session;
    uint32_t fe_ctls;
};

static EXT_RAM_BSS_ATTR endpoint_slot_t s_endpoints[LS_RADIO_MAX_ENDPOINTS];
static SemaphoreHandle_t s_registry_lock;
static volatile int s_registry_init;
/**/
static uint32_t s_generation;
static uint64_t s_diagnostic_epoch;

/* Registry lock held. Saturation stays unknown for the rest of this boot. */
static uint64_t diagnostic_epoch_next(void)
{
    if (s_diagnostic_epoch != UINT64_MAX) ++s_diagnostic_epoch;
    return s_diagnostic_epoch;
}
static ls_radio_iq_tap_fn s_iq_tap;

typedef struct {
    ls_radio_endpoint_event_fn callback;
    void *user;
} endpoint_subscriber_t;

static endpoint_subscriber_t s_subscribers[LS_RADIO_MAX_SUBSCRIBERS];

LS_COLD static ls_radio_err_t ensure_registry(void)
{
    if (__atomic_load_n(&s_registry_init, __ATOMIC_ACQUIRE) != 2) {
        int expected = 0;
        if (__atomic_compare_exchange_n(&s_registry_init, &expected, 1,
                                        false, __ATOMIC_ACQ_REL,
                                        __ATOMIC_ACQUIRE)) {
            s_registry_lock = xSemaphoreCreateMutex();
            __atomic_store_n(&s_registry_init, s_registry_lock ? 2 : 0,
                             __ATOMIC_RELEASE);
        } else {
            while (__atomic_load_n(&s_registry_init, __ATOMIC_ACQUIRE) == 1)
                vTaskDelay(pdMS_TO_TICKS(1));
        }
    }
    return s_registry_lock ? LS_RADIO_OK : LS_RADIO_ERR_NO_MEMORY;
}

static void copy_string(char *dst, size_t dst_size, const char *src)
{
    if (!dst_size) return;
    if (!src) src = "";
    size_t length = strlen(src);
    if (length >= dst_size) length = dst_size - 1;
    memcpy(dst, src, length);
    dst[length] = '\0';
}

static bool range_valid(const ls_radio_range_t *range)
{
    return range && range->min_hz <= range->max_hz;
}

static bool value_in_ranges(uint64_t value, const ls_radio_range_t *ranges,
                            size_t count)
{
    if (value == 0) return true;
    for (size_t i = 0; i < count; ++i) {
        if (value >= ranges[i].min_hz && value <= ranges[i].max_hz)
            return true;
    }
    return false;
}

static bool span_in_ranges(uint64_t min_hz, uint64_t max_hz,
                           const ls_radio_range_t *ranges, size_t count)
{
    if (min_hz == 0 && max_hz == 0) return true;
    if (min_hz != 0 && max_hz != 0 && min_hz > max_hz) return false;
    for (size_t i = 0; i < count; ++i) {
        bool lower_ok = min_hz == 0 || min_hz >= ranges[i].min_hz;
        bool upper_ok = max_hz == 0 || max_hz <= ranges[i].max_hz;
        if (lower_ok && upper_ok)
            return true;
    }
    return false;
}

static endpoint_slot_t *find_slot_locked(const char *endpoint_id)
{
    for (size_t i = 0; i < LS_RADIO_MAX_ENDPOINTS; ++i) {
        if (s_endpoints[i].used &&
            strcmp(s_endpoints[i].endpoint_id, endpoint_id) == 0)
            return &s_endpoints[i];
    }
    return NULL;
}

LS_COLD static endpoint_slot_t *alloc_slot_locked(const char *endpoint_id)
{
    endpoint_slot_t *reusable = NULL;
    for (size_t i = 0; i < LS_RADIO_MAX_ENDPOINTS; ++i) {
        endpoint_slot_t *slot = &s_endpoints[i];
        if (slot->used && strcmp(slot->endpoint_id, endpoint_id) == 0)
            return slot;
        if (!reusable &&
            (!slot->used ||
             (!slot->present && !slot->removing && !slot->leased)))
            reusable = slot;
    }
    return reusable;
}

LS_COLD static bool endpoint_matches(const endpoint_slot_t *slot,
                             const ls_radio_requirements_t *requirements)
{
    if (!slot->present ||
        (slot->capabilities & requirements->required_caps) !=
            requirements->required_caps)
        return false;
    if (!span_in_ranges(requirements->min_hz, requirements->max_hz,
                        slot->frequency_ranges,
                        slot->frequency_range_count))
        return false;
    if ((requirements->required_caps & LS_RADIO_RX_IQ_U8) != 0) {
        uint32_t format = requirements->iq_format;
        if (format == 0) format = LS_RADIO_IQ_FORMAT_U8_INTERLEAVED;
        if ((slot->iq_formats & format) != format)
            return false;
        if (!value_in_ranges(requirements->sample_rate_hz,
                             slot->sample_rate_ranges,
                             slot->sample_rate_range_count))
            return false;
    }
    if ((requirements->required_caps &
         (LS_RADIO_RX_PACKET | LS_RADIO_TX_PACKET)) != 0 &&
        requirements->packet_format != 0 &&
        (slot->packet_formats & requirements->packet_format) !=
            requirements->packet_format)
        return false;
    return true;
}

static void note_quiesced_locked(endpoint_slot_t *slot)
{
    if (slot->in_flight != 0) return;
    for (unsigned i = 0; i < slot->waiters; ++i)
        xSemaphoreGive(slot->quiesced);
}

LS_COLD static void wait_quiesced(endpoint_slot_t *slot)
{
    for (;;) {
        xSemaphoreTake(s_registry_lock, portMAX_DELAY);
        if (slot->in_flight == 0) {
            xSemaphoreGive(s_registry_lock);
            return;
        }
        ++slot->waiters;
        xSemaphoreGive(s_registry_lock);
        xSemaphoreTake(slot->quiesced, portMAX_DELAY);
        xSemaphoreTake(s_registry_lock, portMAX_DELAY);
        --slot->waiters;
        bool done = slot->in_flight == 0;
        xSemaphoreGive(s_registry_lock);
        if (done) return;
    }
}

static ls_radio_err_t session_enter(ls_radio_session_t *session,
                                    uint32_t required_cap,
                                    bool require_streaming,
                                    endpoint_slot_t **out_slot)
{
    if (!session || !out_slot)
        return LS_RADIO_ERR_INVALID;
    ls_radio_err_t init_error = ensure_registry();
    if (init_error != LS_RADIO_OK) return init_error;
    xSemaphoreTake(s_registry_lock, portMAX_DELAY);
    endpoint_slot_t *slot = session->endpoint;
    ls_radio_err_t error = LS_RADIO_OK;
    if (!session->active || session->closing || !slot || !slot->leased)
        error = LS_RADIO_ERR_INVALID;
    else if (!slot->present)
        error = LS_RADIO_ERR_DISCONNECTED;
    else if ((slot->capabilities & required_cap) != required_cap)
        error = LS_RADIO_ERR_UNSUPPORTED;
    else if (require_streaming && (slot->paused || !slot->streaming))
        error = LS_RADIO_ERR_STOPPED;
    else {
        ++slot->in_flight;
        if (!require_streaming) {
            ++slot->diagnostic_controls;
            slot->diagnostic_context_epoch = diagnostic_epoch_next();
        }
        *out_slot = slot;
    }
    xSemaphoreGive(s_registry_lock);
    return error;
}

static ls_radio_err_t session_leave(endpoint_slot_t *slot,
                                    ls_radio_err_t driver_error,
                                    bool read_call, size_t bytes_read,
                                    size_t packets_read)
{
    xSemaphoreTake(s_registry_lock, portMAX_DELAY);
    ls_radio_err_t error = driver_error;
    if (!slot->present)
        error = LS_RADIO_ERR_DISCONNECTED;
    else if (read_call && slot->stopping)
        error = LS_RADIO_ERR_STOPPED;
    if (read_call && error == LS_RADIO_OK &&
        UINT64_MAX - slot->bytes_read < bytes_read)
        slot->diagnostic_counter_overflow = true;
    if (read_call && error == LS_RADIO_OK)
        slot->bytes_read += bytes_read;
    if (read_call && error == LS_RADIO_OK)
        slot->packets_read += packets_read;
    if (read_call && error == LS_RADIO_OK)
        slot->last_error = LS_RADIO_OK;
    if (error != LS_RADIO_OK)
        slot->last_error = error;
    if (slot->in_flight > 0) --slot->in_flight;
    if (!read_call && slot->diagnostic_controls > 0)
        --slot->diagnostic_controls;
    note_quiesced_locked(slot);
    xSemaphoreGive(s_registry_lock);
    return error;
}

/* For configure, gain, start and retune: a request that reached the
   driver and came back io extends the run, one that succeeded ends it, and
   anything else - DISCONNECTED, BUSY, a refusal before the driver - says
   nothing about whether the device answers. Stop is left out on purpose:
   the RTL's stop is host-side and always succeeds, so counting it would
   clear the run between every failed open. Counted while the operation is
   still in flight, so the slot cannot be re-registered under the count. */
LS_COLD static ls_radio_err_t session_leave_control(endpoint_slot_t *slot,
                                            ls_radio_err_t driver_error)
{
    xSemaphoreTake(s_registry_lock, portMAX_DELAY);
    if (slot->present) {
        if (driver_error == LS_RADIO_ERR_IO)
            ++slot->control_io_errors;
        else if (driver_error == LS_RADIO_OK)
        {
            slot->control_io_errors = 0;
            slot->last_error = LS_RADIO_OK;
        }
    }
    xSemaphoreGive(s_registry_lock);
    return session_leave(slot, driver_error, false, 0, 0);
}

LS_COLD static void fill_info(const endpoint_slot_t *slot,
                      ls_radio_endpoint_info_t *out)
{
    memset(out, 0, sizeof(*out));
    copy_string(out->endpoint_id, sizeof(out->endpoint_id), slot->endpoint_id);
    copy_string(out->name, sizeof(out->name), slot->name);
    copy_string(out->owner, sizeof(out->owner), slot->owner);
    out->capabilities = slot->capabilities;
    out->duplex = slot->duplex;
    out->iq_formats = slot->iq_formats;
    out->packet_formats = slot->packet_formats;
    out->frequency_range_count = slot->frequency_range_count;
    out->tuner_model_known=slot->tuner_model_known;
    memcpy(out->frequency_ranges, slot->frequency_ranges,
           slot->frequency_range_count * sizeof(slot->frequency_ranges[0]));
    out->sample_rate_range_count = slot->sample_rate_range_count;
    memcpy(out->sample_rate_ranges, slot->sample_rate_ranges,
           slot->sample_rate_range_count * sizeof(slot->sample_rate_ranges[0]));
    out->present = slot->present;
    out->leased = slot->leased;
    out->streaming = slot->streaming;
    out->paused = slot->paused;
    out->configured = slot->configured;
    out->actual_iq = slot->actual_iq;
    out->actual_packet = slot->actual_packet;
    out->bytes_read = slot->bytes_read;
    out->packets_read = slot->packets_read;
    out->diagnostic_context_epoch = slot->diagnostic_context_epoch;
    out->diagnostic_counter_epoch = slot->diagnostic_counter_epoch;
    out->diagnostic_context_known = slot->present && !slot->removing &&
        !slot->stopping && !slot->pause_pending && !slot->diagnostic_controls &&
        slot->diagnostic_context_epoch != UINT64_MAX;
    out->diagnostic_counter_known = !slot->diagnostic_counter_overflow &&
        slot->diagnostic_counter_epoch != UINT64_MAX;
    out->last_error = slot->last_error;
    out->control_io_errors = slot->control_io_errors;
    out->fe_ctls = slot->fe_ctls;
}

LS_COLD static void publish_endpoint_event(ls_radio_endpoint_event_kind_t kind,
                                   const char *endpoint_id,
                                   uint32_t capabilities)
{
    endpoint_subscriber_t subscribers[LS_RADIO_MAX_SUBSCRIBERS];
    ls_radio_endpoint_event_t event = {
        .kind = kind,
        .capabilities = capabilities,
    };
    copy_string(event.endpoint_id, sizeof(event.endpoint_id), endpoint_id);

    xSemaphoreTake(s_registry_lock, portMAX_DELAY);
    memcpy(subscribers, s_subscribers, sizeof(subscribers));
    xSemaphoreGive(s_registry_lock);

    for (size_t i = 0; i < LS_RADIO_MAX_SUBSCRIBERS; ++i)
        if (subscribers[i].callback)
            subscribers[i].callback(&event, subscribers[i].user);
}

LS_COLD ls_radio_err_t ls_radio_endpoint_register(const ls_radio_endpoint_t *endpoint)
{
    if (!endpoint || !endpoint->endpoint_id || !endpoint->endpoint_id[0] ||
        !endpoint->name || !endpoint->ops || endpoint->capabilities == 0 ||
        endpoint->duplex > LS_RADIO_DUPLEX_FULL ||
        strlen(endpoint->endpoint_id) >= LS_RADIO_ENDPOINT_ID_MAX ||
        endpoint->frequency_range_count == 0 ||
        !endpoint->frequency_ranges ||
        endpoint->frequency_range_count > LS_RADIO_MAX_FREQUENCY_RANGES ||
        endpoint->sample_rate_range_count > LS_RADIO_MAX_RATE_RANGES ||
        (endpoint->sample_rate_range_count != 0 &&
         !endpoint->sample_rate_ranges))
        return LS_RADIO_ERR_INVALID;
    for (size_t i = 0; i < endpoint->frequency_range_count; ++i)
        if (!range_valid(&endpoint->frequency_ranges[i]))
            return LS_RADIO_ERR_INVALID;
    for (size_t i = 0; i < endpoint->sample_rate_range_count; ++i)
        if (!range_valid(&endpoint->sample_rate_ranges[i]))
            return LS_RADIO_ERR_INVALID;
    if ((endpoint->capabilities & LS_RADIO_RX_IQ_U8) != 0 &&
        ((endpoint->iq_formats & LS_RADIO_IQ_FORMAT_U8_INTERLEAVED) == 0 ||
         endpoint->sample_rate_range_count == 0 ||
         !endpoint->ops->iq_configure || !endpoint->ops->iq_set_gain ||
         !endpoint->ops->iq_start || !endpoint->ops->iq_read ||
         !endpoint->ops->iq_retune || !endpoint->ops->iq_stop ||
         !endpoint->ops->cancel_read))
        return LS_RADIO_ERR_INVALID;
    if ((endpoint->capabilities & LS_RADIO_RX_PACKET) != 0 &&
        (endpoint->packet_formats == 0 ||
         !endpoint->ops->packet_rx_configure ||
         !endpoint->ops->packet_rx_start ||
         !endpoint->ops->packet_rx_read ||
         !endpoint->ops->packet_rx_stop ||
         !endpoint->ops->cancel_read))
        return LS_RADIO_ERR_INVALID;
    if (ensure_registry() != LS_RADIO_OK)
        return LS_RADIO_ERR_NO_MEMORY;

    xSemaphoreTake(s_registry_lock, portMAX_DELAY);
    endpoint_slot_t *slot = alloc_slot_locked(endpoint->endpoint_id);
    if (!slot) {
        xSemaphoreGive(s_registry_lock);
        return LS_RADIO_ERR_NO_MEMORY;
    }
    if (slot->present) {
        xSemaphoreGive(s_registry_lock);
        return LS_RADIO_ERR_EXISTS;
    }
    if (slot->removing || slot->leased) {
        xSemaphoreGive(s_registry_lock);
        return LS_RADIO_ERR_BUSY;
    }
    if (!slot->control_lock)
        slot->control_lock = xSemaphoreCreateMutex();
    if (!slot->quiesced)
        slot->quiesced = xSemaphoreCreateCounting(LS_RADIO_MAX_WAITERS, 0);
    if (!slot->control_lock || !slot->quiesced) {
        xSemaphoreGive(s_registry_lock);
        return LS_RADIO_ERR_NO_MEMORY;
    }

    slot->used = true;
    slot->present = true;
    slot->paused = slot->pause_pending = false;
    slot->removing = false;
    slot->configured = false;
    slot->config_kind = 0;
    slot->streaming = false;
    slot->stopping = false;
    slot->stream_kind = 0;
    slot->in_flight = 0;
    slot->waiters = 0;
    slot->bytes_read = 0;
    slot->packets_read = 0;
    slot->diagnostic_context_epoch = diagnostic_epoch_next();
    slot->diagnostic_counter_epoch = slot->diagnostic_context_epoch;
    slot->diagnostic_controls = 0;
    slot->diagnostic_counter_overflow = false;
    slot->last_error = LS_RADIO_OK;
    slot->control_io_errors = 0;
    __atomic_add_fetch(&s_generation, 1, __ATOMIC_RELEASE);
    slot->capabilities = endpoint->capabilities;
    slot->duplex = endpoint->duplex;
    slot->iq_formats = endpoint->iq_formats;
    slot->packet_formats = endpoint->packet_formats;
    slot->frequency_range_count = endpoint->frequency_range_count;
    slot->tuner_model_known=endpoint->tuner_model_known;
    slot->sample_rate_range_count = endpoint->sample_rate_range_count;
    slot->ops = *endpoint->ops;
    slot->driver_ctx = endpoint->driver_ctx;
    slot->fe_ctls = endpoint->ops->fe_ctl ? endpoint->fe_ctls : 0;
    memset(&slot->actual_iq, 0, sizeof(slot->actual_iq));
    memset(&slot->actual_packet, 0, sizeof(slot->actual_packet));
    memset(&slot->session, 0, sizeof(slot->session));
    memset(slot->owner, 0, sizeof(slot->owner));
    copy_string(slot->endpoint_id, sizeof(slot->endpoint_id),
                endpoint->endpoint_id);
    copy_string(slot->name, sizeof(slot->name), endpoint->name);
    memcpy(slot->frequency_ranges, endpoint->frequency_ranges,
           endpoint->frequency_range_count * sizeof(endpoint->frequency_ranges[0]));
    if (endpoint->sample_rate_range_count != 0)
        memcpy(slot->sample_rate_ranges, endpoint->sample_rate_ranges,
               endpoint->sample_rate_range_count *
                   sizeof(endpoint->sample_rate_ranges[0]));
    char published_id[LS_RADIO_ENDPOINT_ID_MAX];
    uint32_t published_caps = slot->capabilities;
    copy_string(published_id, sizeof(published_id), slot->endpoint_id);
    xSemaphoreGive(s_registry_lock);
    publish_endpoint_event(LS_RADIO_ENDPOINT_ATTACHED, published_id,
                           published_caps);
    return LS_RADIO_OK;
}

LS_COLD ls_radio_err_t ls_radio_endpoint_unregister(const char *endpoint_id)
{
    if (!endpoint_id || ensure_registry() != LS_RADIO_OK)
        return LS_RADIO_ERR_INVALID;
    xSemaphoreTake(s_registry_lock, portMAX_DELAY);
    endpoint_slot_t *slot = find_slot_locked(endpoint_id);
    if (!slot || !slot->present) {
        xSemaphoreGive(s_registry_lock);
        return LS_RADIO_ERR_UNAVAILABLE;
    }
    if (slot->removing) {
        xSemaphoreGive(s_registry_lock);
        return LS_RADIO_ERR_BUSY;
    }
    /* invalidate before calling cancel_read, then retain the slot and
     * driver context until every operation that passed the presence check has
     * left. This prevents detach from turning a blocked read into a UAF. */
    slot->present = false;
    slot->removing = true;
    slot->diagnostic_context_epoch = diagnostic_epoch_next();
    slot->streaming = false;
    slot->stopping = true;
    slot->last_error = LS_RADIO_ERR_DISCONNECTED;
    void (*cancel_read)(void *) = slot->ops.cancel_read;
    void *driver_ctx = slot->driver_ctx;
    xSemaphoreGive(s_registry_lock);

    if (cancel_read) cancel_read(driver_ctx);
    wait_quiesced(slot);

    char published_id[LS_RADIO_ENDPOINT_ID_MAX];
    uint32_t published_caps = slot->capabilities;
    copy_string(published_id, sizeof(published_id), slot->endpoint_id);
    xSemaphoreTake(s_registry_lock, portMAX_DELAY);
    slot->driver_ctx = NULL;
    memset(&slot->ops, 0, sizeof(slot->ops));
    slot->fe_ctls = 0;
    slot->removing = false;
    __atomic_add_fetch(&s_generation, 1, __ATOMIC_RELEASE);
    xSemaphoreGive(s_registry_lock);
    publish_endpoint_event(LS_RADIO_ENDPOINT_DETACHED, published_id,
                           published_caps);
    return LS_RADIO_OK;
}

size_t ls_radio_endpoint_count(void)
{
    if (ensure_registry() != LS_RADIO_OK) return 0;
    xSemaphoreTake(s_registry_lock, portMAX_DELAY);
    size_t count = 0;
    for (size_t i = 0; i < LS_RADIO_MAX_ENDPOINTS; ++i)
        if (s_endpoints[i].used) ++count;
    xSemaphoreGive(s_registry_lock);
    return count;
}

uint32_t ls_radio_endpoint_generation(void)
{
    return __atomic_load_n(&s_generation, __ATOMIC_ACQUIRE);
}

LS_COLD ls_radio_err_t ls_radio_endpoint_info(size_t index,
                                      ls_radio_endpoint_info_t *out)
{
    if (!out || ensure_registry() != LS_RADIO_OK)
        return LS_RADIO_ERR_INVALID;
    xSemaphoreTake(s_registry_lock, portMAX_DELAY);
    size_t current = 0;
    endpoint_slot_t *found = NULL;
    for (size_t i = 0; i < LS_RADIO_MAX_ENDPOINTS; ++i) {
        if (!s_endpoints[i].used) continue;
        if (current++ == index) {
            found = &s_endpoints[i];
            break;
        }
    }
    if (found) fill_info(found, out);
    xSemaphoreGive(s_registry_lock);
    return found ? LS_RADIO_OK : LS_RADIO_ERR_UNAVAILABLE;
}

LS_COLD ls_radio_err_t ls_radio_endpoint_get(const char *endpoint_id,
                                     ls_radio_endpoint_info_t *out)
{
    if (!endpoint_id || !out || ensure_registry() != LS_RADIO_OK)
        return LS_RADIO_ERR_INVALID;
    xSemaphoreTake(s_registry_lock, portMAX_DELAY);
    endpoint_slot_t *slot = find_slot_locked(endpoint_id);
    if (slot) fill_info(slot, out);
    xSemaphoreGive(s_registry_lock);
    return slot ? LS_RADIO_OK : LS_RADIO_ERR_UNAVAILABLE;
}

LS_COLD ls_radio_err_t ls_radio_endpoint_try_get(const char *endpoint_id,
                                         ls_radio_endpoint_info_t *out)
{
    if (!endpoint_id || !out) return LS_RADIO_ERR_INVALID;
    if (__atomic_load_n(&s_registry_init, __ATOMIC_ACQUIRE) != 2)
        return LS_RADIO_ERR_BUSY;
    if (xSemaphoreTake(s_registry_lock, 0) != pdTRUE) return LS_RADIO_ERR_BUSY;
    endpoint_slot_t *slot = find_slot_locked(endpoint_id);
    if (slot) fill_info(slot, out);
    else memset(out, 0, sizeof(*out));
    xSemaphoreGive(s_registry_lock);
    return slot ? LS_RADIO_OK : LS_RADIO_ERR_UNAVAILABLE;
}

LS_COLD bool ls_radio_endpoint_available(
    const ls_radio_requirements_t *requirements)
{
    if (!requirements || requirements->required_caps == 0 ||
        ensure_registry() != LS_RADIO_OK)
        return false;
    xSemaphoreTake(s_registry_lock, portMAX_DELAY);
    bool available = false;
    for (size_t i = 0; i < LS_RADIO_MAX_ENDPOINTS; ++i) {
        endpoint_slot_t *slot = &s_endpoints[i];
        if (requirements->preferred_endpoint_id &&
            requirements->preferred_endpoint_id[0] &&
            strcmp(slot->endpoint_id,
                   requirements->preferred_endpoint_id) != 0)
            continue;
        if (endpoint_matches(slot, requirements)) {
            available = true;
            break;
        }
    }
    xSemaphoreGive(s_registry_lock);
    return available;
}

LS_COLD int ls_radio_endpoint_subscribe(ls_radio_endpoint_event_fn callback,
                                void *user)
{
    if (!callback || ensure_registry() != LS_RADIO_OK) return -1;
    xSemaphoreTake(s_registry_lock, portMAX_DELAY);
    int result = -1;
    for (size_t i = 0; i < LS_RADIO_MAX_SUBSCRIBERS; ++i) {
        if (!s_subscribers[i].callback) {
            s_subscribers[i].callback = callback;
            s_subscribers[i].user = user;
            result = (int)i;
            break;
        }
    }
    xSemaphoreGive(s_registry_lock);
    return result;
}

LS_COLD ls_radio_err_t ls_radio_endpoint_recover(const char *endpoint_id)
{
    if (!endpoint_id || !endpoint_id[0] ||
        ensure_registry() != LS_RADIO_OK)
        return LS_RADIO_ERR_INVALID;
    xSemaphoreTake(s_registry_lock, portMAX_DELAY);
    endpoint_slot_t *slot = find_slot_locked(endpoint_id);
    if (!slot || !slot->present) {
        xSemaphoreGive(s_registry_lock);
        return LS_RADIO_ERR_UNAVAILABLE;
    }
    if (slot->leased || slot->removing) {
        xSemaphoreGive(s_registry_lock);
        return LS_RADIO_ERR_BUSY;
    }
    if (!slot->ops.recover) {
        xSemaphoreGive(s_registry_lock);
        return LS_RADIO_ERR_UNSUPPORTED;
    }
    slot->removing = true;
    slot->diagnostic_context_epoch = diagnostic_epoch_next();
    ls_radio_err_t (*recover)(void *) = slot->ops.recover;
    void *driver_ctx = slot->driver_ctx;
    xSemaphoreGive(s_registry_lock);

    xSemaphoreTake(slot->control_lock, portMAX_DELAY);
    ls_radio_err_t error = recover(driver_ctx);
    xSemaphoreGive(slot->control_lock);

    xSemaphoreTake(s_registry_lock, portMAX_DELAY);
    slot->removing = false;
    if (error != LS_RADIO_OK) slot->last_error = error;
    xSemaphoreGive(s_registry_lock);
    return error;
}

/* Front-end calls need no lease: a bias-T or a reference correction belongs
   to the device whoever is streaming from it. They count as in flight, so
   an unplug waits for them before the driver context goes, and they take
   the control lock, so they never interleave with a retune's transfers. */
LS_COLD static endpoint_slot_t *fe_enter(const char *endpoint_id, ls_radio_err_t *error)
{
    *error = LS_RADIO_OK;
    if (!endpoint_id || !endpoint_id[0] || ensure_registry() != LS_RADIO_OK) {
        *error = LS_RADIO_ERR_INVALID;
        return NULL;
    }
    xSemaphoreTake(s_registry_lock, portMAX_DELAY);
    endpoint_slot_t *slot = find_slot_locked(endpoint_id);
    if (!slot || !slot->present) *error = LS_RADIO_ERR_UNAVAILABLE;
    else if (slot->removing) *error = LS_RADIO_ERR_BUSY;
    else ++slot->in_flight;
    xSemaphoreGive(s_registry_lock);
    return *error == LS_RADIO_OK ? slot : NULL;
}

LS_COLD static void fe_leave(endpoint_slot_t *slot)
{
    xSemaphoreTake(s_registry_lock, portMAX_DELAY);
    if (slot->in_flight > 0) --slot->in_flight;
    note_quiesced_locked(slot);
    xSemaphoreGive(s_registry_lock);
}

LS_COLD ls_radio_err_t ls_radio_endpoint_fe(const char *endpoint_id,
                                    ls_radio_fe_ctl_t ctl, bool write,
                                    int *value)
{
    if (!value || (unsigned)ctl >= LS_RADIO_FE_CTLS) return LS_RADIO_ERR_INVALID;
    ls_radio_err_t error;
    endpoint_slot_t *slot = fe_enter(endpoint_id, &error);
    if (!slot) return error;
    xSemaphoreTake(slot->control_lock, portMAX_DELAY);
    if (!(slot->fe_ctls & LS_RADIO_FE_BIT(ctl)) || !slot->ops.fe_ctl)
        error = LS_RADIO_ERR_UNSUPPORTED;
    else
        error = slot->ops.fe_ctl(slot->driver_ctx, ctl, write, value);
    xSemaphoreGive(slot->control_lock);
    fe_leave(slot);
    return error;
}

LS_COLD int ls_radio_endpoint_fe_gains(const char *endpoint_id, int *tenths, int max)
{
    if (!tenths || max <= 0) return 0;
    ls_radio_err_t error;
    endpoint_slot_t *slot = fe_enter(endpoint_id, &error);
    if (!slot) return 0;
    xSemaphoreTake(slot->control_lock, portMAX_DELAY);
    int n = slot->ops.fe_gains ? slot->ops.fe_gains(slot->driver_ctx, tenths, max) : 0;
    xSemaphoreGive(slot->control_lock);
    fe_leave(slot);
    return n < 0 ? 0 : n > max ? max : n;
}

void ls_radio_endpoint_set_iq_tap(ls_radio_iq_tap_fn tap)
{
    __atomic_store_n(&s_iq_tap, tap, __ATOMIC_RELEASE);
}

LS_COLD ls_radio_err_t ls_radio_acquire(const char *owner,
                                const ls_radio_requirements_t *requirements,
                                ls_radio_session_t **out)
{
    if (!owner || !owner[0] || !requirements || !out ||
        requirements->required_caps == 0 ||
        (requirements->min_hz && requirements->max_hz &&
         requirements->min_hz > requirements->max_hz) ||
        ensure_registry() != LS_RADIO_OK)
        return LS_RADIO_ERR_INVALID;
    *out = NULL;
    xSemaphoreTake(s_registry_lock, portMAX_DELAY);
    endpoint_slot_t *choice = NULL;
    bool matched_busy = false;
    for (size_t i = 0; i < LS_RADIO_MAX_ENDPOINTS; ++i) {
        endpoint_slot_t *slot = &s_endpoints[i];
        if (requirements->preferred_endpoint_id &&
            requirements->preferred_endpoint_id[0] &&
            strcmp(slot->endpoint_id,
                   requirements->preferred_endpoint_id) != 0)
            continue;
        if (!endpoint_matches(slot, requirements)) continue;
        if (slot->leased || slot->paused) {
            matched_busy = true;
            continue;
        }
        choice = slot;
        break;
    }
    if (!choice) {
        xSemaphoreGive(s_registry_lock);
        return matched_busy ? LS_RADIO_ERR_BUSY : LS_RADIO_ERR_UNAVAILABLE;
    }
    choice->leased = true;
    choice->diagnostic_context_epoch = diagnostic_epoch_next();
    choice->session.endpoint = choice;
    choice->session.active = true;
    choice->session.closing = false;
    choice->session.requirements = *requirements;
    choice->session.requirements.preferred_endpoint_id = NULL;
    copy_string(choice->owner, sizeof(choice->owner), owner);
    *out = &choice->session;
    xSemaphoreGive(s_registry_lock);
    return LS_RADIO_OK;
}

LS_COLD void ls_radio_release(ls_radio_session_t *session)
{
    if (!session || ensure_registry() != LS_RADIO_OK) return;
    xSemaphoreTake(s_registry_lock, portMAX_DELAY);
    endpoint_slot_t *slot = session->endpoint;
    if (!session->active || session->closing || !slot) {
        xSemaphoreGive(s_registry_lock);
        return;
    }
    session->closing = true;
    slot->diagnostic_context_epoch = diagnostic_epoch_next();
    slot->stopping = true;
    bool present = slot->present;
    void (*cancel_read)(void *) = slot->ops.cancel_read;
    ls_radio_err_t (*stop)(void *) = slot->stream_kind == 2
                                         ? slot->ops.packet_rx_stop
                                         : slot->ops.iq_stop;
    void *driver_ctx = slot->driver_ctx;
    bool was_streaming = slot->streaming;
    if (present) ++slot->in_flight;
    xSemaphoreGive(s_registry_lock);

    if (present && cancel_read) cancel_read(driver_ctx);
    if (present && stop && was_streaming) {
        xSemaphoreTake(slot->control_lock, portMAX_DELAY);
        ls_radio_err_t error = stop(driver_ctx);
        xSemaphoreGive(slot->control_lock);
        if (error != LS_RADIO_OK) {
            xSemaphoreTake(s_registry_lock, portMAX_DELAY);
            slot->last_error = error;
            xSemaphoreGive(s_registry_lock);
        }
    }
    if (present) {
        xSemaphoreTake(s_registry_lock, portMAX_DELAY);
        if (slot->in_flight > 0) --slot->in_flight;
        note_quiesced_locked(slot);
        xSemaphoreGive(s_registry_lock);
    }
    wait_quiesced(slot);

    xSemaphoreTake(s_registry_lock, portMAX_DELAY);
    slot->leased = false;
    slot->streaming = false;
    slot->stopping = false;
    slot->stream_kind = 0;
    slot->owner[0] = '\0';
    session->active = false;
    session->closing = false;
    session->endpoint = NULL;
    xSemaphoreGive(s_registry_lock);
}

LS_COLD ls_radio_err_t ls_radio_iq_configure(ls_radio_session_t *session,
                                     const ls_radio_iq_config_t *requested,
                                     ls_radio_iq_config_t *actual)
{
    if (!requested || !actual ||
        (requested->gain_mode != LS_RADIO_GAIN_AUTO &&
         requested->gain_mode != LS_RADIO_GAIN_MANUAL))
        return LS_RADIO_ERR_INVALID;
    endpoint_slot_t *slot;
    ls_radio_err_t error = session_enter(session, LS_RADIO_RX_IQ_U8,
                                         false, &slot);
    if (error != LS_RADIO_OK) return error;
    xSemaphoreTake(slot->control_lock, portMAX_DELAY);
    xSemaphoreTake(s_registry_lock, portMAX_DELAY);
    if (!slot->present)
        error = LS_RADIO_ERR_DISCONNECTED;
    else if (!value_in_ranges(requested->center_hz, slot->frequency_ranges,
                              slot->frequency_range_count) ||
             !value_in_ranges(requested->sample_rate_hz,
                              slot->sample_rate_ranges,
                              slot->sample_rate_range_count))
        error = LS_RADIO_ERR_UNSUPPORTED;
    else if (slot->streaming)
        error = LS_RADIO_ERR_BUSY;
    else if (!slot->ops.iq_configure)
        error = LS_RADIO_ERR_UNSUPPORTED;
    xSemaphoreGive(s_registry_lock);
    if (error == LS_RADIO_OK)
        error = slot->ops.iq_configure(slot->driver_ctx, requested, actual);
    if (error == LS_RADIO_OK) {
        xSemaphoreTake(s_registry_lock, portMAX_DELAY);
        if (slot->present) {
            slot->actual_iq = *actual;
            slot->configured = true;
            slot->config_kind = 1;
        }
        xSemaphoreGive(s_registry_lock);
    }
    error = session_leave_control(slot, error);
    xSemaphoreGive(slot->control_lock);
    return error;
}

LS_COLD ls_radio_err_t ls_radio_iq_set_gain(ls_radio_session_t *session,
                                    ls_radio_gain_mode_t mode,
                                    int tenths_db,
                                    int *actual_tenths_db)
{
    if (!actual_tenths_db ||
        (mode != LS_RADIO_GAIN_AUTO && mode != LS_RADIO_GAIN_MANUAL))
        return LS_RADIO_ERR_INVALID;
    endpoint_slot_t *slot;
    ls_radio_err_t error = session_enter(session, LS_RADIO_RX_IQ_U8,
                                         false, &slot);
    if (error != LS_RADIO_OK) return error;
    xSemaphoreTake(slot->control_lock, portMAX_DELAY);
    xSemaphoreTake(s_registry_lock, portMAX_DELAY);
    if (!slot->present)
        error = LS_RADIO_ERR_DISCONNECTED;
    else if (!slot->ops.iq_set_gain)
        error = LS_RADIO_ERR_UNSUPPORTED;
    xSemaphoreGive(s_registry_lock);
    if (error == LS_RADIO_OK)
        error = slot->ops.iq_set_gain(slot->driver_ctx, mode, tenths_db,
                                      actual_tenths_db);
    if (error == LS_RADIO_OK) {
        xSemaphoreTake(s_registry_lock, portMAX_DELAY);
        if (slot->present) {
            slot->actual_iq.gain_mode = mode;
            slot->actual_iq.gain_tenths_db = *actual_tenths_db;
        }
        xSemaphoreGive(s_registry_lock);
    }
    error = session_leave_control(slot, error);
    xSemaphoreGive(slot->control_lock);
    return error;
}

LS_COLD ls_radio_err_t ls_radio_iq_set_ppm(ls_radio_session_t *session, int ppm)
{
    if (ppm < -200 || ppm > 200) return LS_RADIO_ERR_INVALID;
    endpoint_slot_t *slot;
    ls_radio_err_t error = session_enter(session, LS_RADIO_RX_IQ_U8,
                                         false, &slot);
    if (error != LS_RADIO_OK) return error;
    xSemaphoreTake(slot->control_lock, portMAX_DELAY);
    xSemaphoreTake(s_registry_lock, portMAX_DELAY);
    if (!slot->present)
        error = LS_RADIO_ERR_DISCONNECTED;
    else if (slot->streaming)
        error = LS_RADIO_ERR_BUSY;
    else if (!slot->ops.iq_set_ppm)
        error = LS_RADIO_ERR_UNSUPPORTED;
    xSemaphoreGive(s_registry_lock);
    if (error == LS_RADIO_OK)
        error = slot->ops.iq_set_ppm(slot->driver_ctx, ppm);
    error = session_leave_control(slot, error);
    xSemaphoreGive(slot->control_lock);
    return error;
}

LS_COLD ls_radio_err_t ls_radio_iq_start(ls_radio_session_t *session)
{
    endpoint_slot_t *slot;
    ls_radio_err_t error = session_enter(session, LS_RADIO_RX_IQ_U8,
                                         false, &slot);
    if (error != LS_RADIO_OK) return error;
    xSemaphoreTake(slot->control_lock, portMAX_DELAY);
    xSemaphoreTake(s_registry_lock, portMAX_DELAY);
    if (!slot->present)
        error = LS_RADIO_ERR_DISCONNECTED;
    else if (slot->paused)
        error = LS_RADIO_ERR_STOPPED;
    else if (!slot->configured || slot->config_kind != 1)
        error = LS_RADIO_ERR_INVALID;
    else if (slot->streaming)
        error = LS_RADIO_ERR_BUSY;
    else if (!slot->ops.iq_start)
        error = LS_RADIO_ERR_UNSUPPORTED;
    xSemaphoreGive(s_registry_lock);
    if (error == LS_RADIO_OK)
        error = slot->ops.iq_start(slot->driver_ctx);
    if (error == LS_RADIO_OK) {
        xSemaphoreTake(s_registry_lock, portMAX_DELAY);
        if (slot->present) {
            slot->streaming = true;
            slot->stopping = false;
            slot->stream_kind = 1;
        }
        xSemaphoreGive(s_registry_lock);
    }
    error = session_leave_control(slot, error);
    xSemaphoreGive(slot->control_lock);
    return error;
}

static ls_radio_iq_capture_fn s_capture_tap;

void ls_radio_endpoint_set_capture_tap(ls_radio_iq_capture_fn tap)
{
    __atomic_store_n(&s_capture_tap, tap, __ATOMIC_RELEASE);
}

ls_radio_err_t ls_radio_iq_read(ls_radio_session_t *session, void *dst,
                                size_t bytes, uint32_t timeout_ms,
                                size_t *read_bytes)
{
    return ls_radio_iq_read_observed(session,dst,bytes,timeout_ms,read_bytes,NULL);
}

ls_radio_err_t ls_radio_iq_read_observed(ls_radio_session_t *session,void *dst,
    size_t bytes,uint32_t timeout_ms,size_t *read_bytes,ls_radio_iq_read_receipt_t *r)
{
    if(r)memset(r,0,sizeof(*r));
    if (!dst || bytes == 0 || !read_bytes) return LS_RADIO_ERR_INVALID;
    *read_bytes = 0;
    endpoint_slot_t *slot;
    ls_radio_err_t error = session_enter(session, LS_RADIO_RX_IQ_U8,
                                         true, &slot);
    if (error != LS_RADIO_OK) return error;
    const ls_radio_iq_capture_fn cap = __atomic_load_n(&s_capture_tap, __ATOMIC_ACQUIRE);
    ls_radio_iq_config_t tuned;
    xSemaphoreTake(s_registry_lock, portMAX_DELAY);
    bool iq_stream = slot->stream_kind == 1;
    uint64_t context=slot->diagnostic_context_epoch;
    bool stable=slot->diagnostic_controls==0 && !slot->stopping && context!=UINT64_MAX;
    if(r){
        r->context_epoch=context;r->configured_center_hz=slot->actual_iq.center_hz;
        r->nominal_rate_hz=slot->actual_iq.sample_rate_hz;
    }
    const uint64_t center_hz = slot->actual_iq.center_hz;
    const uint32_t rate_hz = slot->actual_iq.sample_rate_hz;
    if (cap) tuned = slot->actual_iq;
    xSemaphoreGive(s_registry_lock);
    ls_radio_iq_transport_t before={0};
    if(r && slot->ops.iq_transport_status)
        (void)slot->ops.iq_transport_status(slot->driver_ctx,&before);
    if(r)r->read_started_us=(uint64_t)esp_timer_get_time();
    if (!iq_stream)
        error = LS_RADIO_ERR_BUSY;
    else if (!slot->ops.iq_read)
        error = LS_RADIO_ERR_UNSUPPORTED;
    else
        error = slot->ops.iq_read(slot->driver_ctx, dst, bytes, timeout_ms,
                                  read_bytes);
    if(r){
        r->read_finished_us=(uint64_t)esp_timer_get_time();
        if(slot->ops.iq_transport_status)
            (void)slot->ops.iq_transport_status(slot->driver_ctx,&r->transport);
        xSemaphoreTake(s_registry_lock,portMAX_DELAY);
        r->known=stable && slot->present && !slot->stopping && !slot->diagnostic_controls &&
            context==slot->diagnostic_context_epoch && before.known && r->transport.known &&
            before.epoch==r->transport.epoch && r->transport.epoch!=UINT64_MAX &&
            error==LS_RADIO_OK && *read_bytes && !(*read_bytes&1u);
        xSemaphoreGive(s_registry_lock);
    }
    /* RF LAB's observer, then TIME MACHINE's copy; neither blocks. */
    ls_radio_iq_tap_fn tap = __atomic_load_n(&s_iq_tap, __ATOMIC_ACQUIRE);
    if (tap && error == LS_RADIO_OK && *read_bytes)
        tap(slot->endpoint_id, (const uint8_t *)dst, *read_bytes, center_hz, rate_hz);
    if (cap && error == LS_RADIO_OK && *read_bytes)
        cap(slot->endpoint_id, &tuned, dst, *read_bytes);
    return session_leave(slot,error,true,*read_bytes,0);
}

bool ls_radio_iq_receipt_current(ls_radio_session_t *session,
    const ls_radio_iq_read_receipt_t *r)
{
    if(!r || !r->known)return false;
    endpoint_slot_t *slot;
    if(session_enter(session,LS_RADIO_RX_IQ_U8,true,&slot)!=LS_RADIO_OK)return false;
    ls_radio_iq_transport_t transport={0};
    if(slot->ops.iq_transport_status)
        (void)slot->ops.iq_transport_status(slot->driver_ctx,&transport);
    xSemaphoreTake(s_registry_lock,portMAX_DELAY);
    bool current=slot->present && !slot->stopping && !slot->diagnostic_controls &&
        slot->diagnostic_context_epoch==r->context_epoch && transport.known &&
        transport.fifo_reset_known && transport.epoch==r->transport.epoch &&
        !transport.counters_saturated;
    if(slot->in_flight>0)--slot->in_flight;
    note_quiesced_locked(slot);
    xSemaphoreGive(s_registry_lock);
    return current;
}

LS_COLD ls_radio_err_t ls_radio_iq_retune(ls_radio_session_t *session,
                                  uint64_t center_hz, bool fast,
                                  uint64_t *actual_hz)
{
    if (!actual_hz) return LS_RADIO_ERR_INVALID;
    endpoint_slot_t *slot;
    ls_radio_err_t error = session_enter(session, LS_RADIO_RX_IQ_U8,
                                         false, &slot);
    if (error != LS_RADIO_OK) return error;
    xSemaphoreTake(slot->control_lock, portMAX_DELAY);
    xSemaphoreTake(s_registry_lock, portMAX_DELAY);
    if (!slot->present)
        error = LS_RADIO_ERR_DISCONNECTED;
    else if (slot->streaming && slot->stream_kind != 1)
        error = LS_RADIO_ERR_BUSY;
    else if (!value_in_ranges(center_hz, slot->frequency_ranges,
                              slot->frequency_range_count))
        error = LS_RADIO_ERR_UNSUPPORTED;
    else if (!slot->ops.iq_retune)
        error = LS_RADIO_ERR_UNSUPPORTED;
    xSemaphoreGive(s_registry_lock);
    if (error == LS_RADIO_OK)
        error = slot->ops.iq_retune(slot->driver_ctx, center_hz, fast,
                                    actual_hz);
    if (error == LS_RADIO_OK) {
        xSemaphoreTake(s_registry_lock, portMAX_DELAY);
        if (slot->present)
            slot->actual_iq.center_hz = *actual_hz;
        xSemaphoreGive(s_registry_lock);
    }
    error = session_leave_control(slot, error);
    xSemaphoreGive(slot->control_lock);
    return error;
}

LS_COLD ls_radio_err_t ls_radio_iq_get_health(ls_radio_session_t *session,
                                      ls_radio_iq_health_t *out)
{
    if (!out) return LS_RADIO_ERR_INVALID;
    memset(out, 0, sizeof(*out));
    endpoint_slot_t *slot;
    ls_radio_err_t error = session_enter(session, LS_RADIO_RX_IQ_U8,
                                         false, &slot);
    if (error != LS_RADIO_OK) return error;
    xSemaphoreTake(slot->control_lock, portMAX_DELAY);
    xSemaphoreTake(s_registry_lock, portMAX_DELAY);
    if (!slot->present) error = LS_RADIO_ERR_DISCONNECTED;
    else if (slot->streaming) error = LS_RADIO_ERR_BUSY;
    else if (!slot->ops.iq_get_health) error = LS_RADIO_ERR_UNSUPPORTED;
    xSemaphoreGive(s_registry_lock);
    if (error == LS_RADIO_OK)
        error = slot->ops.iq_get_health(slot->driver_ctx, out);
    error = session_leave(slot, error, false, 0, 0);
    xSemaphoreGive(slot->control_lock);
    return error;
}

LS_COLD ls_radio_err_t ls_radio_iq_stop(ls_radio_session_t *session)
{
    endpoint_slot_t *slot;
    ls_radio_err_t error = session_enter(session, LS_RADIO_RX_IQ_U8,
                                         false, &slot);
    if (error != LS_RADIO_OK) return error;
    xSemaphoreTake(slot->control_lock, portMAX_DELAY);
    xSemaphoreTake(s_registry_lock, portMAX_DELAY);
    bool was_streaming = slot->present && slot->streaming &&
                         slot->stream_kind == 1;
    if (!slot->present)
        error = LS_RADIO_ERR_DISCONNECTED;
    else if (slot->streaming && slot->stream_kind != 1)
        error = LS_RADIO_ERR_BUSY;
    else
        slot->stopping = true;
    xSemaphoreGive(s_registry_lock);
    if (error != LS_RADIO_OK) {
        /* Removal owns cancellation once present has been cleared. */
    } else if (!was_streaming)
        error = LS_RADIO_OK;
    else {
        if (slot->ops.cancel_read) slot->ops.cancel_read(slot->driver_ctx);
        if (!slot->ops.iq_stop)
            error = LS_RADIO_ERR_UNSUPPORTED;
        else
            error = slot->ops.iq_stop(slot->driver_ctx);
    }
    xSemaphoreTake(s_registry_lock, portMAX_DELAY);
    if (slot->present) {
        if (error == LS_RADIO_OK) {
            slot->streaming = false;
            slot->stream_kind = 0;
        }
        slot->stopping = false;
    }
    xSemaphoreGive(s_registry_lock);
    /* The stream has ended: what comes next is not contiguous with it. */
    const ls_radio_iq_capture_fn cap = __atomic_load_n(&s_capture_tap, __ATOMIC_ACQUIRE);
    if (cap && was_streaming) cap(slot->endpoint_id, NULL, NULL, 0);
    error = session_leave(slot, error, false, 0, 0);
    xSemaphoreGive(slot->control_lock);
    return error;
}

LS_COLD ls_radio_err_t ls_radio_packet_rx_configure(
    ls_radio_session_t *session,
    const ls_radio_packet_config_t *requested,
    ls_radio_packet_config_t *actual)
{
    if (!requested || !actual || requested->center_hz == 0 ||
        requested->format == 0 ||
        requested->sync_word_bytes > LS_RADIO_PACKET_SYNC_MAX)
        return LS_RADIO_ERR_INVALID;
    endpoint_slot_t *slot;
    ls_radio_err_t error = session_enter(session, LS_RADIO_RX_PACKET,
                                         false, &slot);
    if (error != LS_RADIO_OK) return error;
    xSemaphoreTake(slot->control_lock, portMAX_DELAY);
    xSemaphoreTake(s_registry_lock, portMAX_DELAY);
    if (!slot->present)
        error = LS_RADIO_ERR_DISCONNECTED;
    else if (slot->streaming)
        error = LS_RADIO_ERR_BUSY;
    else if (!value_in_ranges(requested->center_hz,
                              slot->frequency_ranges,
                              slot->frequency_range_count) ||
             (slot->packet_formats & requested->format) !=
                 requested->format)
        error = LS_RADIO_ERR_UNSUPPORTED;
    else if (!slot->ops.packet_rx_configure)
        error = LS_RADIO_ERR_UNSUPPORTED;
    xSemaphoreGive(s_registry_lock);
    if (error == LS_RADIO_OK)
        error = slot->ops.packet_rx_configure(slot->driver_ctx, requested,
                                              actual);
    if (error == LS_RADIO_OK) {
        xSemaphoreTake(s_registry_lock, portMAX_DELAY);
        if (slot->present) {
            slot->actual_packet = *actual;
            slot->configured = true;
            slot->config_kind = 2;
        }
        xSemaphoreGive(s_registry_lock);
    }
    error = session_leave(slot, error, false, 0, 0);
    xSemaphoreGive(slot->control_lock);
    return error;
}

LS_COLD ls_radio_err_t ls_radio_packet_rx_start(ls_radio_session_t *session)
{
    endpoint_slot_t *slot;
    ls_radio_err_t error = session_enter(session, LS_RADIO_RX_PACKET,
                                         false, &slot);
    if (error != LS_RADIO_OK) return error;
    xSemaphoreTake(slot->control_lock, portMAX_DELAY);
    xSemaphoreTake(s_registry_lock, portMAX_DELAY);
    if (!slot->present)
        error = LS_RADIO_ERR_DISCONNECTED;
    else if (slot->paused)
        error = LS_RADIO_ERR_STOPPED;
    else if (!slot->configured || slot->config_kind != 2)
        error = LS_RADIO_ERR_INVALID;
    else if (slot->streaming)
        error = LS_RADIO_ERR_BUSY;
    else if (!slot->ops.packet_rx_start)
        error = LS_RADIO_ERR_UNSUPPORTED;
    xSemaphoreGive(s_registry_lock);
    if (error == LS_RADIO_OK)
        error = slot->ops.packet_rx_start(slot->driver_ctx);
    if (error == LS_RADIO_OK) {
        xSemaphoreTake(s_registry_lock, portMAX_DELAY);
        if (slot->present) {
            slot->streaming = true;
            slot->stopping = false;
            slot->stream_kind = 2;
        }
        xSemaphoreGive(s_registry_lock);
    }
    error = session_leave(slot, error, false, 0, 0);
    xSemaphoreGive(slot->control_lock);
    return error;
}

ls_radio_err_t ls_radio_packet_rx_read(ls_radio_session_t *session,
                                       ls_radio_packet_t *packet,
                                       uint32_t timeout_ms)
{
    if (!packet || !packet->data || packet->capacity == 0)
        return LS_RADIO_ERR_INVALID;
    packet->bytes = 0;
    endpoint_slot_t *slot;
    ls_radio_err_t error = session_enter(session, LS_RADIO_RX_PACKET,
                                         true, &slot);
    if (error != LS_RADIO_OK) return error;
    xSemaphoreTake(s_registry_lock, portMAX_DELAY);
    bool packet_stream = slot->stream_kind == 2;
    xSemaphoreGive(s_registry_lock);
    if (!packet_stream)
        error = LS_RADIO_ERR_BUSY;
    else if (!slot->ops.packet_rx_read)
        error = LS_RADIO_ERR_UNSUPPORTED;
    else
        error = slot->ops.packet_rx_read(slot->driver_ctx, packet,
                                         timeout_ms);
    return session_leave(slot, error, true, packet->bytes,
                         error == LS_RADIO_OK ? 1 : 0);
}

LS_COLD ls_radio_err_t ls_radio_packet_rx_stop(ls_radio_session_t *session)
{
    endpoint_slot_t *slot;
    ls_radio_err_t error = session_enter(session, LS_RADIO_RX_PACKET,
                                         false, &slot);
    if (error != LS_RADIO_OK) return error;
    xSemaphoreTake(slot->control_lock, portMAX_DELAY);
    xSemaphoreTake(s_registry_lock, portMAX_DELAY);
    bool was_streaming = slot->present && slot->streaming &&
                         slot->stream_kind == 2;
    if (!slot->present)
        error = LS_RADIO_ERR_DISCONNECTED;
    else if (slot->streaming && slot->stream_kind != 2)
        error = LS_RADIO_ERR_BUSY;
    else
        slot->stopping = true;
    xSemaphoreGive(s_registry_lock);
    if (error == LS_RADIO_OK && was_streaming) {
        if (slot->ops.cancel_read) slot->ops.cancel_read(slot->driver_ctx);
        if (!slot->ops.packet_rx_stop)
            error = LS_RADIO_ERR_UNSUPPORTED;
        else
            error = slot->ops.packet_rx_stop(slot->driver_ctx);
    }
    xSemaphoreTake(s_registry_lock, portMAX_DELAY);
    if (slot->present) {
        if (error == LS_RADIO_OK) {
            slot->streaming = false;
            slot->stream_kind = 0;
        }
        slot->stopping = false;
    }
    xSemaphoreGive(s_registry_lock);
    error = session_leave(slot, error, false, 0, 0);
    xSemaphoreGive(slot->control_lock);
    return error;
}

LS_COLD ls_radio_err_t ls_radio_endpoint_pause(const char *endpoint_id,
                                      const char *owner, bool paused)
{
    if (!endpoint_id || !owner || ensure_registry() != LS_RADIO_OK)
        return LS_RADIO_ERR_INVALID;
    xSemaphoreTake(s_registry_lock, portMAX_DELAY);
    endpoint_slot_t *slot = find_slot_locked(endpoint_id);
    if (!slot || !slot->present) {
        xSemaphoreGive(s_registry_lock);
        return LS_RADIO_ERR_UNAVAILABLE;
    }
    if (strcmp(slot->owner, owner) || slot->removing || slot->session.closing || slot->pause_pending) {
        xSemaphoreGive(s_registry_lock);
        return LS_RADIO_ERR_BUSY;
    }
    ++slot->in_flight;
    slot->pause_pending = true;
    ++slot->diagnostic_controls;
    slot->diagnostic_context_epoch = diagnostic_epoch_next();
    slot->paused = paused;
    xSemaphoreGive(s_registry_lock);
    xSemaphoreTake(slot->control_lock, portMAX_DELAY);
    xSemaphoreTake(s_registry_lock, portMAX_DELAY);
    const bool streaming = slot->present && slot->streaming;
    ls_radio_err_t (*stop)(void *) = slot->stream_kind == 2 ?
        slot->ops.packet_rx_stop : slot->ops.iq_stop;
    slot->stopping = paused;
    xSemaphoreGive(s_registry_lock);
    ls_radio_err_t error = LS_RADIO_OK;
    if (paused && streaming) {
        if (slot->ops.cancel_read) slot->ops.cancel_read(slot->driver_ctx);
        error = stop ? stop(slot->driver_ctx) : LS_RADIO_ERR_UNSUPPORTED;
    }
    xSemaphoreTake(s_registry_lock, portMAX_DELAY);
    if (error == LS_RADIO_OK && paused) {
        slot->streaming = false;
        slot->stream_kind = 0;
    }
    if (error != LS_RADIO_OK) slot->paused = false;
    slot->pause_pending = false;
    slot->stopping = false;
    xSemaphoreGive(s_registry_lock);
    error = session_leave(slot, error, false, 0, 0);
    xSemaphoreGive(slot->control_lock);
    return error;
}

LS_COLD const char *ls_radio_err_name(ls_radio_err_t error)
{
    switch (error) {
    case LS_RADIO_OK:               return "ok";
    case LS_RADIO_ERR_INVALID:      return "invalid";
    case LS_RADIO_ERR_UNAVAILABLE:  return "unavailable";
    case LS_RADIO_ERR_BUSY:         return "busy";
    case LS_RADIO_ERR_DISCONNECTED: return "disconnected";
    case LS_RADIO_ERR_UNSUPPORTED:  return "unsupported";
    case LS_RADIO_ERR_IO:           return "io";
    case LS_RADIO_ERR_TIMEOUT:      return "timeout";
    case LS_RADIO_ERR_STOPPED:      return "stopped";
    case LS_RADIO_ERR_NO_MEMORY:    return "no-memory";
    case LS_RADIO_ERR_EXISTS:       return "exists";
    default:                        return "unknown";
    }
}

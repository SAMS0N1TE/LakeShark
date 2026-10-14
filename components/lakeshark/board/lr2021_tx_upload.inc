/* Bounded, amplitude-preserving LR2021 I/Q playback. Included by the LR20xx
 * board driver so it can share the radio lock and established recovery path. */
#define LRTX_UPLOAD_MAX_BYTES 16384u
#define LRTX_MAX_SAMPLES 32767u
#define LRTX_FIFO_BYTES 1024u

/* The mesh component depends on this board component, so keep the ownership
 * query weak and avoid introducing a reverse build dependency. */
extern bool ls_mesh_radio_wanted(void) __attribute__((weak));
extern bool ls_mesh_radio_held(void) __attribute__((weak));
extern bool ls_mesh_radio_bg(void) __attribute__((weak));
extern bool ls_field_owned(void) __attribute__((weak));

static uint8_t *s_lrtx_data;
static uint32_t s_lrtx_size, s_lrtx_next, s_lrtx_crc;
static bool s_lrtx_ready, s_lrtx_sending;
static uint32_t s_lrtx_cancel_generation;

void ls_lora_lr20xx_tx_cancel_request(void)
{
    __atomic_add_fetch(&s_lrtx_cancel_generation, 1u, __ATOMIC_RELEASE);
}

static bool lrtx_cancelled(uint32_t generation)
{
    return __atomic_load_n(&s_lrtx_cancel_generation, __ATOMIC_ACQUIRE) != generation;
}

static uint32_t lrtx_crc32(const uint8_t *data, size_t size)
{
    uint32_t crc = UINT32_MAX;
    while (size--) {
        crc ^= *data++;
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((0u - (crc & 1u)) & 0xEDB88320u);
    }
    return ~crc;
}

static int lrtx_hex(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool lrtx_radio_owned(void)
{
    return lr.tx_busy || lr.scanning || lr.fsk_active || ms.active ||
           s_exp_active || lr20xx_modes_active() ||
           lr20xx_stream_source2_active() || s_lrtx_sending;
}

static esp_err_t lrtx_enqueue(const uint8_t *frame, unsigned frame_len,
                              unsigned total, unsigned *queued,
                              unsigned limit, uint32_t cancel_generation)
{
    if (!frame || !frame_len || (frame_len & 1u) || !queued || (*queued & 1u) ||
        *queued > total) return ESP_ERR_INVALID_ARG;
    if (limit > total - *queued) limit = total - *queued;
    uint8_t staging[62];
    while (limit) {
        if (lrtx_cancelled(cancel_generation)) return ESP_ERR_INVALID_STATE;
        unsigned count = frame_len - (*queued % frame_len);
        if (count > limit) count = limit;
        /* The ESP32-P4 CPU SPI helper bounds one transaction to 64 total
         * bytes, including opcode/address. Keep each FIFO payload <=62. */
        if (count > 62u) count = 62u;
        count &= ~1u;
        if (!count) return ESP_ERR_INVALID_ARG;
        memcpy(staging, frame + (*queued % frame_len), count);
        esp_err_t err = lr20xx_write_tx_fifo(staging, count);
        if (err != ESP_OK) return err;
        /* TX FIFO writes do not need a follow-up command: validate the Stat
         * already clocked back with the last SPI frame before advancing. */
        if (!read_stat_ok()) return ESP_ERR_INVALID_RESPONSE;
        *queued += count;
        limit -= count;
    }
    return ESP_OK;
}

static int lrtx_upload_command(int argc, char **argv)
{
    if (argc == 3 && !strcmp(argv[2], "status")) {
        if (!ls_lora_hw_try_lock()) {
            puts("lrtx-upload: busy");
            return 1;
        }
        const bool ready = s_lrtx_ready, allocated = s_lrtx_data != NULL;
        const uint32_t size = s_lrtx_size, next = s_lrtx_next;
        ls_lora_hw_unlock();
        printf("lrtx-upload: ready=%u bytes=%lu next=%lu allocated=%u\n",
               ready, (unsigned long)size, (unsigned long)next, allocated);
        return 0;
    }

    if (!ls_lora_hw_try_lock()) {
        puts("lrtx-upload: busy");
        return 1;
    }
    if (s_lrtx_sending || lr20xx_stream_source2_active()) {
        ls_lora_hw_unlock();
        puts("lrtx-upload: error radio transaction active");
        return 1;
    }
    if (argc == 3 && !strcmp(argv[2], "abort")) {
        heap_caps_free(s_lrtx_data);
        s_lrtx_data = NULL;
        s_lrtx_size = s_lrtx_next = s_lrtx_crc = 0;
        s_lrtx_ready = false;
        ls_lora_hw_unlock();
        puts("lrtx-upload: aborted");
        return 0;
    }
    if (argc == 5 && !strcmp(argv[2], "begin")) {
        uint32_t size = 0, crc = 0;
        s_lrtx_ready = false;
        s_lrtx_size = s_lrtx_next = s_lrtx_crc = 0;
        heap_caps_free(s_lrtx_data);
        s_lrtx_data = NULL;
        if (!probe_number(argv[3], LRTX_UPLOAD_MAX_BYTES, &size) || size < 2 ||
            (size & 1u) || !probe_number(argv[4], UINT32_MAX, &crc)) {
            ls_lora_hw_unlock();
            puts("lrtx-upload: error invalid size or CRC");
            return 1;
        }
        s_lrtx_data = heap_caps_malloc(size,
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_lrtx_data) {
            ls_lora_hw_unlock();
            puts("lrtx-upload: error PSRAM allocation failed");
            return 1;
        }
        s_lrtx_size = size;
        s_lrtx_crc = crc;
        ls_lora_hw_unlock();
        printf("lrtx-upload: next=0 size=%lu\n", (unsigned long)size);
        return 0;
    }
    if (argc == 5 && !strcmp(argv[2], "chunk")) {
        uint32_t offset = 0;
        const size_t hex_len = strlen(argv[4]);
        uint8_t chunk[64];
        if (!s_lrtx_data || s_lrtx_ready || !probe_number(argv[3], LRTX_UPLOAD_MAX_BYTES, &offset) ||
            offset != s_lrtx_next || !hex_len || hex_len > sizeof(chunk) * 2u ||
            (hex_len & 3u) || hex_len / 2u > s_lrtx_size - s_lrtx_next) {
            ls_lora_hw_unlock();
            puts("lrtx-upload: error invalid or out-of-order chunk");
            return 1;
        }
        const size_t bytes = hex_len / 2u;
        for (size_t i = 0; i < bytes; ++i) {
            int hi = lrtx_hex(argv[4][2u * i]);
            int lo = lrtx_hex(argv[4][2u * i + 1u]);
            if (hi < 0 || lo < 0) {
                ls_lora_hw_unlock();
                puts("lrtx-upload: error malformed hex");
                return 1;
            }
            chunk[i] = (uint8_t)((hi << 4) | lo);
        }
        memcpy(s_lrtx_data + offset, chunk, bytes);
        s_lrtx_next += (uint32_t)bytes;
        const uint32_t next = s_lrtx_next;
        ls_lora_hw_unlock();
        printf("lrtx-upload: next=%lu\n", (unsigned long)next);
        return 0;
    }
    if (argc == 3 && !strcmp(argv[2], "commit")) {
        if (!s_lrtx_data || !s_lrtx_size || s_lrtx_next != s_lrtx_size ||
            lrtx_crc32(s_lrtx_data, s_lrtx_size) != s_lrtx_crc) {
            s_lrtx_ready = false;
            ls_lora_hw_unlock();
            puts("lrtx-upload: error incomplete upload or CRC mismatch");
            return 1;
        }
        for (uint32_t i = 0; i < s_lrtx_size; i += 2) {
            const int i_sample = (int8_t)s_lrtx_data[i];
            const int q_sample = (int8_t)s_lrtx_data[i + 1];
            if (i_sample * i_sample + q_sample * q_sample > 56 * 56) {
                s_lrtx_ready = false;
                ls_lora_hw_unlock();
                puts("lrtx-upload: error magnitude exceeds 56");
                return 1;
            }
        }
        s_lrtx_ready = true;
        const uint32_t size = s_lrtx_size, crc = s_lrtx_crc;
        ls_lora_hw_unlock();
        printf("lrtx-upload: ready bytes=%lu crc32=%08lX\n",
               (unsigned long)size, (unsigned long)crc);
        return 0;
    }
    ls_lora_hw_unlock();
    puts("usage: lora lrtx-upload begin <bytes> <crc32> | chunk <offset> <hex> | commit | status | abort");
    return 1;
}

static bool lrtx_rate_supported(uint32_t rate)
{
    return rate == 4800 || rate == 9600 || rate == 19200 || rate == 38400 ||
           rate == 76800 || rate == 125000;
}

static esp_err_t lrtx_send(const uint8_t *data, uint32_t size, uint32_t rate,
                            uint32_t repeats, uint32_t hz, int dbm, const char **reason)
{
    const char *why = NULL;
    if (reason) *reason = NULL;
#define REFUSE(code, message) do { if (reason) *reason = (message); puts("lrtx-send: " message); return (code); } while (0)
    if (!lrtx_rate_supported(rate)) REFUSE(ESP_ERR_INVALID_ARG, "unsupported sample rate");
    if (!repeats || repeats>16) REFUSE(ESP_ERR_INVALID_ARG, "repeat count outside 1..16");
    if (hz<150000000u || hz>960000000u) REFUSE(ESP_ERR_INVALID_ARG, "frequency outside LF TX band");
    if (dbm<-9 || dbm>10) REFUSE(ESP_ERR_INVALID_ARG, "power outside -9..10 dBm");
    const bool leased = data && ls_mesh_radio_held && ls_mesh_radio_held();
    const uint32_t cancel_generation = __atomic_load_n(&s_lrtx_cancel_generation,
                                                        __ATOMIC_ACQUIRE);
    unsigned total_bytes = 0, sample_count = 0, queued = 0, min_level = LRTX_FIFO_BYTES;
    unsigned refills = 0;
    int64_t duration_us = 0, max_poll_gap = 0, previous_poll = 0;
    uint16_t errors = 0, final_errors = 0;
    uint8_t flags[2] = {0}, level[2] = {0};
    uint32_t irq = 0;
    const char *phase = "preflight";
    esp_err_t err, stop = ESP_OK, recovery = ESP_OK;
    bool touched = false, fast = false, priority_changed = false, rx = false;
    const UBaseType_t original_priority = uxTaskPriorityGet(NULL);

    if (!ls_lora_hw_try_lock()) {
        REFUSE(ESP_ERR_INVALID_STATE, "radio lock busy");
    }
    if (!ls_board_hw_antenna_tx_allowed()) {
        ls_lora_hw_unlock();
        REFUSE(ESP_ERR_INVALID_STATE, "MMCX1 antenna unconfirmed; use ant ext confirm");
    }
    if (lrtx_cancelled(cancel_generation)) {
        ls_lora_hw_unlock();
        REFUSE(ESP_ERR_INVALID_STATE, "cancelled before radio setup");
    }
    if (!data && s_lrtx_ready) { data=s_lrtx_data; size=s_lrtx_size; }
    if (!data || !size || (size & 1u) || size>LRTX_MAX_SAMPLES*2u) why="invalid I/Q buffer";
    else if (!s_bound) why="radio not bound";
    else if (!s_info.lr2021) why="LR2021 required";
    else if (s_info.fw_major != 1 || s_info.fw_minor != 24) why="LR2021 ROM 1.24 required";
    else if (!lr.cfg_valid) why="LoRa configuration invalid";
    else if (lrtx_radio_owned()) {
        if (lr.tx_busy) why="radio transmitting";
        else if (lr.scanning) why="scanner holds the radio";
        else if (lr.fsk_active) why="FSK session holds the radio";
        else if (ms.active || lr20xx_modes_active()) why="modem session holds the radio";
        else if (s_exp_active) why="experiment holds the radio";
        else if (lr20xx_stream_source2_active()) why="I/Q stream holds the radio";
        else why="I/Q transmitter holds the radio";
    }
    err = why ? ESP_ERR_INVALID_STATE : ESP_OK;
    if (err == ESP_OK) {
        const uint64_t samples = (uint64_t)(size / 2u) * repeats;
        duration_us = (int64_t)((samples * 1000000u + rate - 1u) / rate);
        if (!samples || samples > LRTX_MAX_SAMPLES || duration_us > 900000)
            { err = ESP_ERR_INVALID_ARG; why="sample/duration limit exceeded"; }
        else {
            total_bytes = size * repeats;
            sample_count = (unsigned)samples;
        }
    }
    for (uint32_t i=0; err==ESP_OK && i<size; i+=2) {
        int si=(int8_t)data[i], sq=(int8_t)data[i+1];
        if (si*si+sq*sq>56*56) { err=ESP_ERR_INVALID_ARG; why="I/Q magnitude exceeds 56"; }
    }
    if (err == ESP_OK) {
        if (ls_mesh_radio_wanted && ls_mesh_radio_wanted() && !leased) why="foreground listener holds the radio";
        else if (ls_mesh_radio_bg && ls_mesh_radio_bg()) why="background listener holds the radio";
        else if (ls_field_owned && ls_field_owned()) why="field session holds the radio";
        if (why) err=ESP_ERR_INVALID_STATE;
    }
    if (err == ESP_OK) {
        uint32_t marker = 0;
        err = lr20xx_read_reg32(0x800FF8u, &marker);
        if (err != ESP_OK) why="ROM marker read failed";
        else if (marker == 0x600DB002u) { err=ESP_ERR_INVALID_STATE; why="ROM patch marker present"; }
    }
    if (why) {
        ls_lora_hw_unlock();
        if (reason) *reason=why;
        printf("lrtx-send: %s\n",why);
        return err;
    }
    if (err == ESP_OK) {
        rx = lr.rx_mode || lr.rearm;
        s_lrtx_sending = true;
        touched = true;
        phase = "setup";
    }
    if (err == ESP_OK && lrtx_cancelled(cancel_generation)) {
        err = ESP_ERR_INVALID_STATE; phase = "cancelled";
    }
    if (err == ESP_OK) err = step_ok(lr20xx_set_standby(false), "TX standby");
    if (err == ESP_OK) err = setup_radio(LR20XX_PKT_TYPE_OOK, hz);
    if (err == ESP_OK) err = step_ok(lr20xx_ook_set_modulation(rate, 0,
                                     lr20xx_fsk_rx_bw(100000).index, 0), "TX modulation");
    if (err == ESP_OK) err = lr20xx_dcdc_workaround_set();
    if (err == ESP_OK) err = step_ok(lr20xx_ook_set_packet_fixed(32, total_bytes, 0), "TX packet");
    if (err == ESP_OK) err = step_ok(lr20xx_ook_set_sync_none(), "TX sync");
    if (err == ESP_OK) err = lr20xx_set_pa_lf(dbm);
    if (err == ESP_OK) err = step_ok(lr20xx_set_fallback_standby_rc(), "TX fallback");
    if (err == ESP_OK) {
        const uint32_t base = 0x804000u, size = 0x3FCu;
        err = lr20xx_write_reg32(0xF3002Cu, base);
        if (err == ESP_OK) err = lr20xx_write_reg32(0xF30034u, size);
        uint32_t check_base = 0, check_size = 0;
        if (err == ESP_OK) err = lr20xx_read_reg32(0xF3002Cu, &check_base);
        if (err == ESP_OK) err = lr20xx_read_reg32(0xF30034u, &check_size);
        if (err == ESP_OK && ((check_base != base && check_base != 0x4000u) || check_size != size))
            err = ESP_ERR_INVALID_RESPONSE;
    }
    if (err == ESP_OK) {
        phase = "configure-0293";
        const uint8_t args[7] = {(uint8_t)(sample_count >> 8), (uint8_t)sample_count,
            (uint8_t)(rate >> 24), (uint8_t)(rate >> 16), (uint8_t)(rate >> 8),
            (uint8_t)rate, 0};
        err = step_ok(lr20xx_write(0x0293, args, sizeof(args)), "TX 0293");
    }
    /* Keep calibration and radio setup on the normal transport. Fast SPI is
     * acquired only for FIFO service and TX polling, matching the verified
     * v13 sequence. */
    if (err == ESP_OK) {
        phase = "fast-spi";
        err = ls_lora_hw_capture_fast(true);
        fast = err == ESP_OK;
        if (fast && original_priority < 18) {
            vTaskPrioritySet(NULL, 18);
            priority_changed = true;
        }
    }
    if (err == ESP_OK) {
        phase = "fifo";
        err = step_ok(lr20xx_clear_tx_fifo(), "TX clear FIFO");
    }
    if (err == ESP_OK) err = step_ok(lr20xx_clear_irq(LR20XX_IRQ_ALL), "TX clear IRQ");
    const uint8_t clear_flags[2] = {0x3F, 0x3F};
    if (err == ESP_OK) err = step_ok(lr20xx_write(0x0114, clear_flags, 2), "TX clear flags");
    if (err == ESP_OK && lrtx_cancelled(cancel_generation)) {
        err = ESP_ERR_INVALID_STATE; phase = "cancelled";
    }
    if (err == ESP_OK) err = lrtx_enqueue(data, size, total_bytes,
                                           &queued, 896u, cancel_generation);
    if (err == ESP_ERR_INVALID_STATE && lrtx_cancelled(cancel_generation))
        phase = "cancelled";

    const int64_t timeout_us = duration_us + 50000;
    int64_t started_at = 0;
    if (err == ESP_OK) {
        phase = "transmit";
        lr.tx_busy = true;
        const uint32_t ticks = (uint32_t)((timeout_us * 32 + 999) / 1000);
        const uint8_t timeout[3] = {(uint8_t)(ticks >> 16), (uint8_t)(ticks >> 8), (uint8_t)ticks};
        started_at = esp_timer_get_time();
        if (lrtx_cancelled(cancel_generation)) {
            err = ESP_ERR_INVALID_STATE; phase = "cancelled";
        } else {
            err = step_ok(lr20xx_write(0x020D, timeout, sizeof(timeout)), "TX start");
        }
        while (err == ESP_OK && esp_timer_get_time() - started_at < timeout_us) {
            if (lrtx_cancelled(cancel_generation)) {
                err = ESP_ERR_INVALID_STATE; phase = "cancelled";
                break;
            }
            const int64_t poll_now = esp_timer_get_time();
            if (previous_poll && poll_now - previous_poll > max_poll_gap)
                max_poll_gap = poll_now - previous_poll;
            previous_poll = poll_now;
            if (queued < total_bytes) {
                err = lr20xx_read(0x011D, NULL, 0, level, 2);
                const unsigned fill = ((unsigned)level[0] << 8) | level[1];
                if (err == ESP_OK && fill > LRTX_FIFO_BYTES) err = ESP_ERR_INVALID_RESPONSE;
                if (err == ESP_OK && fill < min_level) min_level = fill;
                if (err == ESP_OK && fill <= 768u)
                    err = lrtx_enqueue(data, size, total_bytes,
                                       &queued, (896u - fill) & ~1u,
                                       cancel_generation), refills++;
                if (err != ESP_OK) {
                    if (err == ESP_ERR_INVALID_STATE && lrtx_cancelled(cancel_generation))
                        phase = "cancelled";
                    break;
                }
            }
            err = lr20xx_read(0x011B, NULL, 0, flags, 2);
            if (err == ESP_OK && (flags[1] & 0x30u)) err = ESP_FAIL;
            if (err != ESP_OK) break;
            lr20xx_status_t status;
            err = lr20xx_get_status(&status);
            if (err == ESP_OK) {
                irq = status.irq;
                if (irq & LR20XX_IRQ_TX_DONE) break;
            }
            vTaskDelay(pdMS_TO_TICKS(1));
        }
        if (err == ESP_OK) err = lr20xx_read(0x011B, NULL, 0, flags, 2);
        if (err == ESP_OK) err = lr20xx_read(0x011D, NULL, 0, level, 2);
        if (err == ESP_OK && (!(irq & LR20XX_IRQ_TX_DONE) || queued != total_bytes)) err = ESP_ERR_TIMEOUT;
        if (err == ESP_OK && (flags[1] & 0x30u)) err = ESP_FAIL;
    }
    if (touched) {
        stop = step_ok(lr20xx_set_standby(false), "TX stop");
        lr.tx_busy = false;
    }
    if (fast) {
        esp_err_t release = ls_lora_hw_capture_fast(false);
        if (err == ESP_OK && release != ESP_OK) err = release;
        if (priority_changed) vTaskPrioritySet(NULL, original_priority);
    }
    if (touched) {
        const esp_err_t error_read = lr20xx_get_errors(&errors);
        if (err == ESP_OK && error_read != ESP_OK) err = error_read;
        if (err == ESP_OK && errors) err = ESP_FAIL;
        s_recovered = false;
        recovery = recover_locked("TX playback complete");
        if (recovery == ESP_OK) {
            lr.rearm = false;
            recovery = program_lora();
            if (recovery == ESP_OK && rx) recovery = lr_receive();
        }
        if (recovery == ESP_OK) recovery = lr20xx_get_errors(&final_errors);
        if (recovery == ESP_OK && final_errors) recovery = ESP_FAIL;
        s_lrtx_sending = false;
    }
    const int64_t elapsed = started_at ? esp_timer_get_time() - started_at : 0;
    ls_lora_hw_unlock();
    const unsigned tx_level = (unsigned)((level[0] << 8) | level[1]);
    if (err == ESP_OK && tx_level != 0) err = ESP_FAIL;
    printf("lrtx-stream: fifo=%u samples=%u bytes=%u queued=%u repeats=%lu refills=%u min_level=%u max_poll_gap_us=%lld\n",
           LRTX_FIFO_BYTES, sample_count, total_bytes, queued, (unsigned long)repeats,
           refills, min_level, (long long)max_poll_gap);
    printf("lrtx: mode=upload rate=%lu samples=%u bytes=%u queued=%u repeats=%lu hz=%lu dbm=%d elapsed_us=%lld irq=%08lX txflags=%02X txlevel=%u errors=%04X final_errors=%04X phase=%s result=%s stop=%s recovery=%s\n",
           (unsigned long)rate, sample_count, total_bytes, queued, (unsigned long)repeats,
           (unsigned long)hz, dbm, (long long)elapsed, (unsigned long)irq, flags[1], tx_level, errors,
           final_errors, phase, esp_err_to_name(err), esp_err_to_name(stop),
           esp_err_to_name(recovery));
    if (reason && !strcmp(phase,"cancelled")) *reason="transmit cancelled";
#undef REFUSE
    return err!=ESP_OK?err:stop!=ESP_OK?stop:recovery!=ESP_OK?recovery:final_errors || tx_level?ESP_FAIL:ESP_OK;
}

esp_err_t lr20xx_iq_send(const uint8_t *data, size_t bytes, uint32_t hz, int dbm)
{
    return lr20xx_iq_send_reason(data,bytes,hz,dbm,NULL);
}

esp_err_t lr20xx_iq_send_reason(const uint8_t *data, size_t bytes, uint32_t hz, int dbm, const char **reason)
{
    return lr20xx_iq_send_rate_reason(data,bytes,hz,dbm,125000,reason);
}

esp_err_t lr20xx_iq_send_rate_reason(const uint8_t *data, size_t bytes, uint32_t hz, int dbm, uint32_t rate, const char **reason)
{
    if (reason) *reason=NULL;
    if (!data || bytes>LRTX_MAX_SAMPLES*2u) {
        if (reason) *reason="invalid I/Q buffer";
        return ESP_ERR_INVALID_ARG;
    }
    return lrtx_send(data, (uint32_t)bytes, rate, 1, hz, dbm, reason);
}

static int lrtx_send_command(int argc, char **argv)
{
    uint32_t rate = 0, repeats = 1;
    if (argc < 3 || argc > 4 || !probe_number(argv[2], 125000, &rate) ||
        !lrtx_rate_supported(rate) || (argc == 4 &&
        (!probe_number(argv[3], 16, &repeats) || repeats == 0))) {
        puts("usage: lora lrtx-send 4800|9600|19200|38400|76800|125000 [repeats 1..16]");
        return 1;
    }

    return lrtx_send(NULL, 0, rate, repeats, 915000000u, -9, NULL)==ESP_OK?0:1;
}

/* `lora lrx`: bench probes for the LR2021. Raw commands and register writes,
 * the Patch RAM state (Semtech's own patch, a marker word, none), the chip's
 * clear-channel measurement, and its temperature, supply and random number.
 * Included by the LR20xx board driver for its lock, state and recovery.
 *
 * `lrx own on` takes the part the way an experiment does: LoRa stops, the
 * part waits in standby, and nothing else programs it until `lrx own off`
 * resets it and gives LoRa the part back. rc and rw work with or without
 * ownership, so a register can be changed under a running session. */
#include "lr2021_semtech_pram.h"

#define LRX_RAW_ENGINE   ((lr20xx_engine_t)0x7F)
#define LRX_PRAM_MAGIC   0x600DB002u
#define LRX_PRAM_MARK    0x800FF8u
#define LRX_PRAM_VER     0x800FFCu

static bool lrx_owned(void) { return s_exp_active && s_exp_engine == LRX_RAW_ENGINE; }

/* Hex bytes, "-" for none. */
static bool lrx_hex(const char *s, uint8_t *out, size_t max, size_t *n)
{
    *n = 0;
    if (!strcmp(s, "-")) return true;
    if (!strncmp(s, "0x", 2) || !strncmp(s, "0X", 2)) s += 2;
    const size_t len = strlen(s);
    if (!len || len % 2 || len / 2 > max) return false;
    for (size_t i = 0; i < len; i += 2) {
        char b[3] = {s[i], s[i + 1], 0}, *end;
        const unsigned long v = strtoul(b, &end, 16);
        if (*end) return false;
        out[(*n)++] = (uint8_t)v;
    }
    return true;
}

static const char *lrx_cmd_name(uint16_t stat)
{
    static const char *const names[] = {"FAIL", "PERR", "OK", "DATA", "?4", "?5", "?6", "?7"};
    return names[LR20XX_STAT_CMD(stat)];
}

LR_CMD lrx_own_command(int argc, char **argv)
{
    if (argc != 4 || (strcmp(argv[3], "on") && strcmp(argv[3], "off"))) {
        puts("usage: lora lrx own on|off"); return 1;
    }
    const bool on = !strcmp(argv[3], "on");
    ls_lora_hw_lock();
    esp_err_t err = ESP_OK;
    if (on) {
        if (!s_bound || !s_info.lr2021) err = ESP_ERR_NOT_SUPPORTED;
        else if (lrx_owned()) err = ESP_OK;
        else if (s_exp_active || lr.fsk_active || lr.scanning || lr.tx_busy || lr20xx_modes_active())
            err = ESP_ERR_INVALID_STATE;
        else {
            lr.restore_rx = lr.rx_mode || lr.rearm;
            lr.programmed = lr.rx_mode = lr.rearm = false;
            s_exp_engine = LRX_RAW_ENGINE;
            s_exp_active = true;
            s_exp_reset = false;
            err = lr20xx_set_standby(false);
        }
    } else if (lrx_owned()) {
        s_exp_active = false;
        s_recovered = false;
        err = recover_locked("lab probe finished");
        const esp_err_t restored = restore_lora(lr.restore_rx);
        if (err == ESP_OK) err = restored;
    }
    ls_lora_hw_unlock();
    printf("lrx: own %s: %s\n", on ? "on" : "off", esp_err_to_name(err));
    return err == ESP_OK ? 0 : 1;
}

/* rc <op> [args hex|-] [answer bytes 0..14]: one command, as sent. The
   answer frame's Stat is that command's own status; a write is judged by
   the GetStatus after it. */
LR_CMD lrx_rc_command(int argc, char **argv)
{
    uint32_t op = 0, m = 0;
    uint8_t args[20];
    size_t n = 0;
    if (argc < 4 || argc > 6 || !probe_number(argv[3], 0xFFFF, &op) ||
        (argc > 4 && !lrx_hex(argv[4], args, sizeof args, &n)) ||
        (argc > 5 && !probe_number(argv[5], 14, &m))) {
        puts("usage: lora lrx rc <opcode e.g. 0x0125> [hex args|-] [answer bytes 0..14]"); return 1;
    }
    uint8_t out[14] = {0};
    lr20xx_status_t st = {0};
    ls_lora_hw_lock();
    esp_err_t err = s_bound ? ESP_OK : ESP_ERR_INVALID_STATE;
    if (err == ESP_OK && m) err = read_locked((uint16_t)op, args, n, out, m);
    else if (err == ESP_OK) err = write_locked((uint16_t)op, args, n);
    const uint16_t answer_stat = s_stat;
    if (err == ESP_OK && !m) err = get_status_locked(&st);
    ls_lora_hw_unlock();
    const uint16_t stat = m ? answer_stat : st.stat;
    printf("lrx: rc 0x%04lX stat=0x%04X cmd=%s mode=%u irq=0x%08lX", (unsigned long)op, (unsigned)stat,
           lrx_cmd_name(stat), (unsigned)LR20XX_STAT_MODE(stat), (unsigned long)s_irq);
    if (m) {
        fputs(" data=", stdout);
        for (uint32_t i = 0; i < m; i++) printf("%02X", out[i]);
    }
    printf(" result=%s\n", esp_err_to_name(err));
    return err == ESP_OK ? 0 : 1;
}

/* rw <addr> <mask> <value>: WriteRegMemMask32, then the word read back. */
LR_CMD lrx_rw_command(int argc, char **argv)
{
    uint32_t addr = 0, mask = 0, value = 0, after = 0;
    if (argc != 6 || !probe_number(argv[3], 0xFFFFFCu, &addr) || (addr & 3u) ||
        !probe_number(argv[4], 0xFFFFFFFFu, &mask) || !probe_number(argv[5], 0xFFFFFFFFu, &value)) {
        puts("usage: lora lrx rw <aligned address> <mask> <value>"); return 1;
    }
    ls_lora_hw_lock();
    esp_err_t err = s_bound ? lr20xx_write_reg_mask32(addr, mask, value) : ESP_ERR_INVALID_STATE;
    if (err == ESP_OK) err = lr20xx_check_last_ok();
    if (err == ESP_OK) err = lr20xx_read_reg32(addr, &after);
    ls_lora_hw_unlock();
    printf("lrx: rw 0x%06lX mask 0x%08lX value 0x%08lX -> 0x%08lX result=%s\n", (unsigned long)addr,
           (unsigned long)mask, (unsigned long)value, (unsigned long)after, esp_err_to_name(err));
    return err == ESP_OK ? 0 : 1;
}

/* The marker and version words only. Never the write window at 0x801000:
   once a patch is active the window moves (it runs at 0x101000), and a
   read of the old address stalls the part with BUSY high until a reset. */
static esp_err_t lrx_pram_state_locked(uint32_t *mark, uint32_t *ver, uint32_t *first)
{
    esp_err_t err = lr20xx_read_reg32(LRX_PRAM_MARK, mark);
    if (err == ESP_OK) err = lr20xx_read_reg32(LRX_PRAM_VER, ver);
    *first = 0;
    return err;
}

/* Standby, the words into PRAM, ActivatePram 0x00: what Semtech's driver
   does after every reset. Under the lock. */
static esp_err_t lrx_load_image_locked(const uint32_t *words, unsigned n)
{
    esp_err_t err = step_ok(lr20xx_set_standby(false), "patch standby");
    for (unsigned i = 0; err == ESP_OK && i < n; i++)
        err = lr20xx_write_reg32(COP_PRAM_HOST + 4u * i, words[i]);
    if (err == ESP_OK) {
        const uint8_t activate = 0;
        err = lr20xx_write(0x012D, &activate, 1);
        if (err == ESP_OK) err = lr20xx_check_last_ok();
    }
    return err;
}

/* pram state|semtech|marker|proof|none. Without ownership the LoRa owner
   goes to standby for the load and is armed again afterwards; no reset, so
   the patch stays until the next one (any session that resets the part,
   an I/Q stream or a chip program, clears it). */
LR_CMD lrx_pram_command(int argc, char **argv)
{
    const char *what = argc == 4 ? argv[3] : "";
    const bool state = !strcmp(what, "state"), semtech = !strcmp(what, "semtech"),
               marker = !strcmp(what, "marker"), proof = !strcmp(what, "proof"), none = !strcmp(what, "none");
    if (!state && !semtech && !marker && !proof && !none) {
        puts("usage: lora lrx pram state|semtech|marker|proof|none"); return 1;
    }
    uint32_t mark = 0, ver = 0, first = 0;
    ls_lora_hw_lock();
    esp_err_t err = s_bound ? ESP_OK : ESP_ERR_INVALID_STATE;
    const bool owned = lrx_owned();
    if (err == ESP_OK && !state && !owned &&
        (s_exp_active || lr.fsk_active || lr.scanning || lr.tx_busy || lr20xx_modes_active() || s_cop_loaded))
        err = ESP_ERR_INVALID_STATE;
    const bool rx = lr.rx_mode || lr.rearm;
    if (err == ESP_OK && none) {
        s_recovered = false;
        err = recover_locked("lab probe patch cleared");
    } else if (err == ESP_OK && semtech) {
        err = lrx_load_image_locked(lr2021_semtech_pram, LR2021_SEMTECH_PRAM_WORDS);
    } else if (err == ESP_OK && proof) {
        err = lrx_load_image_locked(pram_proof, sizeof pram_proof / sizeof pram_proof[0]);
    } else if (err == ESP_OK && marker) {
        err = step_ok(lr20xx_set_standby(false), "patch standby");
        if (err == ESP_OK) err = lr20xx_write_reg32(LRX_PRAM_MARK, LRX_PRAM_MAGIC);
        if (err == ESP_OK) err = lr20xx_check_last_ok();
    }
    if (!state && !owned && lr.cfg_valid) {
        const esp_err_t restored = restore_lora(rx);
        if (err == ESP_OK) err = restored;
    } else if (!state && owned) (void)lr20xx_set_standby(false);
    const esp_err_t read = lrx_pram_state_locked(&mark, &ver, &first);
    ls_lora_hw_unlock();
    printf("lrx: pram %s: %s | marker 0x%08lX%s type %lu version %lu\n", what,
           esp_err_to_name(err), (unsigned long)mark, mark == LRX_PRAM_MAGIC ? " (patch flag set)" : "",
           (unsigned long)((ver >> 16) & 0xFF), (unsigned long)((ver >> 8) & 0xFF));
    return err == ESP_OK && read == ESP_OK ? 0 : 1;
}

/* Temperature (Vbe and next to the crystal), supply, random words. Standby
   only, so the part must be owned. */
LR_CMD lrx_sys_command(void)
{
    uint8_t t_vbe[2] = {0}, t_xo[2] = {0}, vb[2] = {0}, r[3][4] = {{0}};
    ls_lora_hw_lock();
    esp_err_t err = !lrx_owned() ? ESP_ERR_INVALID_STATE : lr20xx_set_standby(false);
    const uint8_t a_vbe = (0u << 4) | (1u << 3) | 5u, a_xo = (1u << 4) | (1u << 3) | 5u, a_vb = (1u << 3) | 5u;
    if (err == ESP_OK) err = read_locked(0x0125, &a_vbe, 1, t_vbe, 2);
    if (err == ESP_OK) err = lr20xx_write_reg_mask32(0xF40500u, 0x3u << 8, 0);
    if (err == ESP_OK) err = read_locked(0x0125, &a_xo, 1, t_xo, 2);
    if (err == ESP_OK) err = lr20xx_write_reg_mask32(0xF40500u, 0x3u << 8, 0);
    if (err == ESP_OK) err = read_locked(0x0124, &a_vb, 1, vb, 2);
    if (err == ESP_OK) err = lr20xx_write_reg_mask32(0xF40500u, 0x3u << 8, 0);
    for (unsigned i = 0; err == ESP_OK && i < 3; i++) {
        const uint8_t src = 0;
        err = read_locked(0x0126, &src, 1, r[i], 4);
    }
    ls_lora_hw_unlock();
    const double vbe = (((t_vbe[0] << 8) | t_vbe[1]) >> 3) / 32.0, xo = (((t_xo[0] << 8) | t_xo[1]) >> 3) / 32.0;
    printf("lrx: sys temp_vbe=%.2f C temp_xosc=%.2f C vbat=%u mV random=%02X%02X%02X%02X %02X%02X%02X%02X "
           "%02X%02X%02X%02X result=%s\n", vbe, xo, (unsigned)(((vb[0] & 0x1F) << 8) | vb[1]),
           r[0][0], r[0][1], r[0][2], r[0][3], r[1][0], r[1][1], r[1][2], r[1][3],
           r[2][0], r[2][1], r[2][2], r[2][3], esp_err_to_name(err));
    return err == ESP_OK ? 0 : 1;
}

/* cca <kHz> <us> [gain 0..13] [boost 0..7]: the chip's own clear-channel
   measurement (min, max, average) against GetRssiInst polled over the same
   time in continuous Rx. Owned part only. */
LR_CMD lrx_cca_command(int argc, char **argv)
{
    uint32_t khz = 0, us = 0, gain = 0, boost = 255;
    if (argc < 5 || argc > 7 || !probe_number(argv[3], 2740000, &khz) || khz < 100000 ||
        !probe_number(argv[4], 500000, &us) || us < 10 ||
        (argc > 5 && !probe_number(argv[5], 13, &gain)) || (argc > 6 && !probe_number(argv[6], 7, &boost))) {
        puts("usage: lora lrx cca <kHz> <us 10..500000> [gain 0=AGC..13] [boost 0..7]"); return 1;
    }
    uint8_t res[4] = {0};
    float rmin = 0, rmax = -200, rsum = 0;
    unsigned looks = 0;
    int64_t cca_us = 0;
    ls_lora_hw_lock();
    esp_err_t err = !lrx_owned() ? ESP_ERR_INVALID_STATE : setup_radio(LR20XX_PKT_TYPE_LORA, khz * 1000u);
    if (err == ESP_OK && boost <= 7)
        err = step_lr(lr20xx_set_rx_path(lr20xx_path_for_hz(khz * 1000u), (uint8_t)boost), "SetRxPath");
    if (err == ESP_OK) err = lr20xx_clear_irq(LR20XX_IRQ_ALL);
    const uint32_t ticks = us * 32u;
    const uint8_t a[4] = {(uint8_t)(ticks >> 16), (uint8_t)(ticks >> 8), (uint8_t)ticks, (uint8_t)gain};
    const int64_t t0 = esp_timer_get_time();
    if (err == ESP_OK) err = step_lr(write_locked(0x0218, a, gain ? 4 : 3), "SetCca");
    bool done = false;
    while (err == ESP_OK && !done && esp_timer_get_time() - t0 < (int64_t)us + 200000) {
        uint32_t irq = 0;
        err = lr20xx_get_and_clear_irq(&irq);
        done = (irq & LR20XX_IRQ_TIMEOUT) != 0;
    }
    cca_us = esp_timer_get_time() - t0;
    if (err == ESP_OK && !done) err = ESP_ERR_TIMEOUT;
    if (err == ESP_OK) err = read_locked(0x0219, NULL, 0, res, 4);
    if (err == ESP_OK && gain) err = lr20xx_set_agc_gain((uint8_t)gain);
    if (err == ESP_OK) err = lr20xx_set_rx(0xFFFFFFu);
    const int64_t t1 = esp_timer_get_time();
    while (err == ESP_OK && (esp_timer_get_time() - t1 < (int64_t)us || !looks)) {
        float dbm = 0;
        if (lr20xx_get_rssi_inst(&dbm) == ESP_OK) {
            if (!looks || dbm < rmin) rmin = dbm;
            if (dbm > rmax) rmax = dbm;
            rsum += dbm; looks++;
        }
    }
    if (gain) (void)lr20xx_set_agc_gain(0);
    (void)lr20xx_set_standby(false);
    ls_lora_hw_unlock();
    const float cmin = -(float)((res[0] << 1) | ((res[3] >> 2) & 1)) / 2.0f,
                cmax = -(float)((res[1] << 1) | ((res[3] >> 1) & 1)) / 2.0f,
                cavg = -(float)((res[2] << 1) | (res[3] & 1)) / 2.0f;
    printf("lrx: cca %.3f MHz %lu us gain %lu: chip min %.1f max %.1f avg %.1f dBm (%lld us) | "
           "inst %u looks min %.1f max %.1f avg %.1f dBm result=%s\n", khz / 1000.0, (unsigned long)us,
           (unsigned long)gain, (double)cmin, (double)cmax, (double)cavg, (long long)cca_us, looks,
           (double)rmin, (double)rmax, looks ? (double)(rsum / looks) : 0.0, esp_err_to_name(err));
    return err == ESP_OK ? 0 : 1;
}

/* pllrx <kHz> <kHz2> <ms>: receive at kHz, then write the PLL word
   (0xF40144 = MHz x 2^20) for kHz2 halfway through, with no other command,
   and report the RSSI before and after. Owned part only. */
LR_CMD lrx_pllrx_command(int argc, char **argv)
{
    uint32_t khz = 0, khz2 = 0, ms = 0;
    if (argc != 6 || !probe_number(argv[3], 2740000, &khz) || khz < 100000 ||
        !probe_number(argv[4], 2740000, &khz2) || khz2 < 100000 || !probe_number(argv[5], 10000, &ms) || ms < 20) {
        puts("usage: lora lrx pllrx <kHz> <kHz2> <ms 20..10000>"); return 1;
    }
    float s[2] = {0, 0}, mx[2] = {-200, -200};
    unsigned n[2] = {0, 0};
    uint32_t before = 0, after = 0;
    int64_t write_us = 0;
    ls_lora_hw_lock();
    esp_err_t err = !lrx_owned() ? ESP_ERR_INVALID_STATE : setup_radio(LR20XX_PKT_TYPE_LORA, khz * 1000u);
    if (err == ESP_OK) err = lr20xx_set_rx(0xFFFFFFu);
    if (err == ESP_OK) err = lr20xx_read_reg32(0xF40144u, &before);
    const uint32_t word = (uint32_t)(((uint64_t)khz2 << 20) / 1000u);
    const int64_t t0 = esp_timer_get_time(), half = (int64_t)ms * 500;
    bool moved = false;
    while (err == ESP_OK && esp_timer_get_time() - t0 < 2 * half) {
        if (!moved && esp_timer_get_time() - t0 >= half) {
            const int64_t w0 = esp_timer_get_time();
            err = lr20xx_write_reg32(0xF40144u, word);
            write_us = esp_timer_get_time() - w0;
            moved = true;
            continue;
        }
        float dbm = 0;
        if (lr20xx_get_rssi_inst(&dbm) == ESP_OK && dbm > -250.0f) {
            const int k = moved ? 1 : 0;
            s[k] += dbm; n[k]++;
            if (dbm > mx[k]) mx[k] = dbm;
        }
    }
    if (err == ESP_OK) err = lr20xx_read_reg32(0xF40144u, &after);
    (void)lr20xx_set_standby(false);
    ls_lora_hw_unlock();
    printf("lrx: pllrx %.3f -> %.3f MHz: before avg %.1f max %.1f dBm (%u) | after avg %.1f max %.1f dBm (%u) | "
           "PLL 0x%08lX -> 0x%08lX (asked 0x%08lX) write %lld us result=%s\n", khz / 1000.0, khz2 / 1000.0,
           n[0] ? (double)(s[0] / n[0]) : 0.0, (double)mx[0], n[0], n[1] ? (double)(s[1] / n[1]) : 0.0,
           (double)mx[1], n[1], (unsigned long)before, (unsigned long)after, (unsigned long)word,
           (long long)write_us, esp_err_to_name(err));
    return err == ESP_OK ? 0 : 1;
}

/* step init|tcxo|reg|cal|fe|stby: one piece of the driver's setup on an
   owned part, then whether BUSY came back. Finds the command a loaded
   patch does not survive. */
LR_CMD lrx_step_command(int argc, char **argv)
{
    const char *w = argc == 4 ? argv[3] : "";
    ls_lora_hw_lock();
    esp_err_t err = lrx_owned() ? ESP_OK : ESP_ERR_INVALID_STATE;
    const int64_t t0 = esp_timer_get_time();
    if (err != ESP_OK) {
    } else if (!strcmp(w, "init")) err = initialize_locked();
    else if (!strcmp(w, "tcxo")) {
        const uint32_t ticks = LR20XX_TCXO_DELAY_TICKS;
        const uint8_t a[5] = { ls_lora_port()->tcxo_code, (uint8_t)(ticks >> 24),
                              (uint8_t)(ticks >> 16), (uint8_t)(ticks >> 8), (uint8_t)ticks };
        err = lr20xx_write(LR20XX_OP_SET_TCXO_MODE, a, sizeof a);
        if (err == ESP_OK) err = lr20xx_wait_ready(1200);
        if (err == ESP_OK) err = lr20xx_check_last_ok();
    } else if (!strcmp(w, "reg")) {
        const uint8_t reg = s_dcdc ? 2 : 0;
        err = lr20xx_write(LR20XX_OP_SET_REG_MODE, &reg, 1);
        if (err == ESP_OK) err = lr20xx_check_last_ok();
    } else if (!strcmp(w, "cal")) {
        err = calibrate_locked(LR20XX_CAL_LF_RC | LR20XX_CAL_HF_RC | LR20XX_CAL_PLL |
                               LR20XX_CAL_AAF | LR20XX_CAL_MU | LR20XX_CAL_PA_OFF);
    } else if (!strcmp(w, "fe")) err = lr20xx_calib_fe_hz(915000000u, LR20XX_PATH_LF);
    else if (!strcmp(w, "stby")) { err = lr20xx_set_standby(false); if (err == ESP_OK) err = lr20xx_check_last_ok(); }
    else err = ESP_ERR_INVALID_ARG;
    const int64_t took = esp_timer_get_time() - t0;
    const bool busy_ok = ls_lora_hw_wait_not_busy(50);
    uint16_t errors = 0;
    if (busy_ok) (void)lr20xx_get_errors(&errors);
    ls_lora_hw_unlock();
    if (err == ESP_ERR_INVALID_ARG) { puts("usage: lora lrx step init|tcxo|reg|cal|fe|stby"); return 1; }
    printf("lrx: step %s: %s in %lld us, busy %s, errors 0x%04X\n", w, esp_err_to_name(err), (long long)took,
           busy_ok ? "low" : "STUCK HIGH", (unsigned)errors);
    return err == ESP_OK && busy_ok ? 0 : 1;
}

LR_CMD lrx_command(int argc, char **argv)
{
    const char *sub = argc > 2 ? argv[2] : "";
    if (!strcmp(sub, "step")) return lrx_step_command(argc, argv);
    if (!strcmp(sub, "pllrx")) return lrx_pllrx_command(argc, argv);
    if (!strcmp(sub, "scanmode")) {
        if (argc == 4 && (!strcmp(argv[3], "cca") || !strcmp(argv[3], "inst"))) s_scan_cca = !strcmp(argv[3], "cca");
        else if (argc != 3) { puts("usage: lora lrx scanmode [cca|inst]"); return 1; }
        printf("lrx: scanmode %s\n", s_scan_cca ? "cca" : "inst");
        return 0;
    }
    if (!strcmp(sub, "own")) return lrx_own_command(argc, argv);
    if (!strcmp(sub, "rc")) return lrx_rc_command(argc, argv);
    if (!strcmp(sub, "rw")) return lrx_rw_command(argc, argv);
    if (!strcmp(sub, "pram")) return lrx_pram_command(argc, argv);
    if (!strcmp(sub, "sys")) return lrx_sys_command();
    if (!strcmp(sub, "cca")) return lrx_cca_command(argc, argv);
    puts("usage: lora lrx own on|off | rc <op> [hex] [n] | rw <addr> <mask> <value> |\n"
         "       pram state|semtech|marker|proof|none | sys | cca <kHz> <us> [gain] [boost]");
    return 1;
}

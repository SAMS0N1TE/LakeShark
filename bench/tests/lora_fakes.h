/* Shared fakes for the LoRa socket tests: the expander and GPIO the driver
   touches, and a SPI responder that can play an LR2021, an SX1262, or a bus
   nobody drives. Include once per test executable.

   The LR20xx model follows datasheet rev 2.1: 16-bit opcodes; Stat(2) first on
   every frame; the 32-bit IRQ word after it on write frames of six bytes or
   more; reads as two frames with BUSY between. A frame that arrives while the
   fake holds BUSY high is counted in `violations` and answered with nothing
   useful, so a driver that forgets the wait both fails and is caught. */

#ifndef LORA_FAKES_H
#define LORA_FAKES_H

#include "ls_test.h"
#include "ls_xl9535.h"
#include "ls_board.h"
#include "ls_lora.h"
#include "ls_spi.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "settings.h"

#include <string.h>

#define LS_FK_OK_ 2   /* CMD_OK */

/* ----------------------------------------------------- expander, GPIO -- */

static int  fk_rst_writes;
static int  fk_xl_gets;
static bool fk_dio1_level;

/* Every expander output write, in order, for the antenna switch tests. */
typedef struct { int pin; bool level; } fk_xl_write_t;
static fk_xl_write_t fk_xl_log[32];
static int fk_xl_log_n;
static esp_err_t fk_xl_error;

static void fk_reset_pin(bool level);

esp_err_t ls_xl9535_out(int pin, bool level)
{
    if (fk_xl_error != ESP_OK) return fk_xl_error;
    fk_rst_writes++;
    if (pin == LS_BOARD_XL_RADIO_RST) fk_reset_pin(level);
    if (fk_xl_log_n < 32) { fk_xl_log[fk_xl_log_n].pin = pin; fk_xl_log[fk_xl_log_n].level = level; fk_xl_log_n++; }
    return ESP_OK;
}

/* What the settings say about the antenna (ls_board_hw.c owns it on the board). */
static bool fk_ant_external;
static bool fk_lr_tcxo = true, fk_lr_dcdc;
static uint16_t fk_tcxo_errors;
bool settings_get_lr_tcxo(void) { return fk_lr_tcxo; }
bool settings_set_lr_tcxo(bool tcxo) { fk_lr_tcxo = tcxo; return true; }
bool settings_get_lr_dcdc(void) { return fk_lr_dcdc; }
bool settings_set_lr_dcdc(bool dcdc) { fk_lr_dcdc = dcdc; return true; }
static int32_t fk_lr_fcorr_ppb;
int32_t settings_get_lr_fcorr_ppb(void) { return fk_lr_fcorr_ppb; }
bool settings_set_lr_fcorr_ppb(int32_t ppb)
{
    if (ppb > LS_LR_FCORR_MAX_PPB || ppb < -LS_LR_FCORR_MAX_PPB) return false;
    fk_lr_fcorr_ppb = ppb;
    return true;
}
/* Where the LoRa radio is wired (ls_lora_port.c): nothing saved unless a test saves it. */
static uint64_t fk_port_a, fk_port_b;
static bool fk_port_have;
bool settings_get_lora_port(uint64_t *a, uint64_t *b)
{
    if (!fk_port_have) return false;
    *a = fk_port_a;
    *b = fk_port_b;
    return true;
}
bool settings_set_lora_port(uint64_t a, uint64_t b) { fk_port_a = a; fk_port_b = b; fk_port_have = true; return true; }
#ifndef LS_TEST_REAL_BOARD_HW
bool ls_board_hw_antenna_is_external(void) { return fk_ant_external; }
bool ls_board_hw_antenna_tx_allowed(void) { return true; }
esp_err_t ls_board_hw_antenna_tx_begin(void) { return ESP_OK; }
void ls_board_hw_antenna_tx_end(void) { }
static bool s_stub_mmcx_tx;
void ls_board_hw_antenna_set_mmcx_tx(bool tx) { s_stub_mmcx_tx = tx; }
bool ls_board_hw_antenna_mmcx_tx(void) { return s_stub_mmcx_tx; }
#endif
bool ls_xl9535_ready(void) { return true; }
esp_err_t ls_xl9535_set_dir(int pin, bool output) { (void)pin; (void)output; return ESP_OK; }
esp_err_t ls_xl9535_get(int pin, bool *level)
{
    (void)pin;
    fk_xl_gets++;
    if (level) *level = fk_dio1_level;
    return ESP_OK;
}

/* BUSY: high for this many reads, then low. */
static int fk_busy_for;
static int fk_busy_reads;
static unsigned fk_busy_step_us = 200;
static void fk_capture_advance(void);

esp_err_t gpio_config(const gpio_config_t *cfg) { (void)cfg; return ESP_OK; }
/* GPIO writes, in order, and a reset line that is a GPIO (a module on the
   header) reaching the part the way the expander's does. */
typedef struct { int pin; int level; } fk_gpio_write_t;
static fk_gpio_write_t fk_gpio_log[64];
static int fk_gpio_log_n;
static int fk_rst_gpio = -1;
esp_err_t gpio_set_level(gpio_num_t pin, uint32_t level)
{
    if (fk_gpio_log_n < 64) { fk_gpio_log[fk_gpio_log_n].pin = (int)pin; fk_gpio_log[fk_gpio_log_n].level = (int)level; fk_gpio_log_n++; }
    if ((int)pin == fk_rst_gpio) fk_reset_pin(level != 0);
    return ESP_OK;
}
int gpio_get_level(gpio_num_t pin)
{
    (void)pin;
    fk_busy_reads++;
    ls_shim_time_advance(fk_busy_step_us);
    fk_capture_advance();
    if (fk_busy_for > 0) { fk_busy_for--; return 1; }
    return 0;
}

/* ------------------------------------------------------------- the part -- */

typedef enum {
    FK_ALL_00,       /* nothing drives MISO, pulled low  */
    FK_ALL_FF,       /* nothing drives MISO, pulled high */
    FK_LR,           /* an LR20xx                        */
    FK_SX_ECHO,      /* an SX126x that leaves unknown frames echoed         */
    FK_SX_STATUS,    /* an SX126x that answers every byte with its status   */
} fk_kind_t;

#define FK_LOG 1024
typedef struct { uint8_t b[32]; size_t len; size_t full; bool read2; } fk_frame_t;

typedef struct {
    fk_kind_t kind;

    uint8_t engine_status[8], engine_stats[6];
    int32_t lora_fei;
    /* LR20xx */
    uint8_t  fw_major, fw_minor;
    uint8_t  mode;                 /* ChipMode                              */
    uint8_t  reset_src;
    uint8_t  cmd_status;           /* result of the previous command        */
    uint32_t irq;
    uint16_t errors;
    uint8_t  rssi_hi, rssi_lo;     /* GetRssiInst data bytes                */
    /* Opt-in model of NRESET, for the recovery tests: with model_reset a pulse
       on the reset pin restarts the part (mode, packet type and IRQ forgotten,
       Stat shows the reset source). dead_resets: that many resets leave it
       answering nothing useful, as a part still starting does. wedged: it
       refuses every write until the next reset. has_next_mode: once a GetStatus
       has been answered the part is in next_mode, as a part that came back to Rx
       by itself. */
    bool     model_reset, silent, wedged, has_next_mode;
    int      resets, dead_resets;
    uint8_t  next_mode;
    bool     fail_next_write;
    uint16_t fail_op;              /* a write with this opcode answers CMD_FAIL */
    bool     bad_read_stat;        /* frame 2 of a read carries Stat 0xFFFF */
    uint16_t stuck_after_frame1;   /* opcode whose BUSY never falls         */
    uint32_t last_freq_hz;
    uint8_t  last_path, last_boost;
    bool     read_pending;
    uint16_t read_op;
    uint32_t read_addr;            /* ReadRegMem32 address of the pending read */
    /* The one register the Mode S tuning touches: the OOK detection threshold. */
    uint32_t thr_reg;
    uint32_t iq_fifo_base, iq_fifo_size;  /* raw-IQ FIFO mapping registers */
    uint32_t tx_fifo_base, tx_fifo_size;
    uint16_t tx_samples;
    uint32_t tx_rate;
    unsigned tx_fifo_write_count, tx_fifo_fail_on_write, tx_fifo_total_written;
    unsigned tx_cancel_on_fifo_write;
    int      thr_writes;
    int      bad_reg;              /* register accesses to any other address    */

    /* What the Mode S session programs, as the part would hold it. */
    uint8_t  packet_type;          /* 0xFF until SetPacketType                */
    uint8_t  dio_func[12];         /* last SetDioFunction byte per DIO        */
    uint8_t  dio_rfsw[12];         /* last SetDioRfSwitchConfig per DIO       */
    bool     dio_rfsw_set[12];
    int      irq_dio;
    uint32_t irq_mask;
    uint8_t  agc;
    uint8_t  fallback;
    uint8_t  calibrate_blocks;
    uint8_t  ook_mod[7], ook_pkt[6], ook_sync[5], ook_det[5];
    bool     ook_mod_set, ook_pkt_set, ook_sync_set, ook_det_set;
    int      bad_args;             /* frames with the wrong number of arguments */
    int      bad_mode;             /* commands sent in a mode that refuses them */
    int      tx_frames;            /* any transmit-path opcode: Tx FIFO, PA, SetTx */
    int      rx_arms;              /* SetRx accepted                            */
    /* The Rx FIFO. fk_push_frame() appends and raises RxDone. */
    uint8_t  fifo[1024];
    int      fifo_n;
    uint8_t  ook_rssi_high, ook_rssi_avg, ook_rssi_flags, ook_lqi;

    /* LoRa and GFSK, as the part would hold them. */
    uint8_t  lora_mod[2], lora_pkt[4], lora_sync;
    bool     lora_mod_set, lora_pkt_set, lora_sync_set;
    uint8_t  gfsk_mod[9], gfsk_pkt[7], gfsk_sync[9];
    bool     gfsk_mod_set, gfsk_pkt_set, gfsk_sync_set;
    uint16_t rx_pkt_len;           /* GetRxPktLength                            */
    int8_t   lora_snr_q;           /* GetLoraPacketStatus: SNR in quarter dB    */
    uint8_t  lora_rssi_hi, lora_rssi_flags;   /* RSSI(8:1), and its bit 0 in bit 1 */
    uint8_t  gfsk_rssi_avg, gfsk_rssi_flags;
    uint8_t  gfsk_stats[14];       /* GetFskRxStats: seven big-endian counters  */
    int      gfsk_stats_reads;
    /* The transmitter. hf_pa counts anything that selects, configures or
       powers the HF PA; tx_hf counts a SetTx accepted at 1 GHz or above. */
    uint8_t  pa_sel, pa_cfg[3], tx_params[2];
    int      pa_sel_n, pa_cfg_n, tx_params_n;
    int      hf_pa, tx_hf, tx_starts;
    bool     tx_auto_done;
    uint32_t tx_freq_hz;
    uint8_t  txfifo[300];
    int      txfifo_n;
    /* The DC-DC workaround's registers. */
    uint32_t dcdc_adc, dcdc_sw, dcdc_freq_lf;
    int      dcdc_writes;
    uint32_t proof_ram[25], proof_magic;
    bool proof_fail_result, proof_mapped;
    uint32_t capture_ram[256];
    bool capture_enabled, capture_timeout;
    bool capture_fifo, capture_fifo_full, capture_tail_overflow;
    uint8_t capture_fifo_error_flags;
    unsigned capture_flag_reads, capture_overflow_at;
    bool capture_timed;
    int64_t capture_updated;
    unsigned capture_peak, capture_high_residual_polls;

    /* SX126x */
    uint8_t  sx_status;

    int        nframes;
    fk_frame_t frames[FK_LOG];
    int        violations;
} fk_t;

static fk_t fk;

static void fk_reset_pin(bool level)
{
    if (!level || !fk.model_reset) return;
    fk.resets++;
    fk.mode = 1; fk.reset_src = 2; fk.cmd_status = 2; fk.irq = 0;
    fk.packet_type = 0xFF;
    fk.read_pending = false;
    fk.proof_mapped = false;
    fk.capture_enabled = false;
    fk.capture_fifo = false;
    fk.wedged = false;
    fk.silent = fk.dead_resets > 0;
    if (fk.dead_resets > 0) fk.dead_resets--;
}

static void fk_log(const uint8_t *tx, size_t len, bool read2)
{
    if (fk.nframes >= FK_LOG) return;
    fk_frame_t *f = &fk.frames[fk.nframes++];
    f->len = len < sizeof(f->b) ? len : sizeof(f->b);
    f->full = len;                     /* what was clocked, whatever was kept */
    memcpy(f->b, tx, f->len);
    f->read2 = read2;
}

static uint16_t fk_stat(void)
{
    return (uint16_t)(((unsigned)fk.cmd_status << 9) | (fk.irq ? 0x100u : 0u) |
                      ((unsigned)fk.reset_src << 4) | (fk.mode & 7u));
}

/* Continuous 500 bytes/ms producer, including time spent blocked and in SPI.
   Overflow latches instead of wrapping or silently dropping the evidence. */
static void fk_capture_advance(void)
{
    if (!fk.capture_timed) return;
    int64_t now = esp_timer_get_time();
    if (!fk.capture_fifo || fk.mode != 4) { fk.capture_updated = now; return; }
    unsigned arrived = (unsigned)((now - fk.capture_updated) / 2);
    fk.capture_updated += arrived * 2;
    unsigned level = (unsigned)fk.fifo_n + arrived;
    if (level > fk.capture_peak) fk.capture_peak = level;
    if (level >= 1024) fk.capture_fifo_error_flags |= 0x10;
    if (level > sizeof(fk.fifo)) level = sizeof(fk.fifo);
    while ((unsigned)fk.fifo_n < level) fk.fifo[fk.fifo_n++] = 0;
}

static void fk_respond_lr(const uint8_t *tx, uint8_t *rx, size_t len)
{
    if (fk.silent) {                    /* still starting: a byte of nonsense, nothing done */
        fk_log(tx, len, false);
        memset(rx, 0, len);
        rx[0] = 0x01;
        return;
    }
    const bool violated = fk_busy_for > 0;
    if (violated) fk.violations++;

    if (fk.read_pending) {
        /* Frame 2. */
        uint8_t d[16] = { 0 };
        size_t n = 0;
        switch (fk.read_op) {
        case 0x0101: d[0] = fk.fw_major; d[1] = fk.fw_minor; n = 2; break;
        case 0x0110: d[0] = (uint8_t)(fk.errors >> 8); d[1] = (uint8_t)fk.errors; n = 2; break;
        case 0x020B: d[0] = fk.rssi_hi; d[1] = fk.rssi_lo; n = 2; break;
        case 0x011C:
            if (fk.capture_fifo && fk.mode == 4 && !fk.capture_timeout && !fk.capture_timed) {
                fk.fifo_n = fk.capture_fifo_full ? (fk.iq_fifo_size == 0x3E8u ? 1024 : 256) : 128;
                for (int i = 0; i < fk.fifo_n; i++) fk.fifo[i] = (uint8_t)i;
            }
            if (fk.capture_timed && fk.fifo_n > 32) fk.capture_high_residual_polls++;
            d[0] = (uint8_t)(fk.fifo_n >> 8); d[1] = (uint8_t)fk.fifo_n; n = 2; break;
        case 0x011B:
            fk.capture_flag_reads++;
            if (fk.capture_overflow_at && fk.capture_flag_reads >= fk.capture_overflow_at)
                fk.capture_fifo_error_flags |= 0x10;
            d[0] = fk.capture_fifo_error_flags; n = 2; break;
        case 0x011D: d[0] = 0; d[1] = 0; n = 2; break; /* TX FIFO level is consumed */
        case 0x0287: d[0] = 0; d[1] = 28; d[2] = fk.ook_rssi_avg; d[3] = fk.ook_rssi_high;
                     d[4] = fk.ook_rssi_flags; d[5] = fk.ook_lqi; n = 6; break;
        case 0x0106:
            if (fk.read_addr == 0xF30054u) {
                uint32_t value = fk.capture_timeout ? 0x02002000u : 0x030023FCu;
                if (!fk.capture_enabled || fk.mode != 4) fk.bad_mode++;
                if (!fk.capture_timeout)
                    for (unsigned i = 0; i < 256; i++) fk.capture_ram[i] = 0x12340000u + i;
                d[0] = value >> 24; d[1] = value >> 16; d[2] = value >> 8; d[3] = value;
            } else if (fk.read_addr >= 0x802000u && fk.read_addr <= 0x8023FCu) {
                if (fk.capture_enabled && fk.mode == 4) fk.bad_mode++;
                uint32_t value = fk.capture_ram[(fk.read_addr - 0x802000u) / 4];
                d[0] = value >> 24; d[1] = value >> 16; d[2] = value >> 8; d[3] = value;
            } else if (fk.read_addr == 0xF30028u || fk.read_addr == 0xF30030u ||
                       fk.read_addr == 0xF3002Cu || fk.read_addr == 0xF30034u) {
                uint32_t value = fk.read_addr == 0xF30028u ? fk.iq_fifo_base :
                                 fk.read_addr == 0xF30030u ? fk.iq_fifo_size :
                                 fk.read_addr == 0xF3002Cu ? fk.tx_fifo_base : fk.tx_fifo_size;
                d[0] = value >> 24; d[1] = value >> 16; d[2] = value >> 8; d[3] = value;
            } else if (fk.read_addr == 0xF30E14u) {
                d[0] = (uint8_t)(fk.thr_reg >> 24); d[1] = (uint8_t)(fk.thr_reg >> 16);
                d[2] = (uint8_t)(fk.thr_reg >> 8);  d[3] = (uint8_t)fk.thr_reg;
            } else if (fk.read_addr == 0xF40200u) {
                d[0] = (uint8_t)(fk.dcdc_adc >> 24); d[1] = (uint8_t)(fk.dcdc_adc >> 16);
                d[2] = (uint8_t)(fk.dcdc_adc >> 8);  d[3] = (uint8_t)fk.dcdc_adc;
            } else if (fk.read_addr == 0x800FF8u ||
                       (fk.read_addr >= 0x801000u && fk.read_addr <= 0x801060u) ||
                       (fk.read_addr >= 0x101000u && fk.read_addr <= 0x101060u)) {
                if (fk.read_addr != 0x800FF8u &&
                    ((fk.read_addr < 0x800000u) != fk.proof_mapped)) fk.bad_reg++;
                uint32_t value = fk.read_addr == 0x800FF8u ? fk.proof_magic :
                    fk.proof_ram[(fk.read_addr & 0xFFu) / 4];
                d[0] = value >> 24; d[1] = value >> 16; d[2] = value >> 8; d[3] = value;
            } else fk.bad_reg++;
            n = 4;
            break;
        case 0x0291: d[0] = 4; d[1] = 0; n = 2; break;
        case 0x0212: d[0] = (uint8_t)(fk.rx_pkt_len >> 8); d[1] = (uint8_t)fk.rx_pkt_len; n = 2; break;
        case 0x022A: d[0] = 0x11; d[1] = (uint8_t)fk.rx_pkt_len; d[2] = (uint8_t)fk.lora_snr_q;
                     d[3] = fk.lora_rssi_hi; d[4] = fk.lora_rssi_hi; d[5] = fk.lora_rssi_flags; d[6] = (uint32_t)fk.lora_fei >> 16; d[7] = (uint32_t)fk.lora_fei >> 8; d[8] = fk.lora_fei; n = 9; break;
        case 0x0273: case 0x029A: memcpy(d,fk.engine_status,8); n = fk.read_op == 0x0273 ? 8 : 7; break;
        case 0x0272: case 0x0299: memcpy(d,fk.engine_stats,6); n = 6; break;
        case 0x0247: d[0] = (uint8_t)(fk.rx_pkt_len >> 8); d[1] = (uint8_t)fk.rx_pkt_len;
                     d[2] = fk.gfsk_rssi_avg; d[3] = fk.gfsk_rssi_avg; d[4] = fk.gfsk_rssi_flags; n = 6; break;
        case 0x0246: memcpy(d, fk.gfsk_stats, 14); n = 14; fk.gfsk_stats_reads++; break;
        case 0x0117:
            d[0] = (uint8_t)(fk.irq >> 24); d[1] = (uint8_t)(fk.irq >> 16);
            d[2] = (uint8_t)(fk.irq >> 8);  d[3] = (uint8_t)fk.irq; n = 4;
            fk.irq = 0;
            break;
        default: break;
        }
        fk_log(tx, len, true);
        memset(rx, 0, len);
        if (!violated) {
            const uint16_t s = fk.bad_read_stat ? 0xFFFFu
                : (uint16_t)((3u << 9) | (fk.irq ? 0x100u : 0u) | ((unsigned)fk.reset_src << 4) | (fk.mode & 7u));
            if (len >= 2) { rx[0] = (uint8_t)(s >> 8); rx[1] = (uint8_t)s; }
            for (size_t i = 0; i < n && 2 + i < len; i++) rx[2 + i] = d[i];
        }
        fk.read_pending = false;
        fk.cmd_status = LS_FK_OK_;
        fk_busy_for = 2;
        return;
    }

    uint8_t t[32] = { 0 };
    const size_t cl = len < sizeof(t) ? len : sizeof(t);
    memcpy(t, tx, cl);
    fk_log(t, cl, false);
    if (fk.nframes > 0 && fk.nframes <= FK_LOG) fk.frames[fk.nframes - 1].full = len;
    const uint16_t op = (uint16_t)((t[0] << 8) | t[1]);
    const uint16_t stat_before = fk_stat();
    /* The Tx FIFO bytes are taken before the reply overwrites them: the driver
       sends and receives through the one buffer. */
    if (op == 0x0002 && !violated) {
        fk.tx_fifo_write_count++;
        for (size_t i = 2; i < len && fk.txfifo_n < (int)sizeof(fk.txfifo); i++)
            fk.txfifo[fk.txfifo_n++] = tx[i];
        fk.tx_fifo_total_written += len > 2 ? (unsigned)(len - 2) : 0;
    }

    memset(rx, 0, len);
    if (len >= 2) { rx[0] = (uint8_t)(stat_before >> 8); rx[1] = (uint8_t)stat_before; }
    if (len >= 6) {
        rx[2] = (uint8_t)(fk.irq >> 24); rx[3] = (uint8_t)(fk.irq >> 16);
        rx[4] = (uint8_t)(fk.irq >> 8);  rx[5] = (uint8_t)fk.irq;
    }

    fk_busy_for = 2;
    switch (op) {
    case 0x0100:                        /* GetStatus clears the reset source */
        fk.reset_src = 0;
        fk.cmd_status = LS_FK_OK_;
        if (fk.has_next_mode) { fk.mode = fk.next_mode; fk.has_next_mode = false; }
        return;
    case 0x0101: case 0x0110: case 0x020B: case 0x0117: case 0x011C: case 0x011B: case 0x011D: case 0x0287: case 0x0106: case 0x0291:
    case 0x0212: case 0x022A: case 0x0246: case 0x0247: case 0x0272: case 0x0273: case 0x0299: case 0x029A:
        fk.read_pending = true;
        fk.read_op = op;
        if (op == 0x0106) fk.read_addr = ((uint32_t)t[2] << 16) | ((uint32_t)t[3] << 8) | t[4];
        if (fk.stuck_after_frame1 == op) fk_busy_for = 1000000;
        return;
    case 0x0001: {                      /* ReadRadioRxFifo, one frame (6.1.1): Stat, then the data */
        const size_t want = len > 2 ? len - 2 : 0;
        const size_t got = want < (size_t)fk.fifo_n ? want : (size_t)fk.fifo_n;
        if (want) memset(&rx[2], 0, want);   /* the IRQ word is not part of a direct read */
        if (got) memcpy(&rx[2], fk.fifo, got);
        if (got < (size_t)fk.fifo_n) memmove(fk.fifo, &fk.fifo[got], (size_t)fk.fifo_n - got);
        fk.fifo_n -= (int)got;
        return;                         /* no status change: not a command */
    }
    case 0x0002:                        /* WriteRadioTxFifo, one frame: the bytes joined the Tx FIFO above */
        fk.tx_frames++;
        if (fk.tx_cancel_on_fifo_write &&
            fk.tx_fifo_write_count == fk.tx_cancel_on_fifo_write)
            ls_lora_tx_cancel_request();
        if (fk.fail_op == 0x0002 || fk.tx_fifo_fail_on_write == fk.tx_fifo_write_count) {
            fk.cmd_status = 0; return;
        }
        if (fk.tx_auto_done && fk.tx_fifo_total_written >= (unsigned)fk.tx_samples * 2u)
            fk.irq |= (1u << 19);
        fk.cmd_status = LS_FK_OK_;
        return;
    default: break;
    }

    if (fk.wedged) { fk.cmd_status = 0; return; }
    if (fk.fail_next_write) { fk.fail_next_write = false; fk.cmd_status = 0; return; }
    if (fk.fail_op && fk.fail_op == op) { fk.cmd_status = 0; return; }
    fk.cmd_status = LS_FK_OK_;

    if (op == 0x020D || op == 0x0203 || op == 0x0202 || op == 0x020F) fk.tx_frames++;

    /* The arguments each write takes (datasheet command tables, and RadioLib's
       LR2021 module for the packet and transmit commands). */
    static const struct { uint16_t op; uint8_t n; } nargs_of[] = {
        { 0x0104, 7 }, { 0x0105, 11 }, { 0x012D, 1 }, { 0x0290, 11 }, { 0x0111, 0 }, { 0x0112, 2 }, { 0x0113, 2 }, { 0x0114, 2 }, { 0x0115, 5 },
        { 0x0116, 4 }, { 0x011E, 0 }, { 0x011F, 0 }, { 0x0120, 5 }, { 0x0121, 1 }, { 0x0122, 1 }, { 0x0123, 6 }, { 0x0128, 1 },
        { 0x0200, 4 }, { 0x0201, 2 }, { 0x0202, 3 }, { 0x0203, 2 }, { 0x0206, 1 }, { 0x0207, 1 },
        { 0x020C, 3 }, { 0x020D, 3 }, { 0x020F, 1 }, { 0x021A, 1 },
        { 0x0220, 2 }, { 0x0221, 4 }, { 0x0223, 1 }, { 0x0240, 9 }, { 0x0241, 7 }, { 0x0244, 9 },
        { 0x0281, 7 }, { 0x0282, 6 }, { 0x0284, 5 }, { 0x0288, 5 }, { 0x0293, 7 },
    };
    const size_t nargs = len > 2 ? len - 2 : 0;
    if (op == 0x0224 || op == 0x0225) {
        if (nargs > 3) { fk.bad_args++; fk.cmd_status=1; } return;
    }
    if (op == 0x029C) {
        if (nargs < 14 || nargs > 19 || nargs != 4 + 5 * (t[2] >> 4)) { fk.bad_args++; fk.cmd_status=1; } return;
    }
    bool known = op == 0x0270 || op == 0x0271 || op == 0x0297 || op == 0x0298 || op == 0x029D;
    if (known) {
        const size_t want = op == 0x0270 ? 2 : op == 0x0271 ? 5 : op == 0x0297 ? 8 : op == 0x0298 ? 4 : 0;
        if (nargs != want) { fk.bad_args++; fk.cmd_status=1; return; }
    }
    for (size_t i = 0; i < sizeof(nargs_of) / sizeof(nargs_of[0]); i++)
        if (nargs_of[i].op == op) {
            known = true;
            if (nargs_of[i].n != nargs) { fk.bad_args++; fk.cmd_status = 1; return; }
        }
    if (!known) { fk.cmd_status = 1; return; }                 /* CMD_PERR */

    switch (op) {
    case 0x0120: fk.errors |= fk_tcxo_errors; break;
    case 0x0121: break;
    case 0x0128:
        if (fk.capture_fifo && fk.mode == 4 && fk.capture_tail_overflow)
            fk.capture_fifo_error_flags |= 0x10;
        fk.mode = t[2] ? 2 : 1; break;
    case 0x0105: {                      /* WriteRegMemMask32: Addr(3) Mask(4) Data(4) */
        const uint32_t addr = ((uint32_t)t[2] << 16) | ((uint32_t)t[3] << 8) | t[4];
        const uint32_t mask = ((uint32_t)t[5] << 24) | ((uint32_t)t[6] << 16) | ((uint32_t)t[7] << 8) | t[8];
        const uint32_t data = ((uint32_t)t[9] << 24) | ((uint32_t)t[10] << 16) | ((uint32_t)t[11] << 8) | t[12];
        if (addr == 0xF20024u) { fk.dcdc_sw = (fk.dcdc_sw & ~mask) | (data & mask); fk.dcdc_writes++; break; }
        if (addr != 0xF30E14u) { fk.bad_reg++; fk.cmd_status = 1; break; }
        fk.thr_reg = (fk.thr_reg & ~mask) | (data & mask);
        fk.thr_writes++;
        break;
    }
    case 0x0104: {                      /* WriteRegMem32: Addr(3) Data(4) */
        const uint32_t addr = ((uint32_t)t[2] << 16) | ((uint32_t)t[3] << 8) | t[4];
        if (addr == 0xF30028u || addr == 0xF30030u ||
            addr == 0xF3002Cu || addr == 0xF30034u) {
            uint32_t v = ((uint32_t)t[5] << 24) | ((uint32_t)t[6] << 16) | ((uint32_t)t[7] << 8) | t[8];
            if (addr == 0xF30028u) fk.iq_fifo_base = v;
            if (addr == 0xF30030u) fk.iq_fifo_size = v;
            if (addr == 0xF3002Cu) fk.tx_fifo_base = v;
            if (addr == 0xF30034u) fk.tx_fifo_size = v;
            break;
        }
        if (addr >= 0x802000u && addr <= 0x8023FCu) {
            fk.capture_ram[(addr - 0x802000u) / 4] = ((uint32_t)t[5] << 24) |
                ((uint32_t)t[6] << 16) | ((uint32_t)t[7] << 8) | t[8];
            break;
        }
        if ((addr >= 0x801000u && addr <= 0x801060u) ||
            (addr >= 0x101000u && addr <= 0x101060u)) {
            if ((addr < 0x800000u) != fk.proof_mapped) { fk.bad_reg++; fk.cmd_status = 1; break; }
            fk.proof_ram[(addr & 0xFFu) / 4] = ((uint32_t)t[5] << 24) |
                ((uint32_t)t[6] << 16) | ((uint32_t)t[7] << 8) | t[8];
            break;
        }
        if (addr != 0x80004Cu) { fk.bad_reg++; fk.cmd_status = 1; break; }
        fk.dcdc_freq_lf = ((uint32_t)t[5] << 24) | ((uint32_t)t[6] << 16) | ((uint32_t)t[7] << 8) | t[8];
        fk.dcdc_writes++;
        break;
    }
    case 0x012D:
        /* Model the proof's result, not an ARC emulator. Real execution is
           established separately on hardware. */
        if (fk.mode != 1 || t[2] != 0 || fk.proof_ram[0] != 0x600DB002u ||
            fk.proof_ram[2] != 0x101010u) { fk.cmd_status = 1; break; }
        fk.proof_mapped = true;
        if (!fk.proof_fail_result) fk.proof_ram[24] = 0x373F373Fu;
        break;
    case 0x0290:
        if (fk.mode == 1 && t[2] == 0xFF && t[7] == 0x0A && t[12] == 1) {
            fk.capture_fifo = true; break;
        }
        if (fk.mode != 1 || t[2] != 0xFF || t[7] != 0x92 || t[11] != 0xFF) {
            fk.bad_args++; fk.cmd_status = 1; break;
        }
        fk.capture_enabled = true;
        break;
    case 0x011F:
        fk.txfifo_n = 0;
        /* Keep cumulative write evidence across FIFO clears/refills. */
        break;
    case 0x0114: break;
    case 0x020F:                        /* SelPa: 0 is the LF PA */
        fk.pa_sel = t[2]; fk.pa_sel_n++;
        if (t[2] != 0) fk.hf_pa++;
        break;
    case 0x0202:                        /* SetPaConfig: PaSel in bit 7 of the first byte */
        memcpy(fk.pa_cfg, &t[2], 3); fk.pa_cfg_n++;
        if (t[2] & 0x80) fk.hf_pa++;
        break;
    case 0x0203: memcpy(fk.tx_params, &t[2], 2); fk.tx_params_n++; break;
    case 0x0293:                        /* Configure raw-IQ playback mode */
        if (fk.packet_type != 0x0A || t[8] != 0) { fk.bad_args++; fk.cmd_status = 1; break; }
        fk.tx_samples = (uint16_t)((t[2] << 8) | t[3]);
        fk.tx_rate = ((uint32_t)t[4] << 24) | ((uint32_t)t[5] << 16) |
                     ((uint32_t)t[6] << 8) | t[7];
        break;
    case 0x020D:                        /* SetTx: from standby or FS */
        if (fk.mode == 4 || fk.mode == 5) { fk.cmd_status = 0; fk.bad_mode++; break; }
        fk.tx_starts++;
        fk.tx_freq_hz = fk.last_freq_hz;
        if (fk.last_freq_hz >= 1000000000u || fk.pa_sel != 0 || (fk.pa_cfg[0] & 0x80)) fk.tx_hf++;
        fk.mode = 5;
        if (fk.tx_auto_done && (unsigned)fk.tx_samples * 2u <= 896u) fk.irq |= (1u << 19);
        break;
    case 0x029D: fk.mode = 4; fk.rx_arms++; break;
    case 0x0224: case 0x0225: break;
    case 0x0270: case 0x0271: case 0x0297: case 0x0298: case 0x029C: break;
    case 0x0220: case 0x0221: case 0x0223:          /* LoRa commands need the LoRa packet type */
        if (fk.packet_type != 0x00) { fk.cmd_status = 0; fk.bad_mode++; break; }
        if (op == 0x0220) { memcpy(fk.lora_mod, &t[2], 2); fk.lora_mod_set = true; }
        if (op == 0x0221) { memcpy(fk.lora_pkt, &t[2], 4); fk.lora_pkt_set = true; }
        if (op == 0x0223) { fk.lora_sync = t[2]; fk.lora_sync_set = true; }
        break;
    case 0x0240: case 0x0241: case 0x0244:          /* and GFSK ones the GFSK type */
        if (fk.packet_type != 0x02) { fk.cmd_status = 0; fk.bad_mode++; break; }
        if (op == 0x0240) { memcpy(fk.gfsk_mod, &t[2], 9); fk.gfsk_mod_set = true; }
        if (op == 0x0241) { memcpy(fk.gfsk_pkt, &t[2], 7); fk.gfsk_pkt_set = true; }
        if (op == 0x0244) { memcpy(fk.gfsk_sync, &t[2], 9); fk.gfsk_sync_set = true; }
        break;
    case 0x020C:
        /* Fails when the part is already in Rx (6.3.5). */
        if (fk.mode == 4) { fk.cmd_status = 0; fk.bad_mode++; break; }
        fk.mode = 4; fk.rx_arms++;
        break;
    case 0x0123: if (fk.mode == 4) fk.cmd_status = 0; break;   /* CalibFE refuses in Rx */
    case 0x0200: fk.last_freq_hz = ((uint32_t)t[2] << 24) | ((uint32_t)t[3] << 16) |
                                   ((uint32_t)t[4] << 8) | t[5]; break;
    case 0x0201: fk.last_path = t[2]; fk.last_boost = t[3]; break;
    case 0x0116: fk.irq &= ~(((uint32_t)t[2] << 24) | ((uint32_t)t[3] << 16) |
                             ((uint32_t)t[4] << 8) | t[5]); break;
    case 0x0111: fk.errors = 0; break;
    case 0x0112:                       /* DIO functions only in Standby RC (5.1.1) */
        if (fk.mode != 1 || t[2] < 5 || t[2] > 11) { fk.cmd_status = 0; fk.bad_mode++; break; }
        if (t[2] == 5 && (t[3] & 0xF) != 2) { fk.cmd_status = 1; break; }   /* DIO5: pull-up only, as on the chip */
        fk.dio_func[t[2]] = t[3];
        break;
    case 0x0113:
        if (t[2] < 5 || t[2] > 11) { fk.cmd_status = 1; break; }
        fk.dio_rfsw[t[2]] = t[3]; fk.dio_rfsw_set[t[2]] = true;
        break;
    case 0x0115:
        if (t[2] < 5 || t[2] > 11) { fk.cmd_status = 1; break; }
        fk.irq_dio = t[2];
        fk.irq_mask = ((uint32_t)t[3] << 24) | ((uint32_t)t[4] << 16) | ((uint32_t)t[5] << 8) | t[6];
        break;
    case 0x0122:                        /* Calibrate: not in Rx; ends in Standby RC and takes a while */
        if (fk.mode == 4) { fk.cmd_status = 0; fk.bad_mode++; break; }
        fk.calibrate_blocks = t[2];
        fk.mode = 1;
        fk_busy_for = 60;
        break;
    case 0x0206: fk.fallback = t[2]; break;
    case 0x0207:                        /* SetPacketType: Standby or FS only (8.1.1) */
        if (fk.mode == 4 || fk.mode == 5) { fk.cmd_status = 0; fk.bad_mode++; break; }
        fk.packet_type = t[2];
        break;
    case 0x021A:
        if (t[2] > 13) { fk.cmd_status = 1; break; }
        fk.agc = t[2];
        break;
    case 0x011E: fk.fifo_n = 0; break;
    /* OOK commands fail when the packet type is not OOK (16.3.2). */
    case 0x0281: case 0x0282: case 0x0284: case 0x0288: {
        if (fk.packet_type != 0x0A) { fk.cmd_status = 0; fk.bad_mode++; break; }
        if (op == 0x0281) { memcpy(fk.ook_mod, &t[2], 7); fk.ook_mod_set = true; }
        if (op == 0x0282) { memcpy(fk.ook_pkt, &t[2], 6); fk.ook_pkt_set = true; }
        if (op == 0x0284) { memcpy(fk.ook_sync, &t[2], 5); fk.ook_sync_set = true; }
        if (op == 0x0288) { memcpy(fk.ook_det, &t[2], 5); fk.ook_det_set = true; }
        break;
    }
    default: fk.cmd_status = 1; break;                          /* CMD_PERR */
    }
}

/* A 28-byte packet arrives: its chips join the Rx FIFO (256 bytes, anything
   beyond that is lost) and RxDone is raised. */
__attribute__((unused)) static void fk_push_frame(const uint8_t *frame28)
{
    for (int i = 0; i < 28; i++)
        if (fk.fifo_n < 256) fk.fifo[fk.fifo_n++] = frame28[i];
    fk.irq |= (1u << 18);
}

/* A LoRa or GFSK packet of `n` bytes arrives: into the Rx FIFO, its length
   for GetRxPktLength, and RxDone. */
__attribute__((unused)) static void fk_push_packet(const uint8_t *p, int n)
{
    for (int i = 0; i < n; i++)
        if (fk.fifo_n < 256) fk.fifo[fk.fifo_n++] = p[i];
    fk.rx_pkt_len = (uint16_t)n;
    fk.irq |= (1u << 18);
}

/* The transmit ends: TxDone, and the part drops to its fallback, Standby RC. */
__attribute__((unused)) static void fk_tx_complete(void)
{
    fk.irq |= (1u << 19);
    fk.mode = 1;
}

static void fk_respond_sx(const uint8_t *tx, uint8_t *rx, size_t len)
{
    uint8_t t[32] = { 0 };
    const size_t cl = len < sizeof(t) ? len : sizeof(t);
    memcpy(t, tx, cl);
    fk_log(t, cl, false);
    const uint8_t op = t[0];
    if (fk.kind == FK_SX_STATUS && len) memset(rx, fk.sx_status, len);
    if (op == 0xC0 && len >= 2) rx[1] = fk.sx_status;
    else if (op == 0x1D && len >= 6) { rx[4] = 0x14; rx[5] = 0x24; }
    fk_busy_for = 2;
}

static void fk_respond(const uint8_t *tx, uint8_t *rx, size_t len, void *ctx)
{
    (void)ctx;
    /* Direct reads drain while clocking, faster than the producer. Charge
       command/CS time before removing bytes, then accrue arrivals during the
       data phase afterwards. Charging the whole transfer before draining
       would incorrectly model an atomic FIFO pop at CS deassertion. */
    const bool timed_read = fk.capture_timed && len >= 2 && tx &&
                            tx[0] == 0 && tx[1] == 1;
    if (fk.capture_timed) {
        ls_shim_time_advance(timed_read ? 3 : (len + 1) / 2 + 2);
        fk_capture_advance();
    }
    if (!rx || !len) return;
    switch (fk.kind) {
    case FK_ALL_00: memset(rx, 0x00, len); fk_log(tx, len, false); fk_busy_for = 2; break;
    case FK_ALL_FF: memset(rx, 0xFF, len); fk_log(tx, len, false); fk_busy_for = 2; break;
    case FK_LR:
        fk_respond_lr(tx, rx, len);
        if (timed_read) {
            ls_shim_time_advance((len - 1) / 2);
            fk_capture_advance();
        }
        break;
    case FK_SX_ECHO: case FK_SX_STATUS: fk_respond_sx(tx, rx, len); break;
    }
}

/* Replace the part, then forget the previous answer, the way a swapped module
   or a fresh boot would. */
static void fk_install(fk_kind_t kind)
{
    fk_busy_step_us = 200;
    ls_shim_spi_reset_counters();
    memset(&fk, 0, sizeof(fk));
    fk.kind = kind;
    fk.packet_type = 0xFF;
    fk.fw_major = 1; fk.fw_minor = 0x18;
    fk.mode = 1; fk.reset_src = 2; fk.cmd_status = 2;     /* Stat 0x0421 after NRESET */
    fk.sx_status = 0x22;
    fk.thr_reg = (34u << 20) | 0x123u;      /* the chip's own level, and bits that are not ours */
    fk_busy_for = 0; fk_busy_reads = 0; fk_rst_writes = 0; fk_xl_gets = 0;
    fk_xl_log_n = 0; fk_ant_external = false;
    fk_dio1_level = false;
    ls_shim_spi_on_transfer(fk_respond, &fk);
    ls_lora_stop();
}

/* Frames sent since `from`, as a printable summary for failure messages and a
   matcher for the exact bytes. */
__attribute__((unused)) static bool fk_frame_is(int i, const uint8_t *bytes, size_t n)
{
    return i >= 0 && i < fk.nframes && fk.frames[i].len == n && fk.frames[i].full == n &&
           memcmp(fk.frames[i].b, bytes, n) == 0;
}

#define FK_FRAME(i, ...)                                                      \
    do {                                                                      \
        static const uint8_t exp_[] = { __VA_ARGS__ };                        \
        LS_CHECK_MSG(fk_frame_is((i), exp_, sizeof(exp_)),                    \
                     "frame %d is not the expected %u bytes (got %u bytes, "  \
                     "first %02X %02X)", (i), (unsigned)sizeof(exp_),         \
                     (i) < fk.nframes ? (unsigned)fk.frames[(i)].len : 0u,    \
                     (i) < fk.nframes ? fk.frames[(i)].b[0] : 0,              \
                     (i) < fk.nframes ? fk.frames[(i)].b[1] : 0);             \
    } while (0)

#endif /* LORA_FAKES_H */

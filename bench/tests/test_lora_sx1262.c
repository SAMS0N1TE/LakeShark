/* LS_TEST_SOURCES: ls_lora.c and ls_spi.c against the SPI and GPIO shims */

#include "ls_test.h"
#include "ls_lora.h"
#include "ls_spi.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_timer.h"

#include <string.h>

/* ---------------------------------------------------------------- fakes -- */

/* The expander carries reset; the driver only writes it. */
static int s_rst_writes;
bool ls_xl9535_out(uint8_t pin, bool level)
{
    (void)pin; (void)level;
    s_rst_writes++;
    return true;
}

/* Direction, which the driver sets once for DIO1 so a later change to the
   board bring-up cannot turn the radio's interrupt line into an output. The
   fake records it because "asking is possible" is the whole contract. */
static bool s_dio1_is_output = true;
bool ls_xl9535_set_dir(uint8_t pin, bool output)
{
    (void)pin;
    s_dio1_is_output = output;
    return true;
}

/* DIO1 as the part would drive it: the driver polls this for packet done.
   Low is the resting state, which is what a test that is not sending sees. */
static bool s_dio1_level;
bool ls_xl9535_get(uint8_t pin, bool *level)
{
    (void)pin;
    if (level) *level = s_dio1_level;
    return true;
}

/* BUSY, as the part would drive it: high for `s_busy_for` reads after each
   command, then low. */
static int s_busy_for;
static int s_busy_reads;

/* The driver configures BUSY as an input once at start; nothing here depends
   on what it asked for, only that asking is possible. */
esp_err_t gpio_config(const gpio_config_t *cfg) { (void)cfg; return ESP_OK; }
esp_err_t gpio_set_level(gpio_num_t pin, uint32_t level)
{
    (void)pin; (void)level;
    return ESP_OK;
}

int gpio_get_level(gpio_num_t pin)
{
    (void)pin;
    s_busy_reads++;
    ls_shim_time_advance(200);
    if (s_busy_for > 0) { s_busy_for--; return 1; }
    return 0;
}

/* ------------------------------------------------------------ responder -- */

/* What the part says back. Opcode 0xC0 is GetStatus, 0x1D is ReadRegister. */
typedef struct {
    uint8_t status;        /* the byte GetStatus returns                   */
    uint8_t reg[2];        /* the two bytes at the requested register      */
    int     commands;      /* how many transfers arrived                   */
    uint8_t last_opcode;
    size_t  last_len;
} part_t;

static void respond(const uint8_t *tx, uint8_t *rx, size_t len, void *ctx)
{
    part_t *p = (part_t *)ctx;
    p->commands++;
    p->last_opcode = len ? tx[0] : 0;
    p->last_len = len;

    if (!rx || !len) return;
    uint8_t op = tx[0];

    /* Full duplex: the reply lands in the same buffer, one byte later than
       the opcode, which is what the driver's offsets assume. */
    if (op == 0xC0 && len >= 2) {
        rx[1] = p->status;
    } else if (op == 0x1D && len >= 6) {
        /* opcode, addr hi, addr lo, NOP, then the data. */
        rx[4] = p->reg[0];
        rx[5] = p->reg[1];
    }
    /* Every command is answered with BUSY going high, as the part does. */
    s_busy_for = 2;
}

static part_t s_part;

static void bring_up(uint8_t status, uint8_t r0, uint8_t r1)
{
    ls_shim_spi_reset_counters();
    memset(&s_part, 0, sizeof(s_part));
    s_part.status = status;
    s_part.reg[0] = r0;
    s_part.reg[1] = r1;
    s_busy_for = 0;
    s_busy_reads = 0;
    s_rst_writes = 0;
    ls_shim_spi_on_transfer(respond, &s_part);
    /* start() returns early once it has succeeded, so a case that wants the
       liveness check to run has to forget the previous one first. */
    ls_lora_stop();
}


static uint8_t fsk_mod[9], fsk_packet[10], fsk_sync[4];
static int fsk_rx_starts, fsk_txs;
static bool fsk_ready;
static void respond_fsk(const uint8_t *tx, uint8_t *rx, size_t n, void *ctx)
{
    uint8_t op = tx[0];
    if (op == 0x8b && n == 9) memcpy(fsk_mod, tx, n);
    if (op == 0x8c && n == 10) memcpy(fsk_packet, tx, n);
    if (op == 0x0d && n == 4 && tx[1] == 6 && tx[2] >= 0xc0 && tx[2] < 0xc4)
        fsk_sync[tx[2] - 0xc0] = tx[3];
    if (op == 0x82) fsk_rx_starts++;
    if (op == 0x83) fsk_txs++;
    respond(tx, rx, n, ctx);
    if (!rx) return;
    if (op == 0x12 && n == 4) { rx[2] = 0; rx[3] = fsk_ready ? 2 : 0; }
    if (op == 0x13 && n == 4) { rx[2] = 64; rx[3] = 192; }
    if (op == 0x1e && n == 67) {
        for (int i = 0; i < 64; i++) rx[i + 3] = (uint8_t)i;
    }
    if (op == 0x14 && n == 5) { rx[2] = 0; rx[3] = 180; rx[4] = 190; }
}

LS_CASE(fsk_receive_preserves_lora_and_has_no_packet_restart)
{
    bring_up(0x22, 0x14, 0x24);
    ls_lora_cfg_t saved; ls_lora_cfg_default(&saved);
    LS_EQ_INT(ls_lora_configure(&saved), ESP_OK);
    LS_EQ_INT(ls_lora_receive(), ESP_OK);
    ls_shim_spi_on_transfer(respond_fsk, &s_part);
    fsk_ready = false; fsk_rx_starts = fsk_txs = 0;
    ls_fsk_cfg_t cfg = {929000000, 1200, 4500, 19500, 0x7cd215d8, 64};
    LS_EQ_INT(ls_lora_fsk_begin(&cfg), ESP_OK);
    LS_CHECK(ls_lora_fsk_active());
    LS_EQ_INT(fsk_mod[1], 0x0d); LS_EQ_INT(fsk_mod[2], 0x05); LS_EQ_INT(fsk_mod[3], 0x55);
    LS_EQ_INT(fsk_mod[5], 0x1d);
    LS_EQ_INT(fsk_packet[3], 0); LS_EQ_INT(fsk_packet[4], 32);
    LS_EQ_INT(fsk_packet[7], 64); LS_EQ_INT(fsk_packet[8], 1); LS_EQ_INT(fsk_packet[9], 0);
    LS_EQ_INT(fsk_sync[0], 0x7c); LS_EQ_INT(fsk_sync[3], 0xd8);
    LS_EQ_INT(ls_lora_configure(&saved), ESP_ERR_INVALID_STATE);
    LS_EQ_INT(ls_lora_scan_begin(902000000, 928000000), ESP_ERR_INVALID_STATE);
    uint8_t packet[64]; float rssi;
    LS_EQ_INT(ls_lora_send(packet, 64), ESP_ERR_INVALID_STATE);
    LS_EQ_INT(ls_lora_fsk_poll(packet, 64, &rssi), 0);
    fsk_ready = true;
    LS_EQ_INT(ls_lora_fsk_poll(packet, 63, &rssi), -1);
    LS_EQ_INT(ls_lora_fsk_poll(packet, 64, &rssi), 64);
    LS_EQ_INT(packet[0], 0); LS_EQ_INT(packet[63], 63); LS_NEAR(rssi, -95, 0.01);
    LS_EQ_INT(ls_lora_fsk_poll(packet, 64, &rssi), 64);
    LS_EQ_INT(fsk_rx_starts, 1); LS_EQ_INT(fsk_txs, 0);
    LS_EQ_INT(ls_lora_fsk_end(), ESP_OK);
    LS_CHECK(!ls_lora_fsk_active());
    LS_CHECK(ls_lora_is_receiving());
    LS_EQ_UINT(ls_lora_cfg()->freq_hz, saved.freq_hz);
    LS_EQ_UINT(ls_lora_cfg()->sf, saved.sf);
    cfg.bitrate = 512;
    LS_EQ_INT(ls_lora_fsk_begin(&cfg), ESP_ERR_INVALID_ARG);
    cfg.bitrate = 2400; cfg.bandwidth_hz = 4800;
    LS_EQ_INT(ls_lora_fsk_begin(&cfg), ESP_ERR_INVALID_ARG);
    cfg.bandwidth_hz = 19500;
    LS_EQ_INT(ls_lora_fsk_begin(&cfg), ESP_OK);
    LS_EQ_INT(fsk_mod[1], 0x06); LS_EQ_INT(fsk_mod[2], 0x82); LS_EQ_INT(fsk_mod[3], 0xab);
    LS_EQ_INT(ls_lora_fsk_end(), ESP_OK);
}

/* ---------------------------------------------------------------- cases -- */

LS_CASE(bringing_the_radio_up_takes_the_bus_and_releases_reset)
{
    /* 0x22 is what the board actually reports: standby, last command fine. */
    bring_up(0x22, 0x14, 0x24);
    LS_EQ_INT(ESP_OK, ls_lora_start());
    LS_CHECK(ls_lora_present());

    LS_CHECK_MSG(ls_shim_spi_bus_up(SPI2_HOST), "the radio bus was not opened");
    LS_CHECK_MSG(ls_shim_spi_devices() >= 1, "no device was registered");
    /* Reset is driven low then high; the driver owns that, not the board. */
    LS_CHECK_MSG(s_rst_writes >= 2,
                 "reset was written %d times, expected a pulse", s_rst_writes);
}

LS_CASE(probe_devices_can_be_removed_without_exhausting_the_shared_bus)
{
    LS_EQ_INT(ls_spi_bus(LS_SPI_RADIO),ESP_OK);
    unsigned before=ls_shim_spi_devices();
    for(int i=0;i<32;i++) {
        spi_device_handle_t dev=NULL;
        LS_EQ_INT(ls_spi_device(LS_SPI_RADIO,-1,0,1000000,1,&dev),ESP_OK);
        LS_EQ_UINT(ls_shim_spi_devices(),before+1);
        LS_EQ_INT(ls_spi_remove(LS_SPI_RADIO,dev),ESP_OK);
        LS_EQ_UINT(ls_shim_spi_devices(),before);
    }
    LS_EQ_INT(ls_spi_remove(LS_SPI_RADIO,NULL),ESP_ERR_INVALID_ARG);
}

LS_CASE(a_bus_that_is_already_open_is_not_opened_twice)
{
    /* Four parts share this bus and each will call ls_spi_bus. The real
       driver returns an error on a second initialise of the same host, so
       the caching is what keeps the second radio from failing to start. */
    bring_up(0x22, 0x14, 0x24);
    LS_EQ_INT(ESP_OK, ls_lora_start());
    unsigned devices = ls_shim_spi_devices();

    LS_EQ_INT(ESP_OK, ls_lora_start());
    LS_EQ_INT((int)devices, (int)ls_shim_spi_devices());
}

LS_CASE(every_command_waits_for_busy_to_fall_first)
{
    /* The handshake. Each command answered by the part raises BUSY for two
       reads, so a driver that waits will poll it and a driver that does not
       will show no reads at all beyond the first. */
    bring_up(0x22, 0x14, 0x24);
    LS_EQ_INT(ESP_OK, ls_lora_start());

    int before = s_busy_reads;
    uint8_t st = 0;
    LS_EQ_INT(ESP_OK, ls_lora_status(&st));
    LS_CHECK_MSG(s_busy_reads > before,
                 "a command was issued without reading BUSY");
}

LS_CASE(a_busy_line_stuck_high_is_reported_not_waited_on_forever)
{

    bring_up(0x22, 0x14, 0x24);
    LS_EQ_INT(ESP_OK, ls_lora_start());

    s_busy_for = 1000000;            /* never falls */
    uint8_t st = 0;
    esp_err_t err = ls_lora_status(&st);
    LS_CHECK_MSG(err != ESP_OK, "a stuck BUSY was reported as success");
    s_busy_for = 0;
}

LS_CASE(get_status_returns_the_byte_the_part_sent)
{
    bring_up(0x5A, 0x14, 0x24);
    LS_EQ_INT(ESP_OK, ls_lora_start());

    uint8_t st = 0;
    LS_EQ_INT(ESP_OK, ls_lora_status(&st));
    LS_EQ_INT(0x5A, st);
}

LS_CASE(a_part_that_drives_nothing_is_reported_as_absent)
{
    /* A floating MISO reads as all ones or all zeros depending on the pull.
       Reporting a status of zero as though it were real is how an absent
       radio looks configured. */
    bring_up(0x00, 0, 0);
    LS_CHECK_MSG(ls_lora_start() != ESP_OK,
                 "a status of 0x00 was accepted as a working part");

    bring_up(0xFF, 0, 0);
    LS_CHECK_MSG(ls_lora_start() != ESP_OK,
                 "a status of 0xFF was accepted as a working part");
}

LS_CASE(read_register_skips_the_status_byte_the_part_inserts)
{
    /* The second trap. ReadRegister is opcode, address high, address low,
       then one byte the part inserts before the data. Dropping that NOP
       shifts everything by one and the sync word reads as plausible rubbish.

       0x1424 is the value the board actually returns, so a driver reading it
       correctly here is reading it the same way it does on hardware. */
    bring_up(0x22, 0x14, 0x24);
    LS_EQ_INT(ESP_OK, ls_lora_start());

    uint8_t sync[2] = { 0, 0 };
    LS_EQ_INT(ESP_OK, ls_lora_read_reg(0x0740, sync, sizeof(sync)));
    LS_EQ_INT(0x14, sync[0]);
    LS_EQ_INT(0x24, sync[1]);
}

LS_CASE(read_register_sends_the_address_the_caller_asked_for)
{
    /* Big-endian, high byte first. A byte-swapped address reads a different
       register that also exists and also returns something. */
    bring_up(0x22, 0x14, 0x24);
    LS_EQ_INT(ESP_OK, ls_lora_start());

    uint8_t buf[2];
    ls_lora_read_reg(0x0740, buf, sizeof(buf));

    size_t len = 0;
    const uint8_t *tx = ls_shim_spi_last_tx(&len);
    LS_CHECK_MSG(len >= 3, "the read was only %d bytes", (int)len);
    LS_EQ_INT(0x1D, tx[0]);
    LS_EQ_INT(0x07, tx[1]);
    LS_EQ_INT(0x40, tx[2]);
}

LS_CASE(a_read_larger_than_the_command_buffer_is_refused)
{
    /* The driver builds its transfer in a fixed buffer. A caller asking for
       more than fits has to be told, not quietly given a short read. */
    bring_up(0x22, 0x14, 0x24);
    LS_EQ_INT(ESP_OK, ls_lora_start());

    uint8_t big[64];
    LS_CHECK_MSG(ls_lora_read_reg(0x0740, big, sizeof(big)) != ESP_OK,
                 "an oversized read was accepted");
}

LS_CASE(nothing_talks_to_the_part_before_it_is_started)
{
    /* The console can ask for status at any time. */
    ls_shim_spi_reset();
    memset(&s_part, 0, sizeof(s_part));
    ls_shim_spi_on_transfer(respond, &s_part);

    /* No start call: a command must not reach the bus. */
    uint8_t buf[2];
    ls_lora_read_reg(0x0740, buf, sizeof(buf));
    LS_EQ_INT(0, (int)s_part.commands);
}

LS_CASE(the_mesh_default_spreading_factor_stays_at_seven)
{
    ls_lora_cfg_t cfg;
    memset(&cfg, 0xAA, sizeof(cfg));
    ls_lora_cfg_default(&cfg);

    LS_CHECK_MSG(cfg.sf == 7,
                 "default SF is %u, not 7 - SF8 cannot hear the stock "
                 "'Canada, USA' MeshCore preset and has been reverted here "
                 "three times; read the note above this case", cfg.sf);

    /* The rest of the preset, pinned with it: on their own each is harmless
       and together they are what makes a node audible. */
    LS_EQ_UINT(910525000u, cfg.freq_hz);
    LS_EQ_UINT(62500u, cfg.bw_hz);
    LS_EQ_UINT(5u, cfg.cr);

    /* MeshCore's own rule, from RadioLibWrappers.h: 32 symbols at or below
       SF8. SF7 is below it, so this must not have moved either. */
    LS_EQ_UINT(32u, cfg.preamble);
}

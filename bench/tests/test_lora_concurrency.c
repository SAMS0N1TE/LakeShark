/* LS_TEST_SOURCES: ls_lora.c, ls_lora_sx126x.c, ls_lora_lr20xx.c and ls_spi.c: two tasks on one LoRa SPI device, against an LR2021 fake that notices interleaved frames */

/* On the board the console (`lora`) and the ADS-B pump (the Mode S session)
   both reached the one spi_device_handle_t, and the IDF asserted inside
   spi_device_transmit. The LR20xx's reads are two frames with a BUSY wait
   between them, so even where the driver tolerates overlap, a frame from
   another task between the halves is read back as the answer. This runs real
   threads against the real ls_lora_* calls and a fake that counts both. */

#include <pthread.h>
#include <stdatomic.h>
#include <sched.h>
#include <time.h>

#include "lora_fakes.h"
#include "ls_lora_lr20xx.h"
#include "ls_lora_priv.h"

/* ------------------------------------------------- the interleave detector -- */

static pthread_mutex_t fake_mtx = PTHREAD_MUTEX_INITIALIZER;   /* the fake's own state only */
static atomic_int  in_flight;            /* tasks inside spi_device_transmit right now      */
static atomic_int  max_in_flight;
static atomic_int  interleaved;          /* a frame between a read's two halves, or an orphan half */
static atomic_int  total_frames;
static atomic_int  go;

static void spin_us(int us)
{
    struct timespec t0, t;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    do {
        clock_gettime(CLOCK_MONOTONIC, &t);
    } while ((t.tv_sec - t0.tv_sec) * 1000000L + (t.tv_nsec - t0.tv_nsec) / 1000L < us);
    sched_yield();
}

/* The part, as seen by two tasks. Wraps the single-threaded LR2021 model in
   lora_fakes.h. A read's second frame is all zeros (the part clocks the answer
   out); any other frame between a read's first frame and that one is the bug,
   and so is a lone zero frame with no read pending. */
static void conc_respond(const uint8_t *tx, uint8_t *rx, size_t len, void *ctx)
{
    const int now = atomic_fetch_add(&in_flight, 1) + 1;
    int seen = atomic_load(&max_in_flight);
    while (now > seen && !atomic_compare_exchange_weak(&max_in_flight, &seen, now)) { }

    pthread_mutex_lock(&fake_mtx);
    const bool zero_frame = len >= 2 && tx[0] == 0 && tx[1] == 0;
    if (fk.read_pending && !zero_frame) atomic_fetch_add(&interleaved, 1);
    if (!fk.read_pending && zero_frame) atomic_fetch_add(&interleaved, 1);
    const bool opens_read = len >= 2 && !fk.read_pending && !zero_frame;
    fk_respond(tx, rx, len, ctx);
    const bool now_pending = fk.read_pending;
    atomic_fetch_add(&total_frames, 1);
    pthread_mutex_unlock(&fake_mtx);

    /* Linger inside the transaction, and longer after the first half of a
       read: that is the window another task has to land a frame in. */
    spin_us(opens_read && now_pending ? 120 : 30);
    atomic_fetch_sub(&in_flight, 1);
}

static void conc_reset(void)
{
    atomic_store(&in_flight, 0);
    atomic_store(&max_in_flight, 0);
    atomic_store(&interleaved, 0);
    atomic_store(&total_frames, 0);
    atomic_store(&go, 0);
}

static void wait_go(void)
{
    while (!atomic_load(&go)) sched_yield();
}

/* A LR2021 in a Mode S session, and the fake switched to the concurrent
   responder. */
static void session_with_two_task_fake(void)
{
    fk_install(FK_LR);
    LS_EQ_INT(ls_lora_start(), ESP_OK);
    fk_install(FK_LR);
    LS_EQ_INT(ls_lora_start(), ESP_OK);
    LS_EQ_INT(ls_lora_modes_begin(1090000000u, 13), ESP_OK);
    fk.nframes = 0;
    fk.rssi_hi = 57; fk.rssi_lo = 0x80;       /* GetRssiInst: -57.5 dBm */
    conc_reset();
    ls_shim_spi_on_transfer(conc_respond, &fk);
}

/* ------------------------------------------------------------ the hammer --- */

#define ROUNDS 150

static int  console_bad;       /* answers the console got that were not the part's */
static int  console_calls;
static int  pump_pushed, pump_delivered, pump_bad;
/* Frames a retune cleared from the part's Rx FIFO, and the retunes. */
static int  pump_lost, knob_calls;

/* A knob change is standby, reprogram, ClearRxFifo, Rx again (the sequence
   test_lr20xx_proto.c pins), so a frame waiting in the FIFO then is lost.
   Every frame that does arrive must be whole, one of ours, and later than
   the last. */
static void frame_for(int k, uint8_t out[28]);
static void take(const uint8_t *buf, int *expect)
{
    uint8_t want[28];
    for (int k = *expect; k < pump_pushed; k++) {
        frame_for(k, want);
        if (memcmp(buf, want, 28) == 0) {
            pump_lost += k - *expect;
            *expect = k + 1;
            pump_delivered++;
            return;
        }
    }
    pump_bad++;
}

static void frame_for(int k, uint8_t out[28])
{
    for (int i = 0; i < 28; i++) out[i] = (uint8_t)(k * 7 + i);
}

static void *console_thread(void *arg)
{
    (void)arg;
    wait_go();
    for (int i = 0; i < ROUNDS; i++) {
        float dbm = 0;
        const esp_err_t e = ls_lora_rssi_inst(&dbm);
        console_calls++;
        if (e != ESP_OK || dbm != -57.5f) console_bad++;

        uint8_t st = 0;
        if (ls_lora_status(&st) != ESP_OK) console_bad++;
        console_calls++;

        /* The `adsb lr` knobs, applied live while the pump polls: each is
           standby, reprogram, Rx again, and the pump must never see the part
           in between. */
        if (i % 10 == 5) {
            if (ls_lora_modes_set_boost((i / 10) % 8) != ESP_OK) console_bad++;
            if (ls_lora_modes_set_gain(1 + (i / 10) % 13) != ESP_OK) console_bad++;
            knob_calls += 2;
        }

        /* The `lora` command itself; its output is not what is under test. */
        if (i % 50 == 0) ls_lora_diagnostics();
    }
    return NULL;
}

static void *pump_thread(void *arg)
{
    (void)arg;
    wait_go();
    uint8_t buf[28];
    float rssi;
    int expect = 0;
    for (int i = 0; i < ROUNDS * 3; i++) {
        if (i % 3 == 0) {
            pthread_mutex_lock(&fake_mtx);
            if (fk.fifo_n <= 224) {
                uint8_t f[28];
                frame_for(pump_pushed, f);
                fk_push_frame(f);
                pump_pushed++;
            }
            pthread_mutex_unlock(&fake_mtx);
        }
        const int n = ls_lora_modes_poll(buf, sizeof(buf), &rssi);
        if (n == 28) {
            take(buf, &expect);
        } else if (n < 0) {
            pump_bad++;
        }
    }
    /* Drain what is left. */
    for (int i = 0; i < 40 && pump_delivered + pump_lost < pump_pushed; i++) {
        const int n = ls_lora_modes_poll(buf, sizeof(buf), &rssi);
        if (n == 28) take(buf, &expect);
    }
    /* Frames after the last one delivered that never came. */
    pump_lost += pump_pushed - expect;
    return NULL;
}

LS_CASE(the_console_and_the_ads_b_pump_never_interleave_on_the_spi_device)
{
    session_with_two_task_fake();
    console_bad = console_calls = 0;
    pump_pushed = pump_delivered = pump_bad = pump_lost = knob_calls = 0;

    pthread_t a, b;
    pthread_create(&a, NULL, console_thread, NULL);
    pthread_create(&b, NULL, pump_thread, NULL);
    atomic_store(&go, 1);
    pthread_join(a, NULL);
    pthread_join(b, NULL);

    LS_CHECK_MSG(atomic_load(&total_frames) > 1000, "only %d frames crossed the bus", atomic_load(&total_frames));
    LS_EQ_INT(atomic_load(&max_in_flight), 1);       /* never two inside spi_device_transmit */
    LS_EQ_INT(atomic_load(&interleaved), 0);         /* never a frame between a read's halves */
    LS_EQ_INT(fk.violations, 0);                     /* never a frame while BUSY was high */
    LS_EQ_INT(console_bad, 0);                       /* every answer was the part's own */
    LS_EQ_INT(pump_bad, 0);                          /* frames arrived whole and in order */
    LS_EQ_INT(pump_delivered + pump_lost, pump_pushed);
    /* Only a retune may lose a frame: the one waiting when it cleared the FIFO. */
    LS_CHECK_MSG(pump_lost <= knob_calls, "%d frames lost to %d retunes", pump_lost, knob_calls);
    LS_CHECK(pump_pushed >= ROUNDS);
    LS_EQ_UINT(ls_lora_hw_unguarded(), 0);
    lr20xx_modes_stats_t ms;
    lr20xx_modes_stats(&ms);
    LS_EQ_UINT(ms.bus_errors, 0);
    LS_EQ_UINT(ms.resets, 0);
    LS_EQ_UINT(ms.rearms, 0);                         /* the pump never found the part out of Rx */
    LS_EQ_INT(fk.bad_mode, 0);                        /* nothing was sent in a mode that refuses it */
    LS_EQ_INT(fk.bad_args, 0);
    ls_lora_modes_tuning_t t;
    LS_EQ_INT(ls_lora_modes_tuning(&t), ESP_OK);      /* the last change is what the part holds */
    LS_EQ_INT(t.boost, 6);
    LS_EQ_INT(t.gain_step, 2);
    LS_EQ_UINT(fk.last_boost, 6);
    LS_EQ_UINT(fk.agc, 2);
    LS_EQ_UINT(fk.mode, LR20XX_MODE_RX);
    ls_shim_spi_on_transfer(fk_respond, &fk);
}

/* ------------------------------------------------- the detector has teeth -- */

static void *raw_reader(void *arg)
{
    wait_go();
    const bool locked = *(const bool *)arg;
    for (int i = 0; i < 60; i++) {
        uint8_t f1[2] = { 0x01, 0x01 };            /* GetVersion, frame 1 */
        uint8_t f2[4] = { 0, 0, 0, 0 };            /* frame 2 */
        spi_transaction_t t1 = { .length = 16, .tx_buffer = f1, .rx_buffer = f1 };
        spi_transaction_t t2 = { .length = 32, .tx_buffer = f2, .rx_buffer = f2 };
        if (locked) ls_lora_hw_lock();
        spi_device_transmit(ls_lora_hw_dev(), &t1);
        spi_device_transmit(ls_lora_hw_dev(), &t2);
        if (locked) ls_lora_hw_unlock();
    }
    return NULL;
}

static int raw_pair_run(bool locked)
{
    session_with_two_task_fake();
    LS_EQ_INT(ls_lora_modes_end(), ESP_OK);
    conc_reset();
    fk.read_pending = false;
    fk_busy_for = 0;
    static bool flag_locked, flag_free;
    flag_locked = true; flag_free = false;
    bool *which = locked ? &flag_locked : &flag_free;

    pthread_t a, b;
    pthread_create(&a, NULL, raw_reader, which);
    pthread_create(&b, NULL, raw_reader, which);
    atomic_store(&go, 1);
    pthread_join(a, NULL);
    pthread_join(b, NULL);
    const int bad = atomic_load(&interleaved);
    ls_shim_spi_on_transfer(fk_respond, &fk);
    return bad;
}

LS_CASE(the_fake_does_catch_two_tasks_that_share_the_device_without_the_lock)
{
    /* Two tasks, each clocking GetVersion's two frames, nothing between them
       and the part. Unguarded the fake sees frames land between the halves,
       and two tasks inside the transmit at once; under ls_lora_hw_lock it sees
       neither. This is the control for the case above: it is what that case
       would report if the dispatcher did not take the lock. */
    /* Overlap without the lock is likely, not certain: on a loaded machine the
       two threads can happen to take turns. One sighting is all the control
       needs, so it gets ten tries. */
    int free_run = 0, peak = 0;
    for (int tries = 0; tries < 10 && (free_run == 0 || peak <= 1); tries++) {
        free_run += raw_pair_run(false);
        if (atomic_load(&max_in_flight) > peak) peak = atomic_load(&max_in_flight);
    }
    LS_CHECK_MSG(free_run > 0, "the fake saw no interleaving without the lock (%d)", free_run);
    LS_CHECK(peak > 1);

    const int locked_run = raw_pair_run(true);
    LS_EQ_INT(locked_run, 0);
    LS_EQ_INT(atomic_load(&max_in_flight), 1);
}

/* -------------------------------------------- every path is under the lock -- */

LS_CASE(every_public_path_reaches_the_bus_with_the_lock_held)
{
    /* Deterministic, no threads: ls_lora_hw_transmit counts a transaction made
       by a task that does not hold the lock, so a sequence that skipped it
       would show here whichever other task happened to be running. */
    fk_install(FK_LR);
    LS_EQ_INT(ls_lora_start(), ESP_OK);
    fk_install(FK_LR);
    LS_EQ_INT(ls_lora_start(), ESP_OK);
    LS_EQ_UINT(ls_lora_hw_unguarded(), 0);

    uint8_t st; float dbm, rssi; uint8_t buf[28];
    (void)ls_lora_status(&st);
    LS_EQ_INT(ls_lora_modes_begin(1090000000u, 13), ESP_OK);
    (void)ls_lora_rssi_inst(&dbm);
    (void)ls_lora_modes_poll(buf, sizeof(buf), &rssi);
    uint8_t f[28]; frame_for(1, f);
    fk_push_frame(f);
    LS_EQ_INT(ls_lora_modes_poll(buf, sizeof(buf), &rssi), 28);
    LS_EQ_INT(ls_lora_modes_set_gain(5), ESP_OK);
    ls_lora_diagnostics();
    LS_EQ_INT(ls_lora_modes_end(), ESP_OK);
    ls_lora_diagnostics();
    (void)ls_lora_read_reg(0x0740, buf, 2);
    (void)ls_lora_send(buf, 4);
    (void)ls_lora_send_done();
    (void)ls_lora_poll(buf, sizeof(buf), &rssi, &dbm);
    (void)ls_lora_receive();
    (void)ls_lora_scan_begin(902000000u, 928000000u);
    float bins[LS_LORA_SCAN_BINS];
    bool done;
    (void)ls_lora_scan_pass(bins, LS_LORA_SCAN_BINS, &done);
    (void)ls_lora_scan_sweep(bins, LS_LORA_SCAN_BINS);
    (void)ls_lora_scan_end();
    LS_EQ_UINT(ls_lora_hw_unguarded(), 0);
    LS_CHECK(!ls_lora_hw_locked_by_me());       /* and every call gave it back */

    /* The packet paths, each of which really reaches the bus here. */
    ls_lora_cfg_t cfg; ls_lora_cfg_default(&cfg);
    LS_EQ_INT(ls_lora_configure(&cfg), ESP_OK);
    LS_EQ_INT(ls_lora_receive(), ESP_OK);
    (void)ls_lora_rssi_inst(&dbm);
    (void)ls_lora_poll(buf, sizeof(buf), &rssi, &dbm);
    LS_EQ_INT(ls_lora_send(buf, 4), ESP_OK);
    (void)ls_lora_send_done();
    fk.irq |= 1u << 19;                         /* TxDone */
    fk.mode = LR20XX_MODE_STBY_RC;
    LS_CHECK(ls_lora_send_done());
    LS_EQ_INT(ls_lora_receive(), ESP_OK);
    LS_EQ_INT(ls_lora_modes_begin(1090000000u, 13), ESP_OK);
    LS_EQ_INT(ls_lora_modes_end(), ESP_OK);
    ls_fsk_cfg_t fsk = { .freq_hz = 868000000, .bitrate = 2400, .deviation_hz = 4800,
                         .bandwidth_hz = 19500, .sync_word = 0x7cd215d8, .payload_bytes = 8 };
    LS_EQ_INT(ls_lora_fsk_begin(&fsk), ESP_OK);
    (void)ls_lora_fsk_poll(buf, sizeof(buf), &rssi);
    LS_EQ_INT(ls_lora_fsk_send(buf, 4), ESP_OK);
    fk.irq |= 1u << 19;
    fk.mode = LR20XX_MODE_STBY_RC;
    LS_CHECK(ls_lora_send_done());
    LS_EQ_INT(ls_lora_fsk_receive(), ESP_OK);
    LS_EQ_INT(ls_lora_fsk_end(), ESP_OK);
    LS_EQ_INT(ls_lora_scan_begin(902000000u, 928000000u), ESP_OK);
    LS_EQ_INT(ls_lora_scan_pass(bins, LS_LORA_SCAN_BINS, &done), LS_LORA_SCAN_BINS);
    LS_EQ_INT(ls_lora_scan_sweep(bins, LS_LORA_SCAN_BINS), LS_LORA_SCAN_BINS);
    LS_EQ_INT(ls_lora_scan_end(), ESP_OK);
    ls_lora_diagnostics();
    LS_EQ_UINT(ls_lora_hw_unguarded(), 0);
    LS_CHECK(!ls_lora_hw_locked_by_me());
    LS_EQ_INT(fk.violations, 0);
}

LS_CASE(an_sx126x_path_is_under_the_lock_too)
{
    fk_install(FK_SX_ECHO);
    LS_EQ_INT(ls_lora_start(), ESP_OK);
    fk_install(FK_SX_ECHO);
    LS_EQ_INT(ls_lora_start(), ESP_OK);
    LS_EQ_UINT(ls_lora_hw_unguarded(), 0);

    ls_lora_cfg_t cfg; ls_lora_cfg_default(&cfg);
    (void)ls_lora_configure(&cfg);
    uint8_t st, buf[32]; float rssi, snr, dbm;
    (void)ls_lora_status(&st);
    (void)ls_lora_read_reg(0x0740, buf, 2);
    (void)ls_lora_receive();
    (void)ls_lora_rssi_inst(&dbm);
    (void)ls_lora_poll(buf, sizeof(buf), &rssi, &snr);
    (void)ls_lora_send(buf, 8);
    (void)ls_lora_send_done();
    (void)ls_lora_receive();
    ls_fsk_cfg_t fsk = { .freq_hz = 929000000, .bitrate = 1200, .deviation_hz = 4500,
                         .bandwidth_hz = 19500, .sync_word = 0x7cd215d8, .payload_bytes = 8 };
    (void)ls_lora_fsk_begin(&fsk);
    (void)ls_lora_fsk_receive();
    (void)ls_lora_fsk_poll(buf, sizeof(buf), &rssi);
    (void)ls_lora_fsk_send(buf, 4);
    (void)ls_lora_fsk_end();
    (void)ls_lora_scan_begin(902000000u, 928000000u);
    float bins[LS_LORA_SCAN_BINS];
    bool done;
    (void)ls_lora_scan_pass(bins, LS_LORA_SCAN_BINS, &done);
    (void)ls_lora_scan_sweep(bins, LS_LORA_SCAN_BINS);
    (void)ls_lora_scan_end();
    ls_lora_diagnostics();
    LS_EQ_UINT(ls_lora_hw_unguarded(), 0);
    LS_CHECK(!ls_lora_hw_locked_by_me());
}

/* ------------------------------------------------------------ diagnostics -- */

static bool only_get_status_since(int from)
{
    if (fk.nframes <= from) return false;
    for (int i = from; i < fk.nframes; i++) {
        if (fk.frames[i].read2) return false;
        if (fk.frames[i].b[0] != 0x01 || fk.frames[i].b[1] != 0x00) return false;
    }
    return true;
}

LS_CASE(diagnostics_during_a_mode_s_session_sends_only_get_status)
{
    fk_install(FK_LR);
    LS_EQ_INT(ls_lora_start(), ESP_OK);
    fk_install(FK_LR);
    LS_EQ_INT(ls_lora_start(), ESP_OK);
    LS_EQ_INT(ls_lora_modes_begin(1090000000u, 13), ESP_OK);
    const int mode_before = fk.mode;
    const int arms_before = fk.rx_arms;
    fk.nframes = 0;

    ls_lora_diagnostics();
    LS_CHECK_MSG(only_get_status_since(0), "diagnostics sent more than GetStatus (%d frames)", fk.nframes);
    LS_EQ_INT(fk.nframes, 1);
    LS_EQ_INT(fk.mode, mode_before);              /* the chip was not moved out of Rx */
    LS_EQ_INT(fk.rx_arms, arms_before);
    LS_EQ_INT(fk.bad_mode, 0);
    LS_EQ_INT(fk.violations, 0);

    /* Outside a session it reads the version and the error word as before. */
    LS_EQ_INT(ls_lora_modes_end(), ESP_OK);
    fk.nframes = 0;
    ls_lora_diagnostics();
    LS_CHECK(fk.nframes >= 5);                    /* GetStatus, GetVersion x2, GetErrors x2 */
}

LS_CASE(a_status_read_by_the_console_does_not_hide_a_chip_reset_from_the_session)
{
    fk_install(FK_LR);
    LS_EQ_INT(ls_lora_start(), ESP_OK);
    fk_install(FK_LR);
    LS_EQ_INT(ls_lora_start(), ESP_OK);
    LS_EQ_INT(ls_lora_modes_begin(1090000000u, 13), ESP_OK);

    fk.reset_src = 2;                             /* the part restarted under the session */
    ls_lora_diagnostics();                        /* its GetStatus clears the reset source */
    LS_EQ_INT(fk.reset_src, 0);

    uint8_t buf[28]; float rssi;
    (void)ls_lora_modes_poll(buf, sizeof(buf), &rssi);
    lr20xx_modes_stats_t ms;
    lr20xx_modes_stats(&ms);
    LS_EQ_UINT(ms.resets, 1);                     /* the poll still saw it, and reprogrammed */
    LS_EQ_UINT(fk.mode, LR20XX_MODE_RX);
}

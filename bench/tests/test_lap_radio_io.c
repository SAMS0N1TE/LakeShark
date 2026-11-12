/* Exercise production runtime with callbacks disabled during worker passes. */
#define main phase1_unused_main
#include "test_lap_phase1.c"
#undef main
#include "ls_lap_radio_io.h"
#include "tx_protection.h"
extern uint32_t (*ls_lap_api_host_clock)(void);
static uint32_t tick_ms = 1000;
static uint32_t io_clock(void) { return tick_ms; }
static void pump_io(unsigned n) {
    ls_lap_api_callback(false);
    for (unsigned i = 0; i < n; i++) {
        tick_ms += 40;
        ls_lap_radio_service();
    }
    ls_lap_api_callback(true);
}
static lap_packet_config_t pc(void) {
    return (lap_packet_config_t){.size = sizeof(lap_packet_config_t),
                                 .version = 1,
                                 .frequency_hz = 406050000,
                                 .modulation = LAP_MOD_LORA,
                                 .sf = 7,
                                 .bandwidth_hz = 125000,
                                 .coding_rate = 5,
                                 .preamble = 8,
                                 .crc = 1,
                                 .sync_word = 0x12,
                                 .power_dbm = 0};
}
static void start(unsigned caps) {
    ls_lap_sys_host()->hardware = LAP_HW_SD;
    begin("io-test", 1);
    lap_app_t a = {.flags = caps};
    strcpy(a.id, "io-test");
    ls_lap_radio_begin(&a);
    pump_io(1);
}
static void finish(void) {
    end();
    pump_io(1);
}
static void packets(void) {
    start(0);
    lap_packet_config_t c = pc();
    assert(lap_packet_open(0, &c) == LAP_RX_EACCES);
    assert(lap_tx_packet(0, &c, (char[]){"x"}, 1) == LAP_RX_EACCES);
    finish();
    start(LAP_CAP_RADIO_RX);
    lap_caps_t caps;
    assert(!lap_caps(&caps, sizeof(caps)) && (caps.hardware & LAP_HW_PACKET));
    assert(lap_packet_open(0, (const void *)1) == LAP_RX_EINVAL);
    int h = lap_packet_open(0, &c);
    assert(h > 0);
    c.frequency_hz = 1;
    pump_io(1);
    assert(ls_lap_io_sim_command("packet=hello"));
    assert(ls_lap_io_sim_command("iorssi=-674"));
    pump_io(1);
    char data[255];
    lap_packet_meta_t m;
    assert(lap_packet_read(h, data, 2, &m) == LAP_IO_ESIZE);
    assert(lap_packet_read(h, data, 255, &m) == 5);
    assert(!memcmp(data, "hello", 5) && m.frequency_hz == 406050000 && m.rssi_tenths_dbm == -674 &&
           m.timestamp_us > 0 && (m.flags & LAP_DATA_SYNTHETIC));
    for (int i = 0; i < 12; i++) {
        ls_lap_io_sim_command("packet=overflow");
        pump_io(1);
    }
    lap_io_status_t s;
    assert(!lap_packet_status(h, &s) && s.dropped >= 4 && s.queued == 8);
    lap_rx_config_t iqc = {
        .center_hz = 433920000, .rate_hz = 48000, .gain_tenths_db = LAP_GAIN_AUTO};
    assert(lap_rx_open(0, &iqc) == LAP_RX_EBUSY);
    lap_packet_close(h);
    pump_io(1);
    assert(lap_packet_read(h, data, 255, &m) == LAP_RX_EINVAL);
    c = pc();
    c.modulation = LAP_MOD_FSK;
    c.bitrate = 512;
    c.sync_bits = 32;
    c.payload_bytes = 8;
    c.crc = 1;
    h = lap_packet_open(0, &c);
    assert(h > 0);
    pump_io(1);
    assert(!lap_packet_status(h, &s) && s.error == LAP_RX_EUNSUPPORTED);
    lap_packet_close(h);
    pump_io(1);
    c.crc = 0;
    h = lap_packet_open(0, &c);
    assert(h > 0);
    pump_io(1);
    ls_lap_io_sim_command("packet=FSK");
    pump_io(1);
    assert(lap_packet_read(h, data, 255, &m) == 3);
    lap_packet_close(h);
    pump_io(1);
    c.modulation = LAP_MOD_OOK;
    c.bitrate=4800;
    c.sync_bits = 8;
    h = lap_edges_open(0, &c);
    assert(h > 0);
    pump_io(1);
    ls_lap_io_sim_command("edges=100,-200,300,-400");
    pump_io(1);
    int32_t pulses[256];
    assert(lap_edges_read(h, pulses, 2, &m) == LAP_IO_ESIZE);
    assert(lap_edges_read(h, pulses, 256, &m) == 4 && pulses[1] == -200);
    lap_packet_close(h);
    pump_io(1);
    ls_lap_io_sim_command("iomodes=0");
    c = pc();
    assert(lap_packet_open(0, &c) == LAP_RX_EUNSUPPORTED);
    ls_lap_io_sim_command("iomodes=255");
    finish();
}
static void scanning(void) {
    start(LAP_CAP_RADIO_RX);
    lap_scan_config_t c = {.size = sizeof(c),
                           .version = 1,
                           .radio = 0,
                           .receiver = pc(),
                           .count = 2,
                           .frequencies = {433920000, 434045000},
                           .dwell_ms = 80,
                           .hang_ms = 80,
                           .squelch_tenths_dbm = -700};
    ls_lap_io_sim_command("iorssi=-1000");
    int h = lap_scan_start(&c);
    assert(h > 0);
    pump_io(1);
    lap_scan_status_t s;
    assert(!lap_scan_status(h, &s) && s.frequency_hz == 433920000);
    pump_io(3);
    assert(!lap_scan_status(h, &s) && s.channel == 1);
    ls_lap_io_sim_command("iorssi=-650");
    pump_io(1);
    lap_packet_meta_t hit;
    assert(lap_scan_next(h, &hit) == 1 && hit.rssi_tenths_dbm == -650);
    assert(lap_scan_next(h, &hit) == 0);
    assert(!lap_scan_control(h, LAP_SCAN_HOLD));
    pump_io(4);
    assert(!lap_scan_status(h, &s) && s.held);
    assert(!lap_scan_control(h, LAP_SCAN_NEXT));
    pump_io(1);
    ls_lap_io_sim_command("iorssi=-1000");
    pump_io(1);
    assert(!lap_scan_status(h, &s) && !s.held);
    assert(!lap_scan_control(h, LAP_SCAN_SKIP));
    pump_io(1);
    assert(!lap_scan_control(h, LAP_SCAN_RESUME));
    lap_scan_stop(h);
    pump_io(1);
    finish();
}
static void database(void) {
    _mkdir("sdcard");
    _mkdir("sdcard/channels");
    start(LAP_CAP_RADIO_RX);
    lap_channel_t r[2] = {0};
    for (int i = 0; i < 2; i++) {
        r[i].size = sizeof(r[i]);
        r[i].version = 1;
        r[i].frequency_hz = 433920000 + i * 125000;
        r[i].mode = 1;
        r[i].flags = 1;
        r[i].bank = 2;
        r[i].tone = 300;
        strcpy(r[i].name, i ? "second" : "first");
    }
    /* The bank name from the app's own memory, as an app passes it: the
       runtime refuses pointers that are neither the app's nor its stack,
       and a literal in this test program is neither (on Linux). */
    char bank[] = "sdk-test", escape[] = "../escape";
    assert(lap_channels_write(escape, r, 2) == LAP_RX_EINVAL);
    int h = lap_channels_write(bank, r, 2);
    assert(h > 0);
    r[0].frequency_hz = 1;
    pump_io(1);
    lap_io_status_t s;
    assert(!lap_io_status(h, &s) && s.state == LAP_IO_DONE);
    h = lap_channels_query(bank, 1, 64);
    assert(h > 0);
    assert(lap_channels_read(h, r, 2) == LAP_RX_EAGAIN);
    pump_io(1);
    assert(lap_channels_read(h, r, 2) == 1 && r[0].frequency_hz == 434045000 && r[0].tone == 300 &&
           r[0].bank == 2);
    remove("sdcard/channels/sdk-test.bin");
    remove("sdcard/channels/sdk-test.bak");
    finish();
}
static void recordings(void) {
    start(LAP_CAP_RADIO_RX);
    remove("lap-sd-io/apps_data/io-test/packets.lsr");
    lap_packet_config_t c = pc();
    int h = lap_packet_open(0, &c);
    assert(h > 0);
    pump_io(1);
    lap_record_config_t rc = {.size = sizeof(rc),
                              .version = 1,
                              .source = h,
                              .format = LAP_RECORD_PACKETS,
                              .rate_hz = 4800,
                              .frequency_hz = 406050000,
                              .max_bytes = 65536};
    strcpy(rc.name, "packets.lsr");
    int rec = lap_record_start(&rc);
    assert(rec > 0);
    pump_io(1);
    ls_lap_io_sim_command("packet=record this");
    pump_io(1);
    assert(!lap_record_mark(rec, (char[]){"test mark"}));
    pump_io(1);
    assert(!lap_record_stop(rec));
    pump_io(2);
    lap_record_status_t s;
    assert(!lap_record_status(rec, &s) && s.state == LAP_IO_DONE && s.blocks >= 2);
    lap_packet_close(h);
    pump_io(1);
    int rp = lap_replay_open((char[]){"packets.lsr"});
    assert(rp > 0);
    pump_io(1);
    char data[255];
    lap_packet_meta_t m;
    assert(lap_packet_read(rp, data, 255, &m) == 11 && (m.flags & LAP_DATA_REPLAY));
    assert(!memcmp(data, "record this", 11));
    assert(!lap_replay_seek(rp, 0));
    pump_io(1);
    assert(lap_packet_read(rp, data, 255, &m) == 11);
    pump_io(4);
    assert(!lap_replay_status(rp, &s) && s.state == LAP_IO_DONE);
    lap_replay_close(rp);
    pump_io(1);
    strcpy(rc.name, "failed.lsr");
    remove("lap-sd-io/apps_data/io-test/failed.lsr");
    h = lap_packet_open(0, &c);
    assert(h > 0);
    pump_io(1);
    rc.source = h;
    rec = lap_record_start(&rc);
    assert(rec > 0);
    pump_io(1);
    ls_lap_io_sim_command("iofilefail=-27");
    ls_lap_io_sim_command("packet=write failure");
    pump_io(1);
    assert(!lap_record_status(rec, &s) && s.state == LAP_IO_FAILED && s.error == LAP_IO_ENOSPC);
    ls_lap_io_sim_command("iofilefail=0");
    lap_packet_close(h);
    pump_io(1);
    FILE *f = fopen("lap-sd-io/apps_data/io-test/truncated.lsr", "wb");
    assert(f);
    fwrite("LSR2", 1, 4, f);
    fclose(f);
    rp = lap_replay_open((char[]){"truncated.lsr"});
    assert(rp > 0);
    pump_io(1);
    assert(!lap_replay_status(rp, &s) && s.error == LAP_IO_EFORMAT);
    lap_replay_close(rp);
    pump_io(1);
    remove("lap-sd-io/apps_data/io-test/failed.lsr");
    remove("lap-sd-io/apps_data/io-test/truncated.lsr");
    finish();
}

static void sample_recordings(void) {
    start(LAP_CAP_RADIO_RX);
    for (int format = 1; format <= 2; format++) {
        char name[16];
        strcpy(name, format == 1 ? "iq.lsr" : "audio.lsr");
        char path[128];
        snprintf(path, sizeof(path), "lap-sd-io/apps_data/io-test/%s", name);
        remove(path);
        lap_record_config_t c = {.size = sizeof(c),
                                 .version = 1,
                                 .format = format,
                                 .rate_hz = format == 1 ? 125000 : 16000,
                                 .frequency_hz = 433920000,
                                 .max_bytes = 65536};
        strcpy(c.name, name);
        int r = lap_record_start(&c);
        assert(r > 0);
        pump_io(1);
        int16_t samples[1024];
        for (int i = 0; i < 1024; i++)
            samples[i] = i;
        ls_lap_api_callback(false);
        ls_lap_io_tap(format, samples, sizeof(samples), 433920000, c.rate_hz, LAP_DATA_GAP);
        ls_lap_api_callback(true);
        pump_io(1);
        assert(!lap_record_stop(r));
        pump_io(2);
        lap_record_status_t stat;
        assert(!lap_record_status(r, &stat) && stat.state == LAP_IO_DONE);
        int iqh = 0;
        if (format == 1) {
            lap_rx_config_t iqc = {
                .center_hz = 433920000, .rate_hz = 125000, .gain_tenths_db = LAP_GAIN_AUTO};
            iqh = lap_rx_open(-1, &iqc);
            assert(iqh > 0);
            pump_io(1);
        } else
            assert(!lap_audio_listen(true));
        int h = lap_replay_open(name);
        assert(h > 0);
        pump_io(3);
        uint8_t data[2048];
        lap_packet_meta_t m;
        assert(lap_replay_read(h, data, sizeof(data), &m) == 2048 && (m.flags & LAP_DATA_REPLAY) &&
               (m.flags & LAP_DATA_GAP));
        assert(!memcmp(data, samples, sizeof(samples)));
        if (format == 1) {
            lap_iq_meta_t im;
            assert(lap_rx_read(iqh, (int16_t *)data, 512, &im) == 512 &&
                   (im.flags & LAP_IQ_REPLAY));
        } else
            assert(lap_audio_read((int16_t *)data, 1024) == 1024);
        assert(!lap_replay_seek(h, 0));
        pump_io(1);
        assert(lap_replay_read(h, data, sizeof(data), &m) == 2048);
        lap_replay_close(h);
        if (iqh)
            lap_rx_close(iqh);
        pump_io(1);
        remove(path);
    }
    finish();
}
static void transmit(void) {
    start(LAP_CAP_RADIO_TX);
    lap_packet_config_t c = pc();
    lap_io_status_t s;
    c.power_dbm = 23;
    assert(lap_tx_packet(0, &c, (char[]){"x"}, 1) == LAP_IO_EPOWER);
    c.power_dbm = 0;
    c.port = LAP_PORT_RX_ONLY;
    assert(lap_tx_packet(0, &c, (char[]){"x"}, 1) == LAP_IO_ERXPORT);
    c.port = 0;
    /* 406 MHz has no legal/band prompt or exclusion. */
    int h = lap_tx_packet(0, &c, (char[]){"direct"}, 6);
    assert(h > 0);
    pump_io(3);
    assert(!lap_tx_status(h, &s) && s.state == LAP_IO_DONE);
    h = lap_tx_packet(0, &c, (char[]){"cancel"}, 6);
    assert(h > 0);
    lap_tx_cancel(h);
    pump_io(1);
    assert(!lap_tx_status(h, &s) && s.state == LAP_IO_CANCELLED);
    ls_lap_io_sim_command("iotemp=1");
    h = lap_tx_packet(0, &c, (char[]){"x"}, 1);
    assert(h > 0);
    pump_io(1);
    assert(!lap_tx_status(h, &s) && s.error == LAP_IO_ETHERMAL);
    ls_lap_io_sim_command("iotemp=0");
    int8_t iq[256] = {0};
    h = lap_tx_iq(0, &c, iq, 128, 125000);
    assert(h > 0);
    pump_io(3);
    assert(!lap_tx_status(h, &s) && s.state == LAP_IO_DONE);
    h = lap_tx_replay(0, &c, (char[]){"packets.lsr"});
    assert(h > 0);
    pump_io(3);
    assert(!lap_tx_status(h, &s) && s.state == LAP_IO_DONE);
    finish();
    /* Shared per-radio accounting persists across app generations. */
    assert(!ls_tx_protect(LS_TX_MIXRF, 10, false, 5000000, 1000000));
    assert(ls_tx_protect(LS_TX_MIXRF, 10, false, 1, 1000001) == LAP_IO_ETHERMAL);
    assert(!ls_tx_protect(LS_TX_MIXRF, 10, false, 5000000, 27000000));
    assert(ls_tx_protect(LS_TX_MIXRF, 10, false, 1, 53000000) == LAP_IO_EDUTY);
    assert(!ls_tx_protect(LS_TX_MIXRF, 10, false, 1, 89000000));
    assert(ls_tx_protect(LS_TX_SOCKET, 0, true, 1, 1000000) == LAP_IO_ERXPORT);
    /* Charged by energy: a -3 dBm carrier for ten minutes, paid a second at a
       time, fits; six seconds at full power still does not. */
    for (uint64_t t = 200000000; t < 800000000; t += 1000000)
        assert(!ls_tx_protect(LS_TX_SIM, -3, false, 1000000, t));
    assert(ls_tx_protect(LS_TX_SIM, 22, false, 6000000, 900000000) == LAP_IO_ETHERMAL);
    assert(ls_tx_lora_airtime(8, 7, 125000, 5, 8, true, false) == 36096);
    remove("lap-sd-io/apps_data/io-test/packets.lsr");
}
static void hackrf_tx(void) {
    start(0); lap_hackrf_tx_config_t gain={.size=sizeof(gain),.version=1,.vga_db=12};
    assert(lap_tx_hackrf_config(0,&gain)==LAP_RX_EACCES); finish();
    start(LAP_CAP_RADIO_TX); assert(lap_tx_hackrf_config(0,&gain)==LAP_RX_EUNSUPPORTED);
    ls_lap_io_sim_command("hackrf=1"); pump_io(2);
    /* Refresh the radio snapshot through the normal worker. */
    finish(); start(LAP_CAP_RADIO_TX|LAP_CAP_RADIO_RX);
    assert(lap_tx_hackrf_config(0,(void *)1)==LAP_RX_EINVAL);
    gain.vga_db=48; assert(lap_tx_hackrf_config(0,&gain)==LAP_RX_EINVAL); gain.vga_db=12;
    assert(!lap_tx_hackrf_config(0,&gain));
    lap_packet_config_t c=pc(); c.frequency_hz=5800000000ULL;
    int8_t iq[256]; memset(iq,127,sizeof(iq)); lap_io_status_t st;
    int h=lap_tx_iq(0,&c,iq,128,2000000); assert(h>0);
    assert(lap_tx_hackrf_config(0,&gain)==LAP_RX_EBUSY); pump_io(3);
    assert(!lap_tx_status(h,&st)&&st.state==LAP_IO_DONE);
    lap_record_config_t recording={.size=sizeof(recording),.version=1,.format=LAP_RECORD_IQ16,
        .rate_hz=2000000,.frequency_hz=100000000,.max_bytes=65536};
    strcpy(recording.name,"hackrf.lsr"); remove("lap-sd-io/apps_data/io-test/hackrf.lsr");
    int rec=lap_record_start(&recording); assert(rec>0); pump_io(1);
    int16_t samples[512]; for(int i=0;i<512;i++) samples[i]=i*63;
    ls_lap_api_callback(false); ls_lap_io_tap(LAP_RECORD_IQ16,samples,sizeof(samples),100000000,2000000,0); ls_lap_api_callback(true);
    pump_io(1); assert(!lap_record_stop(rec)); pump_io(2);
    h=lap_tx_replay(0,&c,(char[]){"hackrf.lsr"}); assert(h>0); pump_io(3);
    assert(!lap_tx_status(h,&st)&&st.state==LAP_IO_DONE);
    remove("lap-sd-io/apps_data/io-test/hackrf.lsr");
    ls_lap_io_sim_command("hackrflimits=10,0");
    h=lap_tx_iq(0,&c,iq,128,2000000); assert(h>0); pump_io(3);
    assert(!lap_tx_status(h,&st)&&st.error==LAP_IO_EPOWER);
    gain.vga_db=10; gain.amp=1; assert(!lap_tx_hackrf_config(0,&gain));
    h=lap_tx_iq(0,&c,iq,128,2000000); assert(h>0); pump_io(3);
    assert(!lap_tx_status(h,&st)&&st.error==LAP_IO_EPOWER);
    gain.amp=0; assert(!lap_tx_hackrf_config(0,&gain));
    ls_lap_io_sim_command("hackrfrx=1");
    h=lap_tx_iq(0,&c,iq,128,2000000); assert(h>0); pump_io(3);
    assert(!lap_tx_status(h,&st)&&st.error==LAP_RX_EBUSY); ls_lap_io_sim_command("hackrfrx=0");
    ls_lap_io_sim_command("hackrftemp=1");
    h=lap_tx_iq(0,&c,iq,128,2000000); assert(h>0); pump_io(3);
    assert(!lap_tx_status(h,&st)&&st.error==LAP_IO_ETHERMAL); ls_lap_io_sim_command("hackrftemp=0");
    h=lap_tx_iq(0,&c,iq,128,1000000); assert(h>0); pump_io(3);
    assert(!lap_tx_status(h,&st)&&st.error==LAP_RX_ERANGE);
    h=lap_tx_iq(0,&c,iq,128,2000000); assert(h>0); lap_tx_cancel(h); pump_io(2);
    assert(!lap_tx_status(h,&st)&&st.state==LAP_IO_CANCELLED);
    finish(); ls_lap_io_sim_command("hackrf=0"); ls_lap_io_sim_command("hackrflimits=47,1");
}
static void nfc_tests(void) {
    start(0);
    assert(lap_nfc_open() == LAP_RX_ENODEV);
    ls_lap_io_sim_command("nfc=sample");
    int h = lap_nfc_open();
    assert(h > 0);
    pump_io(1);
    lap_nfc_event_t e;
    uint8_t data[2048];
    assert(lap_nfc_next(h, &e, data, 1) == LAP_IO_ESIZE);
    assert(lap_nfc_next(h, &e, data, sizeof(data)) == 1 && e.uid_len == 4 && e.ndef_bytes > 0);
    const uint8_t msg[] = {0xd1, 1, 5, 'T', 2, 'e', 'n', 'H', 'i'}; /* app stack, not this program's data */
    assert(!lap_nfc_write(h, msg, sizeof(msg)));
    pump_io(1);
    assert(lap_nfc_next(h, &e, data, sizeof(data)) == 1 && e.kind == 5 &&
           !memcmp(msg, data, sizeof(msg)));
    ls_lap_io_sim_command("nfc=removed");
    pump_io(1);
    assert(lap_nfc_next(h, &e, data, sizeof(data)) == 1 && e.kind == 3);
    lap_nfc_close(h);
    pump_io(1);
    ls_lap_io_sim_command("nfc=none");
    finish();
}
int main(void) {
#ifdef _WIN32
    unsigned char *base = (unsigned char *)GetModuleHandleA(NULL);
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    ls_lap_api_set_image(base, nt->OptionalHeader.SizeOfImage);
#endif
    ls_lap_api_host_clock = io_clock;
    packets();
    scanning();
    database();
    recordings();
    sample_recordings();
    transmit();
    hackrf_tx();
    nfc_tests();
    puts(
        "test_lap_radio_io: packet/edge, scanner/banks, capture/replay, TX protection, NFC passed");
    return 0;
}

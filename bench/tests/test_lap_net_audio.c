/* Shared Phase 2 service contracts; no device, GUI or external network. */
#include "ls_lap_net_audio.h"
#include "ls_lap_radio.h"
#include <assert.h>
#include <limits.h>
#include <stdlib.h>
#ifdef _WIN32
#include <direct.h>
#include <winsock2.h>
#define mkdir_one(p) _mkdir(p)
#else
#include <sys/stat.h>
#define mkdir_one(p) mkdir(p, 0777)
#endif
static bool inside = true, network = true;
static uint32_t hardware = LAP_HW_WIFI | LAP_HW_BLE | LAP_HW_SPEAKER | LAP_HW_MIC | LAP_HW_SD;
static uint32_t grant = LAP_CAP_MIC, now;
static unsigned checks;
#define CHECK(e)                                                                                             \
    do {                                                                                                     \
        assert(e);                                                                                           \
        checks++;                                                                                            \
    } while (0)
static uint32_t clock_now(void) { return now; }
uint32_t (*ls_lap_api_host_clock)(void) = clock_now;
bool ls_lap_api_inside(void) { return inside; }
bool ls_lap_api_readable(const void *p, size_t n) {
    return p && (uintptr_t)p > 4096 && n < 1048576 && (uintptr_t)p + n >= (uintptr_t)p;
}
bool ls_lap_net_allowed(void) { return network; }
uint32_t ls_lap_sys_caps(void) { return hardware; }
uint32_t ls_lap_granted(void) { return grant; }
bool ls_lap_api_data_path(char *out, size_t cap, const char *name) {
    if (!name || !*name || strchr(name, '/') || strchr(name, '\\') || strstr(name, "..") || strlen(name) > 48)
        return false;
    snprintf(out, cap, "net-audio-fixture/%s", name);
    return true;
}
static void tick(unsigned ms) {
    now += ms;
    ls_lap_av_service();
}
static void close_op(int h) {
    for (int i = 0; i < 4; i++)
        lap_operation_close(INT_MAX - i);
    lap_operation_close(h);
    tick(1);
}
static void wifi_http(void) {
    lap_operation_t op;
    lap_wifi_status_t ws;
    lap_time_status_t time;
    network = false;
    CHECK(lap_wifi_scan_start() == LAP_IO_EACCES);
    network = true;
    hardware = 0;
    CHECK(lap_wifi_scan_start() == LAP_IO_ENOTSUP);
    hardware = 255;
    CHECK(lap_wifi_status((void *)1) == LAP_IO_EINVAL);
    int h = lap_wifi_scan_start();
    CHECK(h > 0);
    CHECK(!lap_operation_status(h, &op) && op.state == LAP_OPERATION_PENDING);
    lap_wifi_ap_t ap[16];
    CHECK(lap_wifi_scan_result(h, 0, ap, 16) == LAP_IO_EAGAIN);
    tick(40);
    CHECK(lap_wifi_scan_result(h, 0, ap, 16) == 1 && !strcmp(ap[0].ssid, "SIM WLAN"));
    close_op(h);
    CHECK(lap_operation_status(h, &op) == LAP_IO_ECLOSED);
    h = lap_wifi_connect("not saved", NULL);
    tick(40);
    CHECK(!lap_operation_status(h, &op) && op.error == LAP_IO_EAUTH);
    close_op(h);
    char ssid[33] = "test network", password[65] = "test password";
    h = lap_wifi_connect(ssid, password);
    memset(password, 'x', 13);
    ssid[0] = 'x';
    tick(40);
    CHECK(!lap_wifi_status(&ws) && ws.connected && !strcmp(ws.ssid, "test network") && ws.rssi < 0);
    close_op(h);
    h = lap_wifi_connect("test network", NULL);
    tick(40);
    CHECK(!lap_operation_status(h, &op) && !op.error);
    close_op(h);
    ls_lap_av_script("wifierror=-110");
    h = lap_wifi_connect("bad", "incorrect");
    tick(40);
    CHECK(!lap_operation_status(h, &op) && op.error == LAP_IO_EAUTH);
    close_op(h);
    ls_lap_av_script("wifierror=0");
    h = lap_wifi_ap_request(true);
    tick(40);
    CHECK(!lap_wifi_status(&ws) && ws.ap_running);
    close_op(h);
    h = lap_wifi_ap_request(false);
    tick(40);
    CHECK(!lap_wifi_status(&ws) && !ws.ap_running);
    close_op(h);
    h = lap_wifi_disconnect();
    tick(40);
    CHECK(!lap_wifi_status(&ws) && !ws.connected);
    close_op(h);
    h = lap_wifi_connect("test network", NULL);
    tick(40);
    close_op(h);
    h = lap_time_sync_request();
    tick(40);
    CHECK(!lap_time_status(&time) && time.synced && time.source == 2 && time.uncertainty_ms == UINT32_MAX);
    close_op(h);
    int ops[4];
    for (int i = 0; i < 4; i++) {
        ops[i] = lap_wifi_scan_start();
        CHECK(ops[i] > 0);
    }
    CHECK(lap_wifi_scan_start() == LAP_IO_EBUSY);
    for (int i = 0; i < 4; i++)
        close_op(ops[i]);
    lap_http_request_t req = {.size = sizeof(req), .version = 1, .timeout_ms = 1000, .response_limit = 65536};
    strcpy(req.url, "https://example.test/path");
    strcpy(req.headers, "Accept: text/plain\r\n");
    lap_http_status_t hs;
    char body[128], header[100];
    h = lap_http_open(&req);
    CHECK(h > 0);
    CHECK(lap_http_read(h, body, sizeof(body)) == LAP_IO_EAGAIN);
    req.url[0] = '!';
    tick(40);
    CHECK(!lap_http_status(h, &hs) && hs.status_code == 200);
    CHECK(lap_http_header(h, "content-type", header, sizeof(header)) == 10 && !strcmp(header, "text/plain"));
    int total = 0;
    for (int i = 0; i < 8; i++) {
        int n = lap_http_read(h, body, sizeof(body));
        if (n > 0)
            total += n;
        tick(40);
    }
    CHECK(total == (int)strlen("LakeShark HTTPS fixture\n"));
    CHECK(lap_http_read(h, body, sizeof(body)) == 0);
    lap_http_close(INT_MAX);
    lap_http_close(INT_MAX - 1);
    lap_http_close(h);
    tick(1);
    CHECK(lap_http_status(h, &hs) == LAP_IO_ECLOSED);
    strcpy(req.url, "https://example.test/");
    ls_lap_av_script("httperror=-109");
    h = lap_http_open(&req);
    tick(40);
    CHECK(!lap_http_status(h, &hs) && hs.error == LAP_IO_ETLS);
    CHECK(lap_http_read(h, body, sizeof(body)) == LAP_IO_ETLS);
    lap_http_close(INT_MAX);
    lap_http_close(INT_MAX - 1);
    lap_http_close(h);
    tick(1);
    ls_lap_av_script("httperror=0");
    req.response_limit = 2;
    h = lap_http_open(&req);
    tick(40);
    CHECK(!lap_http_status(h, &hs) && hs.error == LAP_IO_ELIMIT);
    lap_http_close(INT_MAX);
    lap_http_close(INT_MAX - 1);
    lap_http_close(h);
    tick(1);
    strcpy(req.headers, "Host: attacker\r\n");
    CHECK(lap_http_open(&req) == LAP_IO_EINVAL);
    req.headers[0] = 0;
    strcpy(req.url, "https://user:pass@example.test/");
    CHECK(lap_http_open(&req) == LAP_IO_EINVAL);
    strcpy(req.url, "https://example.test/");
    req.response_limit = 65536;
    h = lap_http_open(&req);
    now += 1001;
    tick(0);
    CHECK(!lap_http_status(h, &hs) && hs.error == LAP_IO_ETIMEOUT);
    lap_http_close(INT_MAX);
    lap_http_close(INT_MAX - 1);
    lap_http_close(h);
    tick(1);
}
static void udp_mdns(void) {
    lap_socket_address_t local = {.size = sizeof(local), .version = 1, .port = 43992}, to = local, from;
    strcpy(to.ip, "127.0.0.1");
    int h = lap_udp_open(&local);
    CHECK(h > 0);
    tick(1);
    lap_operation_t st;
    CHECK(!lap_udp_status(h, &st) && st.state == LAP_OPERATION_READY && !st.error);
    CHECK(lap_udp_send(h, &to, "packet", 6) == 6);
    char data[1472];
    int n = LAP_IO_EAGAIN;
    for (int i = 0; i < 40 && n < 0; i++) {
        tick(1);
        n = lap_udp_recv(h, &from, data, sizeof(data));
    }
    CHECK(n == 6 && !memcmp(data, "packet", 6) && from.port == 43992);
    CHECK(lap_udp_send(h, &to, "long", 4) == 4);
    tick(1);
    CHECK(lap_udp_recv(h, &from, data, 1) == LAP_IO_ELIMIT);
    CHECK(lap_udp_recv(h, &from, data, 4) == 4);
    CHECK(lap_udp_send(h, &to, NULL, 0) == 0);
    tick(1);
    CHECK(lap_udp_recv(h, &from, NULL, 0) == 0);
    for (int i = 0; i < 4; i++)
        CHECK(lap_udp_send(h, &to, data, 1) == 1);
    CHECK(lap_udp_send(h, &to, data, 1) == LAP_IO_EAGAIN);
    for (int i = 0; i < 4; i++)
        tick(1);
    CHECK(lap_udp_send(h, &to, "drop", 4) == 4);
    tick(1);
    CHECK(!lap_udp_status(h, &st) && st.dropped == 1);
    CHECK(lap_udp_send(h, &to, data, 1473) == LAP_IO_ELIMIT);
    CHECK(lap_udp_join(h, "127.0.0.1", true) == LAP_IO_EINVAL);
    CHECK(!lap_udp_join(h, "239.255.42.1", true));
    tick(1);
    CHECK(!lap_udp_join(h, "239.255.42.1", false));
    tick(1);
    for (int i = 0; i < 4; i++)
        lap_udp_close(INT_MAX - i);
    lap_udp_close(h);
    tick(1);
    CHECK(lap_udp_status(h, &st) == LAP_IO_ECLOSED);
    int b = lap_mdns_browse("_http", "_tcp");
    CHECK(b > 0);
    lap_mdns_service_t service;
    CHECK(lap_mdns_next(b, &service) == LAP_IO_EAGAIN);
    tick(40);
    CHECK(lap_mdns_next(b, &service) == 1 && service.port == 8080);
    CHECK(lap_mdns_next(b, &service) == 0);
    lap_mdns_stop(b);
    tick(1);
    lap_mdns_registration_t reg = {.size = sizeof(reg), .version = 1, .port = 1234};
    strcpy(reg.instance, "SDK test");
    strcpy(reg.service, "_test");
    strcpy(reg.protocol, "_udp");
    strcpy(reg.txt, "a=one;b=two");
    ls_lap_av_script("mdnserror=-111");
    b = lap_mdns_register(&reg);
    CHECK(b > 0);
    tick(1);
    CHECK(lap_mdns_next(b, &service) == LAP_IO_ECONFLICT);
    lap_mdns_stop(b);
    tick(1);
    ls_lap_av_script("mdnserror=0");
    b = lap_mdns_register(&reg);
    tick(1);
    CHECK(!lap_mdns_next(b, &service));
    lap_mdns_stop(b);
    tick(1);
}
static void ble(void) {
    ls_lap_av_script("blesilence=1");
    int scan = lap_ble_scan_start();
    CHECK(scan > 0);
    tick(40);
    uint8_t address[] = {1, 2, 3, 4, 5, 6}, payload[LAP_BLE_AD_MAX];
    memset(payload, 0xab, sizeof(payload));
    ls_lap_ble_advert_push(address, 1, -55, false, payload, sizeof(payload), now);
    lap_ble_advert_t ad;
    uint8_t copy[LAP_BLE_AD_MAX];
    CHECK(lap_ble_advert_next(scan, &ad, copy, 20) == LAP_IO_ELIMIT);
    CHECK(lap_ble_advert_next(scan, &ad, copy, sizeof(copy)) == sizeof(payload) && ad.rssi == -55 &&
          !memcmp(payload, copy, sizeof(payload)));
    for (int i = 0; i < 12; i++)
        ls_lap_ble_advert_push(address, 1, -60 - i, false, payload, 20, now + i);
    lap_operation_t status;
    CHECK(!lap_ble_scan_status(scan, &status) && status.dropped >= 4);
    int h = lap_ble_connect(address, 1);
    CHECK(h > 0);
    tick(40);
    lap_ble_event_t ev;
    uint8_t value[20];
    CHECK(lap_ble_event_next(&ev, value, sizeof(value)) == 1 && ev.kind == LAP_BLE_CONNECTED);
    int op = lap_gatt_discover(h, 0, 0);
    CHECK(op > 0);
    CHECK(lap_gatt_read(h, 3) == LAP_IO_EBUSY);
    tick(40);
    CHECK(lap_ble_event_next(&ev, value, sizeof(value)) == 1 && ev.kind == LAP_GATT_SERVICE &&
          ev.operation == op);
    CHECK(lap_ble_event_next(&ev, value, sizeof(value)) == 1 && ev.kind == LAP_GATT_COMPLETE);
    CHECK(lap_gatt_discover(h, 1, 8) > 0);
    tick(40);
    CHECK(lap_ble_event_next(&ev, value, sizeof(value)) == 1 && ev.kind == LAP_GATT_CHARACTERISTIC);
    CHECK(lap_ble_event_next(&ev, value, sizeof(value)) == 1);
    CHECK(lap_gatt_descriptors(h, 3, 8) > 0);
    tick(40);
    CHECK(lap_ble_event_next(&ev, value, sizeof(value)) == 1 && ev.kind == LAP_GATT_DESCRIPTOR &&
          ev.attribute == 4);
    CHECK(lap_ble_event_next(&ev, value, sizeof(value)) == 1);
    CHECK(lap_gatt_read(h, 3) > 0);
    tick(40);
    CHECK(lap_ble_event_next(&ev, NULL, 0) == LAP_IO_ELIMIT);
    CHECK(lap_ble_event_next(&ev, value, sizeof(value)) == 1 && ev.kind == LAP_GATT_VALUE && value[0] == 87);
    CHECK(lap_gatt_write(h, 3, payload, 21) == LAP_IO_ELIMIT);
    CHECK(lap_gatt_write(h, 3, payload, 20) > 0);
    tick(40);
    CHECK(lap_ble_event_next(&ev, value, sizeof(value)) == 1 && ev.kind == LAP_GATT_WRITTEN);
    CHECK(lap_gatt_subscribe(h, 4, true) > 0);
    tick(40);
    CHECK(lap_ble_event_next(&ev, value, sizeof(value)) == 1);
    ls_lap_av_script("blenotify=1234");
    CHECK(lap_ble_event_next(&ev, value, sizeof(value)) == 1 && ev.kind == LAP_GATT_NOTIFY &&
          value[0] == 0x12);
    ls_lap_av_script("gatterror=-105");
    CHECK(lap_gatt_read(h, 3) > 0);
    tick(40);
    CHECK(lap_ble_event_next(&ev, value, sizeof(value)) == 1 && ev.error == LAP_IO_EIO);
    ls_lap_av_script("gatterror=0");
    lap_ble_disconnect(INT_MAX);
    lap_ble_disconnect(h);
    tick(1);
    CHECK(lap_gatt_read(h, 3) == LAP_IO_ECLOSED);
    lap_ble_scan_stop(scan);
    tick(1);
    CHECK(lap_ble_scan_status(scan, &status) == LAP_IO_ECLOSED);
}
static uint32_t get32(uint8_t *p) {
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static void sound(void) {
    grant = 0;
    CHECK(lap_mic_open() == LAP_IO_EACCES);
    grant = LAP_CAP_MIC;
    hardware &= ~LAP_HW_MIC;
    CHECK(lap_mic_open() == LAP_IO_ENOTSUP);
    hardware |= LAP_HW_MIC;
    int16_t pcm[2048];
    for (int i = 0; i < 2048; i++)
        pcm[i] = 1000;
    CHECK(!lap_mixer_config(0, 100, 20));
    CHECK(!lap_mixer_config(2, 100, 100));
    CHECK(lap_mixer_write(0, pcm, 2048) == 2048);
    CHECK(lap_mixer_write(0, pcm, 1) == 0);
    CHECK(lap_mixer_write(2, pcm, 320) == 320);
    tick(20);
    ls_lap_sound_host_t *host = ls_lap_sound_host();
    CHECK(host->count == 320 && host->pcm[0] == 1200 && host->pcm[319] == 1200);
    tick(20);
    CHECK(host->pcm[0] == 1000);
    ls_lap_av_script("audiomute=1");
    tick(20);
    lap_operation_t state;
    CHECK(!lap_mixer_status(0, &state) && state.dropped == 320);
    ls_lap_av_script("audiomute=0");
    lap_mixer_clear(0);
    int mic = lap_mic_open();
    CHECK(mic > 0 && ls_lap_mic_active());
    int r = lap_audio_record_start(mic, "test.wav", 100);
    CHECK(r > 0);
    for (int i = 0; i < 6; i++)
        tick(20);
    CHECK(!lap_audio_operation(r, &state) && state.state == LAP_OPERATION_DONE && state.transferred == 3200 &&
          !state.error);
    lap_audio_close(r);
    tick(1);
    FILE *f = fopen("net-audio-fixture/test.wav", "rb");
    CHECK(f != NULL);
    uint8_t h[44];
    CHECK(fread(h, 1, 44, f) == 44 && !memcmp(h, "RIFF", 4) && get32(h + 40) == 3200);
    fclose(f);
    CHECK(lap_mic_read(mic, pcm, 2048, &state) > 0);
    for (int i = 0; i < 30; i++)
        tick(20);
    CHECK(lap_mic_read(mic, pcm, 2048, &state) > 0 && state.dropped > 0);
    CHECK(lap_sample_play(1, "../other.wav") == LAP_IO_EINVAL);
    r = lap_sample_play(1, "test.wav");
    CHECK(r > 0);
    for (int i = 0; i < 15; i++)
        tick(20);
    CHECK(!lap_audio_operation(r, &state) && state.state == LAP_OPERATION_DONE && !state.error);
    lap_audio_close(r);
    tick(1);
    r = lap_audio_record_start(mic, "stopped.wav", 1000);
    CHECK(r > 0);
    tick(20);
    CHECK(!lap_audio_record_stop(r));
    tick(20);
    CHECK(!lap_audio_operation(r, &state) && state.state == LAP_OPERATION_DONE && !state.error &&
          state.transferred > 0 && state.transferred < 32000);
    lap_audio_close(r);
    tick(1);
    /* RIFF parser skips unknown odd-sized chunks and catches truncated PCM. */
    f = fopen("net-audio-fixture/test.wav", "rb");
    uint8_t wav[3244];
    CHECK(fread(wav, 1, sizeof(wav), f) == sizeof(wav));
    fclose(f);
    f = fopen("net-audio-fixture/padded.wav", "wb");
    uint32_t size = get32(wav + 4) + 10;
    for (int i = 0; i < 4; i++)
        wav[4 + i] = size >> (8 * i);
    fwrite(wav, 1, 12, f);
    fwrite("JUNK\1\0\0\0x\0", 1, 10, f);
    fwrite(wav + 12, 1, sizeof(wav) - 12, f);
    fclose(f);
    r = lap_sample_play(1, "padded.wav");
    CHECK(r > 0);
    for (int i = 0; i < 15; i++)
        tick(20);
    CHECK(!lap_audio_operation(r, &state) && !state.error && state.state == LAP_OPERATION_DONE);
    lap_audio_close(r);
    tick(1);
    f = fopen("net-audio-fixture/truncated.wav", "wb");
    fwrite(wav, 1, 44, f);
    fclose(f);
    r = lap_sample_play(1, "truncated.wav");
    tick(20);
    CHECK(!lap_audio_operation(r, &state) && state.error == LAP_IO_EIO);
    lap_audio_close(r);
    tick(1);
    int old_mic = mic;
    lap_mic_close(INT_MAX);
    lap_mic_close(mic);
    tick(1);
    CHECK(!ls_lap_mic_active());
    mic = lap_mic_open();
    CHECK(mic > 0 && mic != old_mic);
    lap_mic_close(old_mic);
    tick(1);
    CHECK(ls_lap_mic_active());
    r = lap_audio_record_start(mic, "unload.wav", 1000);
    CHECK(r > 0);
    tick(20);
    ls_lap_av_end();
    tick(20);
    CHECK(!ls_lap_mic_active());
    f = fopen("net-audio-fixture/unload.wav", "rb");
    CHECK(f != NULL);
    CHECK(fread(h, 1, 44, f) == 44 && get32(h + 40) > 0);
    fclose(f);
    ls_lap_av_begin();
    tick(1);
    CHECK(lap_mic_read(mic, pcm, 2048, &state) == LAP_IO_ECLOSED);
}
int main(void) {
    mkdir_one("net-audio-fixture");
    ls_lap_av_begin();
    tick(1);
    wifi_http();
    udp_mdns();
    ble();
    sound();
    inside = false;
    CHECK(lap_ble_scan_start() == LAP_IO_EINVAL);
    inside = true;
    ls_lap_av_end();
    tick(1);
    printf("Phase 2 net/audio: %u assertions passed\n", checks);
    return 0;
}

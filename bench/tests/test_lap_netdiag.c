/* API 2.8 network diagnostics: contracts over the simulator's fixed LAN.
   No device, GUI or real network; the worker runs on a virtual clock. */
#include "ls_lap_net_audio.h"
#include "ls_lap_usage.h"
#include <assert.h>
#include <stdlib.h>
static bool inside = true, network = true;
static uint32_t hardware = LAP_HW_WIFI | LAP_HW_SD, now;
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
uint32_t ls_lap_granted(void) { return 0; }
bool ls_lap_api_data_path(char *out, size_t cap, const char *name) {
    snprintf(out, cap, "netdiag-fixture/%s", name);
    return true;
}
static void tick(unsigned ms) {
    for (unsigned i = 0; i < ms; i += 10) {
        now += 10;
        ls_lap_av_service();
    }
}
static lap_ping_config_t config(uint16_t count, uint16_t interval, uint16_t timeout, uint8_t ttl) {
    return (lap_ping_config_t){.size = sizeof(lap_ping_config_t), .version = 1, .count = count,
                               .interval_ms = interval, .timeout_ms = timeout, .ttl = ttl};
}
/* Waits up to ms for the next answer; returns what lap_ping_next did. */
static int next(int h, lap_ping_reply_t *r, unsigned ms) {
    int e;
    for (unsigned t = 0; (e = lap_ping_next(h, r)) == LAP_IO_EAGAIN && t < ms; t += 10) tick(10);
    return e;
}
static int lookup(const char *name, char ip[16]) {
    int h = lap_dns_resolve(name, 0), e;
    CHECK(h > 0);
    for (unsigned t = 0; (e = lap_dns_result(h, ip)) == LAP_IO_EAGAIN && t < 6000; t += 10) tick(10);
    CHECK(lap_dns_result(h, ip) == LAP_IO_ECLOSED);   /* a final answer frees the handle */
    return e;
}

static void permission(void) {
    lap_net_info_t ni;
    lap_arp_entry_t arp[4];
    char ip[16];
    network = false;
    CHECK(lap_net_info(0, &ni) == LAP_IO_EACCES);
    CHECK(lap_ping_start("192.0.2.1", NULL) == LAP_IO_EACCES);
    CHECK(lap_ping_next(1, &(lap_ping_reply_t){0}) == LAP_IO_EACCES);
    CHECK(lap_dns_resolve("localhost", 0) == LAP_IO_EACCES);
    CHECK(lap_dns_result(1, ip) == LAP_IO_EACCES);
    CHECK(lap_arp_probe("192.0.2.1") == LAP_IO_EACCES);
    CHECK(lap_arp_table(arp, 4) == LAP_IO_EACCES);
    lap_ping_stop(1);
    network = true;
    inside = false;
    CHECK(lap_net_info(0, &ni) == LAP_IO_EINVAL);
    CHECK(lap_ping_start("192.0.2.1", NULL) == LAP_IO_EINVAL);
    inside = true;
}

static void info(void) {
    lap_net_info_t ni;
    CHECK(lap_net_info(0, (void *)1) == LAP_IO_EINVAL);
    CHECK(lap_net_info(-1, &ni) == LAP_IO_EINVAL);
    CHECK(lap_net_info(3, &ni) == LAP_IO_EINVAL);
    tick(10);
    CHECK(!lap_net_info(LAP_NETIF_ACTIVE, &ni));
    CHECK(ni.size == sizeof(ni) && ni.version == 1 && ni.kind == LAP_NETIF_WIFI && ni.link_up);
    CHECK(!strcmp(ni.ip, "192.0.2.2") && !strcmp(ni.mask, "255.255.255.0"));
    CHECK(!strcmp(ni.gateway, "192.0.2.1") && !strcmp(ni.dns, "192.0.2.1"));
    CHECK(ni.mac[0] == 0x02 && ni.mac[5] == 0x02);
    CHECK(!lap_net_info(LAP_NETIF_WIFI, &ni) && ni.kind == LAP_NETIF_WIFI);
    /* USB Ethernet is reserved: absent on this firmware. */
    CHECK(lap_net_info(LAP_NETIF_ETH, &ni) == LAP_IO_ENOTSUP && ni.kind == LAP_NETIF_NONE && !ni.link_up);
    hardware = LAP_HW_SD;           /* a board without Wi-Fi */
    tick(510);
    CHECK(!lap_net_info(LAP_NETIF_ACTIVE, &ni) && ni.kind == LAP_NETIF_NONE && !ni.link_up && !ni.ip[0]);
    CHECK(lap_net_info(LAP_NETIF_WIFI, &ni) == LAP_IO_ENOTSUP);
    hardware = LAP_HW_WIFI | LAP_HW_SD;
    tick(510);
    CHECK(!lap_net_info(0, &ni) && ni.link_up);
}

static void ping(void) {
    lap_ping_reply_t r;
    lap_ping_config_t c = config(3, 100, 500, 0);
    int h = lap_ping_start("192.0.2.10", &c);
    CHECK(h > 0);
    CHECK(lap_ping_next(h, &r) == LAP_IO_EAGAIN);
    for (int s = 0; s < 3; s++) {
        CHECK(next(h, &r, 1000) == 1);
        CHECK(r.size == sizeof(r) && r.seq == s && r.type == LAP_PING_REPLY && r.ttl == 64);
        CHECK(r.rtt_us == 900 + 10 * 53 + s % 5 * 90u && !strcmp(r.from, "192.0.2.10"));
    }
    CHECK(lap_ping_next(h, &r) == 0);
    lap_ping_stop(h);
    CHECK(lap_ping_next(h, &r) == LAP_IO_ECLOSED);

    /* A silent address: one timeout per request, then done. */
    c = config(2, 100, 200, 0);
    h = lap_ping_start("192.0.2.99", &c);
    CHECK(next(h, &r, 1000) == 1 && r.type == LAP_PING_TIMEOUT && r.seq == 0 && !r.rtt_us && !r.from[0]);
    CHECK(next(h, &r, 1000) == 1 && r.type == LAP_PING_TIMEOUT && r.seq == 1);
    CHECK(next(h, &r, 1000) == 0);
    lap_ping_stop(h);

    /* NULL config: four requests a second apart. */
    h = lap_ping_start("localhost", NULL);
    for (int s = 0; s < 4; s++) CHECK(next(h, &r, 1500) == 1 && r.seq == s && !strcmp(r.from, "127.0.0.1"));
    CHECK(lap_ping_next(h, &r) == 0);
    lap_ping_stop(h);

    /* Names resolve first; a name that does not stays an error. */
    h = lap_ping_start("gateway.lan", &(lap_ping_config_t){.size = sizeof(c), .version = 1, .count = 1});
    CHECK(next(h, &r, 1000) == 1 && r.type == LAP_PING_REPLY && !strcmp(r.from, "192.0.2.1"));
    lap_ping_stop(h);
    h = lap_ping_start("nowhere.invalid", NULL);
    CHECK(next(h, &r, 1000) == LAP_IO_ENOTFOUND);
    CHECK(lap_ping_next(h, &r) == LAP_IO_ENOTFOUND);
    lap_ping_stop(h);
    tick(10);
}

static void traceroute(void) {
    static const char *const hops[] = {"192.0.2.1", "203.0.113.1", "203.0.113.9", "198.51.100.7"};
    lap_ping_reply_t r;
    unsigned last = 0;
    for (int ttl = 1; ttl <= 4; ttl++) {
        lap_ping_config_t c = config(1, 0, 1000, (uint8_t)ttl);
        int h = lap_ping_start("198.51.100.7", &c);
        CHECK(h > 0);
        CHECK(next(h, &r, 1000) == 1);
        CHECK(r.type == (ttl < 4 ? LAP_PING_TTL_EXCEEDED : LAP_PING_REPLY));
        CHECK(!strcmp(r.from, hops[ttl - 1]) && r.rtt_us > last);
        last = r.rtt_us;
        CHECK(next(h, &r, 100) == 0);
        lap_ping_stop(h);
    }
    tick(10);
}

static void limits(void) {
    lap_ping_config_t c = config(1, 0, 0, 0);
    lap_ping_reply_t r;
    c.size--;
    CHECK(lap_ping_start("192.0.2.1", &c) == LAP_IO_EINVAL);
    c = config(1001, 0, 0, 0);
    CHECK(lap_ping_start("192.0.2.1", &c) == LAP_IO_EINVAL);
    c = config(1, 19, 0, 0);
    CHECK(lap_ping_start("192.0.2.1", &c) == LAP_IO_EINVAL);
    c = config(1, 0, 99, 0);
    CHECK(lap_ping_start("192.0.2.1", &c) == LAP_IO_EINVAL);
    c = config(1, 0, 0, 0);
    c.payload_size = 1401;
    CHECK(lap_ping_start("192.0.2.1", &c) == LAP_IO_EINVAL);
    c.payload_size = 1400;
    c.reserved = 1;
    CHECK(lap_ping_start("192.0.2.1", &c) == LAP_IO_EINVAL);
    CHECK(lap_ping_start(NULL, NULL) == LAP_IO_EINVAL);
    CHECK(lap_ping_start("", NULL) == LAP_IO_EINVAL);
    CHECK(lap_ping_start("bad host", NULL) == LAP_IO_EINVAL);
    CHECK(lap_ping_next(12345, &r) == LAP_IO_ECLOSED);
    CHECK(lap_ping_next(-1, &r) == LAP_IO_ECLOSED);

    /* Four per app; stop frees a slot on the worker's next pass. */
    int h[5];
    for (int i = 0; i < 4; i++) CHECK((h[i] = lap_ping_start("192.0.2.1", NULL)) > 0);
    CHECK(lap_ping_start("192.0.2.1", NULL) == LAP_IO_EBUSY);
    lap_ping_stop(h[2] + 4);        /* a stale or foreign handle changes nothing */
    tick(10);
    CHECK(lap_ping_start("192.0.2.1", NULL) == LAP_IO_EBUSY);
    lap_ping_stop(h[2]);
    CHECK(lap_ping_next(h[2], &r) == LAP_IO_ECLOSED);
    tick(10);
    CHECK((h[4] = lap_ping_start("192.0.2.1", NULL)) > 0 && h[4] != h[2]);
    CHECK(ls_lap_netdiag_usage(0).handles == 4);
    for (int i = 0; i < 5; i++) lap_ping_stop(h[i]);
    tick(10);
    CHECK(ls_lap_netdiag_usage(0).handles == 0);

    /* Backpressure: an app that stops reading gets at most 8 answers queued
       and loses none; reading again resumes the run in order. */
    c = config(20, 20, 1000, 0);
    int b = lap_ping_start("192.0.2.42", &c);
    tick(2000);
    int seen = 0, e;
    while ((e = lap_ping_next(b, &r)) == 1) CHECK(r.seq == seen++);
    CHECK(seen == 8 && e == LAP_IO_EAGAIN);
    while ((e = next(b, &r, 1000)) == 1) CHECK(r.seq == seen++ && r.type == LAP_PING_REPLY);
    CHECK(seen == 20 && e == 0);
    lap_ping_stop(b);
    tick(10);
}

static void dns(void) {
    char ip[16];
    CHECK(lap_dns_resolve("localhost", 99) == LAP_IO_EINVAL);
    CHECK(lap_dns_resolve("localhost", 10001) == LAP_IO_EINVAL);
    CHECK(lap_dns_resolve(NULL, 0) == LAP_IO_EINVAL);
    CHECK(lap_dns_resolve("a b", 0) == LAP_IO_EINVAL);
    char *volatile bad = (char *)1;
    CHECK(lap_dns_result(1, bad) == LAP_IO_EINVAL);
    int h = lap_dns_resolve("localhost", 1000);
    CHECK(lap_dns_result(h, ip) == LAP_IO_EAGAIN);
    tick(10);
    CHECK(!lap_dns_result(h, ip) && !strcmp(ip, "127.0.0.1"));
    CHECK(lap_dns_result(h, ip) == LAP_IO_ECLOSED);
    CHECK(!lookup("gateway.lan", ip) && !strcmp(ip, "192.0.2.1"));
    CHECK(!lookup("192.0.2.42", ip) && !strcmp(ip, "192.0.2.42"));
    CHECK(!lookup("example.org", ip) && !strncmp(ip, "198.51.100.", 11));
    char again[16];
    CHECK(!lookup("example.org", again) && !strcmp(ip, again));     /* deterministic */
    CHECK(lookup("host.invalid", ip) == LAP_IO_ENOTFOUND);
    ls_lap_av_script("dnsfail=1");
    CHECK(lookup("example.org", ip) == LAP_IO_ENOTFOUND);
    ls_lap_av_script("dnsfail=0");
    /* Four lookups; an answer nobody collects is dropped after 10 s. */
    int l[4];
    for (int i = 0; i < 4; i++) CHECK((l[i] = lap_dns_resolve("localhost", 0)) > 0);
    CHECK(lap_dns_resolve("localhost", 0) == LAP_IO_EBUSY);
    tick(9000);
    CHECK(lap_dns_resolve("localhost", 0) == LAP_IO_EBUSY);
    tick(1100);
    CHECK(lap_dns_result(l[0], ip) == LAP_IO_ECLOSED);
    h = lap_dns_resolve("localhost", 0);
    CHECK(h > 0);
    tick(10);
    CHECK(!lap_dns_result(h, ip));
}

static void arp(void) {
    lap_arp_entry_t t[40];
    CHECK(lap_arp_probe("192.0.2") == LAP_IO_EINVAL);
    CHECK(lap_arp_probe("10.0.0.1") == LAP_IO_EINVAL);     /* not on the interface's subnet */
    CHECK(lap_arp_probe(NULL) == LAP_IO_EINVAL);
    CHECK(lap_arp_table(t, -1) == LAP_IO_EINVAL);
    CHECK(lap_arp_table((void *)1, 4) == LAP_IO_EINVAL);
    CHECK(lap_arp_table(NULL, 0) == 0);
    for (int i = 1; i <= 16; i++) {
        char ip[16];
        snprintf(ip, sizeof(ip), "192.0.2.%d", i * 3 - 2);  /* 1, 4, ... 46: some alive */
        CHECK(!lap_arp_probe(ip));
    }
    CHECK(lap_arp_probe("192.0.2.3") == LAP_IO_EAGAIN);     /* sixteen already wait */
    tick(10);
    CHECK(!lap_arp_probe("192.0.2.3"));
    CHECK(!lap_arp_probe("192.0.2.23"));
    tick(10);
    int n = lap_arp_table(t, 40);
    /* .1 .3 .10 .23 .42 answer; probes and earlier pings found them all. */
    CHECK(n == 5);
    static const char *const ips[] = {"192.0.2.1", "192.0.2.3", "192.0.2.10", "192.0.2.23", "192.0.2.42"};
    for (int i = 0; i < n; i++) CHECK(!strcmp(t[i].ip, ips[i]) && t[i].iface == LAP_NETIF_WIFI && t[i].size == sizeof(t[i]));
    CHECK(t[2].mac[0] == 0xDC && t[2].mac[1] == 0xA6 && t[2].mac[2] == 0x32);   /* Raspberry Pi */
    CHECK(lap_arp_table(t, 2) == 2 && !strcmp(t[1].ip, "192.0.2.3"));
    /* Pairs expire five minutes after they were last seen. */
    tick(301000);
    CHECK(lap_arp_table(t, 40) == 0);
}

static void link_down(void) {
    lap_net_info_t ni;
    lap_ping_reply_t r;
    char ip[16];
    int h = lap_ping_start("192.0.2.1", &(lap_ping_config_t){.size = sizeof(lap_ping_config_t), .version = 1, .count = 100});
    CHECK(next(h, &r, 1000) == 1);
    ls_lap_av_script("netdown=1");
    tick(510);
    CHECK(!lap_net_info(0, &ni) && ni.kind == LAP_NETIF_WIFI && !ni.link_up && !ni.ip[0]);
    while (lap_ping_next(h, &r) == 1) {}        /* answers already queued still drain */
    CHECK(lap_ping_next(h, &r) == LAP_IO_ENETDOWN);
    lap_ping_stop(h);
    CHECK(lap_arp_probe("192.0.2.1") == LAP_IO_ENETDOWN);
    h = lap_dns_resolve("localhost", 0);
    tick(10);
    CHECK(lap_dns_result(h, ip) == LAP_IO_ENETDOWN);
    ls_lap_av_script("netdown=0");
    tick(510);
    CHECK(!lap_net_info(0, &ni) && ni.link_up);
}

static void app_exit(void) {
    lap_ping_reply_t r;
    char ip[16];
    int p = lap_ping_start("192.0.2.1", NULL), d = lap_dns_resolve("localhost", 0);
    CHECK(p > 0 && d > 0 && ls_lap_netdiag_usage(0).handles == 2);
    ls_lap_av_end();                /* the app closes */
    tick(10);
    CHECK(ls_lap_netdiag_usage(0).handles == 0);
    ls_lap_av_begin();              /* the next app sees nothing of it */
    CHECK(lap_ping_next(p, &r) == LAP_IO_ECLOSED && lap_dns_result(d, ip) == LAP_IO_ECLOSED);
}

int main(void) {
    ls_lap_av_begin();
    permission();
    info();
    ping();
    traceroute();
    limits();
    dns();
    arp();
    link_down();
    app_exit();
    ls_lap_av_end();
    tick(10);
    printf("API 2.8 network diagnostics: %u assertions passed\n", checks);
    return 0;
}

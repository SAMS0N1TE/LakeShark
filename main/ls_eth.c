#include "sdkconfig.h"
#include "ls_eth.h"
#include <stdio.h>
#include <string.h>

#if CONFIG_LS_NANO_ETH
#include <inttypes.h>
#include "esp_attr.h"
#include "esp_eth.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_heap_caps.h"
#include "hal/emac_hal.h"
#include "lwip/ip4_addr.h"
#include "nvs.h"
#include "ls_nvs_safe.h"

/* Waveshare C5 Nano, Peripheral Quick Reference / Ethernet:
 * https://docs.waveshare.com/ESP32-P4-NANO-WIFI6-DB
 * IP101GRI addr 1, reset 51. IDF 5.5's P4 EMAC defaults match the wiring:
 * CRS_DV28 RXD0=29 RXD1=30 MDC31 TXD0=34 TXD1=35 TX_EN49 MDIO52,
 * external 50 MHz REF_CLK input on GPIO50. C5 SDIO/Wi-Fi is independent. */
static esp_eth_handle_t s_eth;
static esp_netif_t *s_netif;
/* Off until 'eth on': the EMAC holds ~10.5 KB of internal DMA RAM for good, and
   with it ADS-B pushed DMA free down to 1.7 KB. Only boards that use the cable pay. */
static bool s_enabled = false, s_running, s_static, s_initialized, s_link;
static esp_netif_ip_info_t s_ip;
static uint32_t s_rx, s_tx, s_rx_bytes, s_tx_bytes;
static esp_err_t (*s_transmit)(esp_eth_mac_t *, uint8_t *, uint32_t);

/* Boot snapshots, not post-boot estimates. Keep the diagnostics out of the
 * internal heap being measured. Publish only complete immutable entries. */
typedef struct {
    size_t internal_free, internal_largest, internal_min;
    size_t dma_free, dma_largest, dma_min;
} eth_heap_t;
static EXT_RAM_BSS_ATTR eth_heap_t s_heap[3];
static unsigned s_heap_count;
static const char *const s_heap_names[] = {"pre-mac", "post-mac", "ready"};

static void heap_print(unsigned i)
{
    const eth_heap_t *h = &s_heap[i];
    printf("eth heap %-8s: internal_free=%u internal_largest=%u internal_min=%u "
           "dma_free=%u dma_largest=%u dma_min=%u\n", s_heap_names[i],
           (unsigned)h->internal_free, (unsigned)h->internal_largest, (unsigned)h->internal_min,
           (unsigned)h->dma_free, (unsigned)h->dma_largest, (unsigned)h->dma_min);
}

static void heap_mark(unsigned i)
{
    multi_heap_info_t internal, dma;
    heap_caps_get_info(&internal, MALLOC_CAP_INTERNAL);
    heap_caps_get_info(&dma, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    s_heap[i] = (eth_heap_t){internal.total_free_bytes, internal.largest_free_block,
        internal.minimum_free_bytes, dma.total_free_bytes, dma.largest_free_block,
        dma.minimum_free_bytes};
    __atomic_store_n(&s_heap_count, i + 1, __ATOMIC_RELEASE);
    heap_print(i);
}

/* All L2 frames delivered to lwIP / accepted by the MAC, including ARP/DHCP.
 * Atomic 32-bit counters wrap; never take a console lock in the MAC task. */
static esp_err_t counted_tx(esp_eth_mac_t *mac, uint8_t *buf, uint32_t len)
{
    esp_err_t e = s_transmit(mac, buf, len);
    if (e == ESP_OK) {
        __atomic_fetch_add(&s_tx, 1, __ATOMIC_RELAXED);
        __atomic_fetch_add(&s_tx_bytes, len, __ATOMIC_RELAXED);
    }
    return e;
}
static esp_err_t counted_rx(esp_eth_handle_t eth, uint8_t *buf, uint32_t len, void *ctx)
{
    (void)eth;
    __atomic_fetch_add(&s_rx, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&s_rx_bytes, len, __ATOMIC_RELAXED);
    return esp_netif_receive(ctx, buf, len, NULL);
}
static void event(void *ctx, esp_event_base_t base, int32_t id, void *data)
{
    (void)ctx;
    if (base == ETH_EVENT && *(esp_eth_handle_t *)data == s_eth) {
        if (id == ETHERNET_EVENT_CONNECTED) __atomic_store_n(&s_link, true, __ATOMIC_RELAXED);
        if (id == ETHERNET_EVENT_DISCONNECTED || id == ETHERNET_EVENT_STOP)
            __atomic_store_n(&s_link, false, __ATOMIC_RELAXED);
        ESP_LOGI("eth", "link %s", __atomic_load_n(&s_link, __ATOMIC_RELAXED) ? "up" : "down");
    } else if (base == IP_EVENT && id == IP_EVENT_ETH_GOT_IP) {
        ip_event_got_ip_t *got = data;
        if (got->esp_netif == s_netif) ESP_LOGI("eth", "IP " IPSTR, IP2STR(&got->ip_info.ip));
    }
}
static esp_err_t store(void *unused)
{
    (void)unused;
    nvs_handle_t h;
    esp_err_t e = nvs_open("lseth", NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    e = nvs_set_u8(h, "enabled", s_enabled);
    if (e == ESP_OK) e = nvs_set_u8(h, "static", s_static);
    if (e == ESP_OK) e = nvs_set_blob(h, "ip", &s_ip, sizeof(s_ip));
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e;
}
static esp_err_t apply_ip(void)
{
    esp_err_t e = esp_netif_dhcpc_stop(s_netif);
    if (e != ESP_OK && e != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) return e;
    esp_netif_ip_info_t ip = s_static ? s_ip : (esp_netif_ip_info_t){0};
    e = esp_netif_set_ip_info(s_netif, &ip);
    if (e == ESP_OK && !s_static) {
        e = esp_netif_dhcpc_start(s_netif);
        if (e == ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED) e = ESP_OK;
    }
    return e;
}
static esp_err_t enable(bool on)
{
    if (on == s_running) return ESP_OK;
    esp_err_t e;
    if (on) {
        e = apply_ip();
        if (e == ESP_OK) e = esp_eth_start(s_eth);
    } else e = esp_eth_stop(s_eth);
    if (e == ESP_OK) s_running = on;
    return e;
}
static esp_err_t init(void)
{
    esp_err_t e = esp_netif_init();
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) return e;
    e = esp_event_loop_create_default();
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) return e;
    eth_mac_config_t mc = ETH_MAC_DEFAULT_CONFIG();
    eth_esp32_emac_config_t emac = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    eth_phy_config_t pc = ETH_PHY_DEFAULT_CONFIG();
    pc.phy_addr = 1; pc.reset_gpio_num = 51;
    heap_mark(0); /* netif/event loop are up; console REPL is already reserved */
    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&emac, &mc);
    heap_mark(1); /* retained even on failure, after the driver's cleanup */
    esp_eth_phy_t *phy = esp_eth_phy_new_ip101(&pc);
    esp_eth_netif_glue_handle_t glue = NULL;
    if (!mac || !phy) { e = ESP_ERR_NO_MEM; goto fail; }
    s_transmit = mac->transmit; mac->transmit = counted_tx;
    esp_eth_config_t cfg = ETH_DEFAULT_CONFIG(mac, phy);
    e = esp_eth_driver_install(&cfg, &s_eth);
    if (e != ESP_OK) goto fail;
    esp_netif_config_t nc = ESP_NETIF_DEFAULT_ETH();
    esp_netif_inherent_config_t inherent = *nc.base;
    inherent.route_prio = 150; /* wins over STA (100); Wi-Fi takes over on link loss */
    nc.base = &inherent;
    s_netif = esp_netif_new(&nc);
    if (!s_netif) { e = ESP_ERR_NO_MEM; goto fail; }
    glue = esp_eth_new_netif_glue(s_eth);
    if (!glue) { e = ESP_ERR_NO_MEM; goto fail; }
    e = esp_netif_attach(s_netif, glue);
    if (e != ESP_OK) goto fail;
    e = esp_eth_update_input_path(s_eth, counted_rx, s_netif);
    if (e != ESP_OK) goto fail;
    e = esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, event, NULL);
    if (e != ESP_OK) goto fail;
    e = esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, event, NULL);
    if (e == ESP_OK) { s_initialized = true; return e; }
    esp_event_handler_unregister(ETH_EVENT, ESP_EVENT_ANY_ID, event);
fail:
    if (glue) esp_eth_del_netif_glue(glue);
    if (s_netif) { esp_netif_destroy(s_netif); s_netif = NULL; }
    if (s_eth) { esp_eth_driver_uninstall(s_eth); s_eth = NULL; }
    if (mac) mac->del(mac);
    if (phy) phy->del(phy);
    return e;
}
void ls_eth_start(void)
{
    nvs_handle_t h;
    if (nvs_open("lseth", NVS_READONLY, &h) == ESP_OK) {
        uint8_t v;
        if (nvs_get_u8(h, "enabled", &v) == ESP_OK) s_enabled = v;
        if (nvs_get_u8(h, "static", &v) == ESP_OK) s_static = v;
        size_t n = sizeof(s_ip);
        if (nvs_get_blob(h, "ip", &s_ip, &n) != ESP_OK || n != sizeof(s_ip)) s_static = false;
        nvs_close(h);
    }
    if (!s_enabled) {
        ESP_LOGI("eth", "off ('eth on' to use the wired port)");
        return;
    }
    esp_err_t e = init();
    if (e == ESP_OK) e = enable(true);
    if (e == ESP_OK) heap_mark(2);
    if (e != ESP_OK) ESP_LOGE("eth", "bring-up: %s", esp_err_to_name(e));
}
int ls_eth_command(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "heap")) {
        const unsigned count = __atomic_load_n(&s_heap_count, __ATOMIC_ACQUIRE);
        const eth_mac_config_t mc = ETH_MAC_DEFAULT_CONFIG();
        printf("eth heap: RX=%u TX=%u buffer=%u desc=%u rings=%u rx_stack=%u "
               "(objects/TCB/alignment extra)\n", CONFIG_ETH_DMA_RX_BUFFER_NUM,
               CONFIG_ETH_DMA_TX_BUFFER_NUM, CONFIG_ETH_DMA_BUFFER_SIZE,
               (unsigned)sizeof(eth_dma_rx_descriptor_t),
               (unsigned)(CONFIG_ETH_DMA_RX_BUFFER_NUM *
                   (CONFIG_ETH_DMA_BUFFER_SIZE + sizeof(eth_dma_rx_descriptor_t)) +
                   CONFIG_ETH_DMA_TX_BUFFER_NUM *
                   (CONFIG_ETH_DMA_BUFFER_SIZE + sizeof(eth_dma_tx_descriptor_t))),
               (unsigned)mc.rx_task_stack_size);
        for (unsigned i = 0; i < count; ++i) heap_print(i);
        if (!count) puts("eth heap: no initialization attempt yet");
        return 0;
    }
    if (argc == 2 && (!strcmp(argv[1], "on") || !strcmp(argv[1], "off"))) {
        const bool on = !strcmp(argv[1], "on");
        s_enabled = on;
        esp_err_t e = ls_nvs_run(store, NULL, 0);
        if (e == ESP_OK && on && !s_initialized) {
            /* Late in a running board the heap may be too broken up for the
               EMAC; then the saved choice takes it at the next boot. */
            if (init() != ESP_OK) {
                puts("eth: on from the next boot (not enough free internal RAM right now)");
                return 0;
            }
        }
        if (e == ESP_OK && s_initialized) e = enable(on);
        if (!on && s_initialized) puts("eth: off; its buffers are freed at the next boot");
        printf("eth: %s\n", esp_err_to_name(e));
        return e == ESP_OK ? 0 : 1;
    }
    if (argc > 1) {
        if (!s_initialized) { puts("eth: off - 'eth on' first"); return 1; }
        esp_err_t e;
        if ((argc == 5 && !strcmp(argv[1], "static")) || (argc == 2 && !strcmp(argv[1], "dhcp"))) {
            esp_netif_ip_info_t ip = {0};
            if (argc == 5 && (!ip4addr_aton(argv[2], (ip4_addr_t *)&ip.ip) ||
                !ip4addr_aton(argv[3], (ip4_addr_t *)&ip.netmask) ||
                !ip4addr_aton(argv[4], (ip4_addr_t *)&ip.gw) || !ip.ip.addr || !ip.netmask.addr)) {
                puts("eth: invalid IPv4 address/mask/gateway"); return 1;
            }
            bool old_static = s_static; esp_netif_ip_info_t old_ip = s_ip;
            s_static = argc == 5; s_ip = ip;
            e = apply_ip();
            if (e != ESP_OK) { s_static = old_static; s_ip = old_ip; apply_ip(); }
        } else { puts("eth [heap|on|off|dhcp|static <ip> <mask> <gw>]"); return 1; }
        if (e == ESP_OK) e = ls_nvs_run(store, NULL, 0);
        printf("eth: %s\n", esp_err_to_name(e));
        if (e != ESP_OK) return 1;
    }
    esp_netif_ip_info_t ip = {0};
    uint8_t mac[6] = {0}; eth_speed_t speed = ETH_SPEED_10M;
    bool link = __atomic_load_n(&s_link, __ATOMIC_RELAXED);
    if (s_netif && esp_netif_is_netif_up(s_netif)) esp_netif_get_ip_info(s_netif, &ip);
    if (s_eth) {
        esp_eth_ioctl(s_eth, ETH_CMD_G_MAC_ADDR, mac);
        if (link) esp_eth_ioctl(s_eth, ETH_CMD_G_SPEED, &speed);
    }
    printf("eth: %s link=%s speed=%dM ip=" IPSTR " mask=" IPSTR " gw=" IPSTR " %s\n",
           s_running ? "on" : "off", link ? "up" : "down", link ? (speed == ETH_SPEED_100M ? 100 : 10) : 0,
           IP2STR(&ip.ip), IP2STR(&ip.netmask), IP2STR(&ip.gw), s_static ? "static" : "dhcp");
    printf("eth: mac=%02x:%02x:%02x:%02x:%02x:%02x rx=%" PRIu32 " tx=%" PRIu32 " rx_bytes=%" PRIu32 " tx_bytes=%" PRIu32 "\n",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
           __atomic_load_n(&s_rx, __ATOMIC_RELAXED), __atomic_load_n(&s_tx, __ATOMIC_RELAXED),
           __atomic_load_n(&s_rx_bytes, __ATOMIC_RELAXED), __atomic_load_n(&s_tx_bytes, __ATOMIC_RELAXED));
    return 0;
}
#else
void ls_eth_start(void) {}
int ls_eth_command(int argc, char **argv)
{
    (void)argc; (void)argv;
    puts("eth: requires CONFIG_LS_NANO_ETH (Nano panel-runtime only)");
    return 1;
}
#endif

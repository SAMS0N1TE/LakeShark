/* mesh_gateway.cpp - generic LoRaMesher mesh gateway for the ESP32-P4 panel.
 *
 * Brings up the SX1262 + LoRaMesher and exposes a read-only view of the mesh:
 * network state, this node's address, and the routing table (per-node hops /
 * link quality / RSSI / SNR) with a short RSSI history. The Mesh app GUI reads
 * this snapshot on a timer.
 *
 * (A proprietary device-polling protocol previously lived here; it has been
 * removed for the public release. This is now a plain LoRaMesher node/link
 * viewer.)
 */
#include "mesh_gateway.h"

#include <Arduino.h>
#include <memory>
#include <cstring>
#include <vector>
#include <map>
#include <algorithm>

#include "loramesher.hpp"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/uart_vfs.h"

using namespace loramesher;

static const char *TAG = "mesh";

#ifndef MESH_LORA_NSS
#define MESH_LORA_NSS    2
#define MESH_LORA_RST    3
#define MESH_LORA_BUSY   4
#define MESH_LORA_DIO1   5
#define MESH_LORA_SCK   20
#define MESH_LORA_MOSI  21
#define MESH_LORA_MISO  22
#define MESH_LORA_RF_SW1 46
#endif

#define MESH_TCXO_V 1.8f

#ifndef MESH_NODE_ADDR
#define MESH_NODE_ADDR 0x0999
#endif

#define MESH_FREQ      915.0f
#define MESH_SF        7U
#define MESH_BW        125.0
#define MESH_CR        7U
#define MESH_POWER     20
#define MESH_SYNC      20U
#define MESH_PREAMBLE  8U
#define MESH_MAXPKT    192U

#ifndef CONFIG_ESP_CONSOLE_UART_NUM
#define CONFIG_ESP_CONSOLE_UART_NUM 0
#endif

static std::unique_ptr<LoraMesher> mesher;
static uint16_t s_node_addr = MESH_NODE_ADDR;
static volatile uint32_t s_last_rx_ms = 0;
static volatile uint32_t s_rx_total   = 0;

struct RssiRing {
    float    buf[MESH_RSSI_HISTORY] = {0};
    uint16_t head = 0;
    uint16_t count = 0;
};
static std::map<uint16_t, RssiRing> s_rssi;
static SemaphoreHandle_t s_db_mtx = nullptr;

static inline void db_lock(void)   { if (s_db_mtx) xSemaphoreTake(s_db_mtx, portMAX_DELAY); }
static inline void db_unlock(void) { if (s_db_mtx) xSemaphoreGive(s_db_mtx); }

static void onDataReceived(AddressType src, const std::vector<uint8_t> &data)
{
    (void)src; (void)data;
    s_rx_total++;
    s_last_rx_ms = millis();
}

static void mesh_rssi_sampler(void *)
{
    for (;;) {
        if (mesher) {
            auto routes = mesher->GetRoutingTable();
            db_lock();
            for (const auto &rt : routes) {
                if (!rt.destination || rt.destination == s_node_addr) continue;
                if (rt.last_rssi == 0.0f) continue;
                RssiRing &r = s_rssi[rt.destination];
                r.buf[r.head] = rt.last_rssi;
                r.head = (r.head + 1) % MESH_RSSI_HISTORY;
                if (r.count < MESH_RSSI_HISTORY) r.count++;
            }
            db_unlock();
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

extern "C" void mesh_gateway_start(void)
{
    initArduino();
    s_db_mtx = xSemaphoreCreateMutex();

    uart_vfs_dev_port_set_tx_line_endings(CONFIG_ESP_CONSOLE_UART_NUM, ESP_LINE_ENDINGS_LF);

    pinMode(MESH_LORA_RF_SW1, OUTPUT);
    digitalWrite(MESH_LORA_RF_SW1, HIGH);

    PinConfig pins(MESH_LORA_NSS, MESH_LORA_RST, MESH_LORA_DIO1, MESH_LORA_BUSY,
                   MESH_LORA_SCK, MESH_LORA_MISO, MESH_LORA_MOSI);

    RadioConfig radio(RadioType::kSx1262, MESH_FREQ, MESH_SF, MESH_BW, MESH_CR,
                      MESH_POWER, MESH_SYNC, true, MESH_PREAMBLE);
    radio.setTcxoVoltage(MESH_TCXO_V);

    LoRaMeshProtocolConfig proto;
    proto.setMaxPacketSize(MESH_MAXPKT);
    proto.setNodeRole(NodeRole::NETWORK_MANAGER);
    proto.setNodeCapabilities(NodeCapabilities::GATEWAY);
    proto.setTargetDutyCycle(0.25f);
    proto.setMinSleepFraction(0.05f);

    auto builder = LoraMesher::Builder()
                       .withPinConfig(pins)
                       .withRadioConfig(radio)
                       .withLoRaMeshProtocol(proto);
    builder.withNodeAddress((AddressType)(MESH_NODE_ADDR));
    mesher = builder.Build();

    mesher->SetDataCallback(onDataReceived);

    Result r = mesher->Start();
    if (!r) {
        ESP_LOGE(TAG, "LoRaMesher Start failed: %s — is the SX1262 wired?",
                 r.GetErrorMessage().c_str());
    } else {
        s_node_addr = mesher->GetNodeAddress();
        ESP_LOGI(TAG, "LoRaMesher gateway up: addr=0x%04X", (unsigned)s_node_addr);
    }

    xTaskCreate(mesh_rssi_sampler, "mesh_rssi", 4096, nullptr, 2, nullptr);
}

extern "C" void mesh_gateway_get_snapshot(mesh_snapshot_t *out)
{
    if (!out) return;
    std::memset(out, 0, sizeof(*out));
    if (!mesher) return;

    auto st = mesher->GetNetworkStatus();
    out->state     = (int)st.current_state;
    out->nm        = st.network_manager;
    out->nodes     = (uint32_t)st.connected_nodes;
    out->synced    = st.is_synchronized;
    out->node_addr = mesher->GetNodeAddress();
    out->uptime_s  = (uint32_t)(millis() / 1000);
    out->radio_ok  = (out->state != 0);
    out->rx_total  = s_rx_total;
    out->last_rx_age_s = s_last_rx_ms ? (millis() - s_last_rx_ms) / 1000 : 0xFFFFFFFF;

    auto routes = mesher->GetRoutingTable();
    uint32_t nr = 0;
    for (const auto &rt : routes) {
        if (nr >= MESH_MAX_ROUTES) break;
        mesh_route_t &d = out->routes[nr++];
        d.addr     = rt.destination;
        d.next_hop = rt.next_hop;
        d.hops     = rt.hop_count;
        d.lq       = rt.link_quality;
        d.rssi     = rt.last_rssi;
        d.snr      = rt.last_snr;
        d.is_nm    = rt.is_network_manager;
        d.is_gw    = (rt.capabilities & NodeCapabilities::GATEWAY) != 0;
    }
    out->route_count = nr;
}

extern "C" const char *mesh_gateway_state_name(int state)
{
    switch (state) {
        case 0: return "INIT";
        case 1: return "DISCOVERY";
        case 2: return "JOINING";
        case 3: return "NORMAL";
        case 4: return "NET_MANAGER";
        case 5: return "FAULT_RECOVERY";
        default: return "?";
    }
}

extern "C" void mesh_gateway_node_db_reset(void)
{
    db_lock();
    s_rssi.clear();
    db_unlock();
    ESP_LOGI(TAG, "mesh view reset");
}

extern "C" uint32_t mesh_gateway_get_rssi_history(uint16_t addr, float *out, uint32_t max)
{
    if (!out || max == 0) return 0;
    uint32_t n = 0;
    db_lock();
    auto it = s_rssi.find(addr);
    if (it != s_rssi.end()) {
        RssiRing &r = it->second;
        uint32_t cnt = r.count;
        if (cnt > max) cnt = max;

        uint16_t start = (uint16_t)((r.head + MESH_RSSI_HISTORY - r.count) % MESH_RSSI_HISTORY);
        uint32_t skip = r.count - cnt;
        start = (uint16_t)((start + skip) % MESH_RSSI_HISTORY);
        for (uint32_t i = 0; i < cnt; i++) {
            out[n++] = r.buf[(start + i) % MESH_RSSI_HISTORY];
        }
    }
    db_unlock();
    return n;
}

extern "C" void mesh_gateway_rescan(void)
{
    if (!mesher) return;
    auto routes = mesher->GetRoutingTable();
    ESP_LOGI(TAG, "rescan: %u route(s) known", (unsigned)routes.size());
}

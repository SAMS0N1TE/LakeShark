// Bastion SSH server with simple tunnel command

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netdb.h>
#include <fcntl.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "protocol_examples_common.h"
#include "esp_eth.h"
#include "ethernet_init.h"
#include "console_simple_init.h"

#include <libssh/libssh.h>
#include <libssh/server.h>
#include <libssh/callbacks.h>

#include "ssh_vfs.h"

static const char* TAG = "bastion_ssh";

void init_ssh_server(void);

void wifi_init_softap(void);

static void init_ethernet_and_netif(void)
{
    static esp_eth_handle_t *s_eth_handles = NULL;
    static uint8_t s_eth_port_cnt = 0;

    ESP_ERROR_CHECK(ethernet_init_all(&s_eth_handles, &s_eth_port_cnt));

    esp_netif_inherent_config_t esp_netif_config = ESP_NETIF_INHERENT_DEFAULT_ETH();
    esp_netif_config_t cfg_spi = {
        .base = &esp_netif_config,
        .stack = ESP_NETIF_NETSTACK_DEFAULT_ETH
    };
    assert(s_eth_port_cnt == 1); // only one Ethernet port supported
        // attach Ethernet driver to TCP/IP stack
    esp_netif_t *eth_netif = esp_netif_new(&cfg_spi);
    assert(eth_netif != NULL);
    ESP_ERROR_CHECK(esp_netif_attach(eth_netif, esp_eth_new_netif_glue(s_eth_handles[0])));
    ESP_ERROR_CHECK(esp_eth_start(s_eth_handles[0]));
}

static void initialize_esp_components(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    // ESP_ERROR_CHECK(example_connect()); // STA or other networking
    init_ethernet_and_netif();
    wifi_init_softap();
}

void app_main(void)
{
    initialize_esp_components();
    init_ssh_server();
}

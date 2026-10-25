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

#define DEFAULT_PORT "22"
#define DEFAULT_USERNAME "user"
#define DEFAULT_PASSWORD "password"

static volatile int authenticated = 0;
static int tries = 0;
static ssh_channel g_channel = NULL;

// Track simple tunnels by local port
typedef struct tunnel_cfg {
    int listen_port;
    char host[64];
    int host_port;
    struct tunnel_cfg *next;
} tunnel_cfg_t;
static tunnel_cfg_t *s_tunnels = NULL;

// --- Utils ---
static int set_nonblock(int fd, int nb)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    if (nb) flags |= O_NONBLOCK; else flags &= ~O_NONBLOCK;
    return fcntl(fd, F_SETFL, flags);
}

static int tcp_connect(const char *host, int port)
{
    char portstr[16];
    struct addrinfo hints = {0}, *res = NULL, *p;
    int s = -1;
    snprintf(portstr, sizeof(portstr), "%d", port);
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_INET;
    if (getaddrinfo(host, portstr, &hints, &res) != 0) return -1;
    for (p = res; p; p = p->ai_next) {
        s = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (s < 0) continue;
        if (connect(s, p->ai_addr, p->ai_addrlen) == 0) break;
        close(s); s = -1;
    }
    freeaddrinfo(res);
    return s;
}

// --- Forwarding tasks ---
typedef struct {
    int a;
    int b;
} pair_t;

static void bridge_task(void *arg)
{
    pair_t *p = (pair_t*)arg;
    int a = p->a, b = p->b;
    free(p);
    const size_t BUF = 1460;
    uint8_t *buf = malloc(BUF);
    if (!buf) goto out;

    set_nonblock(a, 1); set_nonblock(b, 1);

    while (1) {
        fd_set rfds; FD_ZERO(&rfds); FD_SET(a, &rfds); FD_SET(b, &rfds);
        int nfds = (a > b ? a : b) + 1;
        struct timeval tv = { .tv_sec = 30, .tv_usec = 0 };
        int r = select(nfds, &rfds, NULL, NULL, &tv);
        if (r < 0) break;
        if (r == 0) continue;
        if (FD_ISSET(a, &rfds)) {
            int n = recv(a, buf, BUF, 0);
            if (n <= 0) break;
            int off = 0; while (off < n) { int m = send(b, buf + off, n - off, 0); if (m <= 0) { goto out; } off += m; }
        }
        if (FD_ISSET(b, &rfds)) {
            int n = recv(b, buf, BUF, 0);
            if (n <= 0) break;
            int off = 0; while (off < n) { int m = send(a, buf + off, n - off, 0); if (m <= 0) { goto out; } off += m; }
        }
    }
out:
    if (buf) free(buf);
    if (a >= 0) close(a);
    if (b >= 0) close(b);
    vTaskDelete(NULL);
}

typedef struct {
    int listen_port;
    char host[64];
    int host_port;
} listener_cfg_t;

static void listener_task(void *arg)
{
    listener_cfg_t cfg = *(listener_cfg_t*)arg;
    free(arg);
    int ls = -1;
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(cfg.listen_port);

    ls = socket(AF_INET, SOCK_STREAM, 0);
    if (ls < 0) goto done;
    int one = 1; setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (bind(ls, (struct sockaddr*)&addr, sizeof(addr)) < 0) goto done;
    if (listen(ls, 4) < 0) goto done;
    ESP_LOGI(TAG, "Tunnel listening on :%d -> %s:%d", cfg.listen_port, cfg.host, cfg.host_port);

    while (1) {
        int cs = accept(ls, NULL, NULL);
        if (cs < 0) continue;
        int rs = tcp_connect(cfg.host, cfg.host_port);
        if (rs < 0) { close(cs); continue; }
        pair_t *pr = malloc(sizeof(pair_t));
        if (!pr) { close(cs); close(rs); continue; }
        pr->a = cs; pr->b = rs;
        xTaskCreate(bridge_task, "tun_fwd", 4096, pr, 9, NULL);
    }

done:
    if (ls >= 0) close(ls);
    vTaskDelete(NULL);
}

void tunnel_add_and_start(int p1, const char *host, int p2)
{
    // list bookkeeping (optional for now)
    tunnel_cfg_t *node = calloc(1, sizeof(*node));
    if (!node) return;
    node->listen_port = p1; strncpy(node->host, host, sizeof(node->host)-1); node->host_port = p2;
    node->next = s_tunnels; s_tunnels = node;

    listener_cfg_t *cfg = malloc(sizeof(*cfg));
    if (!cfg) return;
    cfg->listen_port = p1; strncpy(cfg->host, host, sizeof(cfg->host)-1); cfg->host_port = p2;
    xTaskCreate(listener_task, "tun_listen", 4096, cfg, 8, NULL);
}


void tunnel_stop(int p1)
{
    // Minimal: just note; full implementation would track sockets and close
    tunnel_cfg_t **pp = &s_tunnels; while (*pp) { if ((*pp)->listen_port == p1) { tunnel_cfg_t *tmp=*pp; *pp=(*pp)->next; free(tmp); break; } pp=&(*pp)->next; }
}

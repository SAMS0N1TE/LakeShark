/* HTTPS / UDP PROBE: workflow/UI stays in the app; workers own transport. */
#include <lakeshark_app.h>
#include <stdio.h>
#include <string.h>
static lap_caps_t caps;
static int http = -1, udp = -1, browse = -1, advertise = -1, operation = -1, scan = -1, dialog = -1;
static int result, received, datagrams, dialog_stage;
static char ssid[33], password[65], message[80] = "H HTTPS  U UDP  W join  S scan";
static void start_http(void) {
    if (http >= 0)
        lap_http_close(http);
    lap_http_request_t r = {.size = sizeof(r),
                            .version = 1,
                            .method = LAP_HTTP_GET,
                            .timeout_ms = 10000,
                            .response_limit = 65536};
    strcpy(r.url, "https://example.com/");
    strcpy(r.headers, "Accept: text/plain\r\n");
    http = lap_http_open(&r);
    result = http;
    received = 0;
}
static void enter(void) {
    lap_caps(&caps, sizeof(caps));
    http = udp = browse = advertise = operation = scan = dialog = -1;
    received = datagrams = 0;
    if (!(caps.hardware & LAP_HW_WIFI)) {
        strcpy(message, "Wi-Fi not fitted");
        return;
    }
    start_http();
    lap_socket_address_t a = {.size = sizeof(a), .version = 1, .port = 43991};
    udp = lap_udp_open(&a);
    browse = lap_mdns_browse("_http", "_tcp");
}
static void leave(void) {
    if (http >= 0)
        lap_http_close(http);
    if (udp >= 0)
        lap_udp_close(udp);
    if (browse >= 0)
        lap_mdns_stop(browse);
    if (advertise >= 0)
        lap_mdns_stop(advertise);
    if (operation >= 0)
        lap_operation_close(operation);
    if (scan >= 0)
        lap_operation_close(scan);
    if (dialog >= 0)
        lap_dialog_cancel(dialog);
}
static bool key(lap_key_t k, char ch) {
    if (k != LAP_KEY_CHAR || !(caps.hardware & LAP_HW_WIFI))
        return false;
    if (ch == 'h')
        start_http();
    else if (ch == 's') {
        if (scan >= 0)
            lap_operation_close(scan);
        scan = lap_wifi_scan_start();
    } else if (ch == 'w') {
        dialog = lap_text_input("Wi-Fi SSID", ssid, 32, 0);
        dialog_stage = 1;
    } else if (ch == 'k') {
        if (operation >= 0)
            lap_operation_close(operation);
        operation = lap_wifi_connect(ssid, NULL);
    } else if (ch == 'd') {
        if (operation >= 0)
            lap_operation_close(operation);
        operation = lap_wifi_disconnect();
    } else if (ch == 'a') {
        if (operation >= 0)
            lap_operation_close(operation);
        operation = lap_wifi_ap_request(true);
    } else if (ch == 'x') {
        if (operation >= 0)
            lap_operation_close(operation);
        operation = lap_wifi_ap_request(false);
    } else if (ch == 't') {
        if (operation >= 0)
            lap_operation_close(operation);
        operation = lap_time_sync_request();
    } else if (ch == 'u' && udp >= 0) {
        lap_socket_address_t to = {.size = sizeof(to), .version = 1, .port = 43991};
        strcpy(to.ip, "127.0.0.1");
        result = lap_udp_send(udp, &to, "probe", 5);
    } else if (ch == 'm' && udp >= 0)
        result = lap_udp_join(udp, "239.255.42.1", true);
    else if (ch == 'n' && udp >= 0)
        result = lap_udp_join(udp, "239.255.42.1", false);
    else if (ch == 'v') {
        lap_mdns_registration_t r = {.size = sizeof(r), .version = 1, .port = 43991};
        strcpy(r.instance, "LakeShark Probe");
        strcpy(r.service, "_lsprobe");
        strcpy(r.protocol, "_udp");
        strcpy(r.txt, "sdk=2.4");
        if (advertise >= 0)
            lap_mdns_stop(advertise);
        advertise = lap_mdns_register(&r);
    } else
        return false;
    return true;
}
static void draw(void) {
    char line[100];
    lap_wifi_status_t wifi = {0};
    lap_time_status_t ts = {0};
    if (caps.hardware & LAP_HW_WIFI) {
        lap_wifi_status(&wifi);
        lap_time_status(&ts);
    }
    if (dialog >= 0) {
        int value = 0;
        char text[65];
        int r = lap_dialog_poll(dialog, text, sizeof(text), &value);
        if (r == 1) {
            lap_dialog_cancel(dialog);
            dialog = -1;
            if (dialog_stage == 1) {
                snprintf(ssid, sizeof(ssid), "%.32s", text);
                dialog = lap_text_input("Password (empty: saved)", "", 64, 0);
                dialog_stage = 2;
            } else {
                snprintf(password, sizeof(password), "%s", text);
                if (operation >= 0)
                    lap_operation_close(operation);
                operation = lap_wifi_connect(ssid, password[0] ? password : NULL);
                memset(password, 0, sizeof(password));
                memset(text, 0, sizeof(text));
            }
        } else if (r < 0 || r == 2)
            dialog = -1;
    }
    if (http >= 0) {
        lap_http_status_t s;
        char data[512];
        int r = lap_http_read(http, data, sizeof(data));
        if (r > 0)
            received += r;
        if (!lap_http_status(http, &s)) {
            result = s.error;
            snprintf(message, sizeof(message), "HTTP %ld  %d bytes  error %ld", (long)s.status_code, received,
                     (long)s.error);
            char type[64];
            if (s.state != LAP_OPERATION_PENDING)
                lap_http_header(http, "Content-Type", type, sizeof(type));
        }
    }
    if (udp >= 0) {
        char data[64];
        lap_socket_address_t from;
        lap_operation_t s;
        if (lap_udp_recv(udp, &from, data, sizeof(data)) >= 0)
            datagrams++;
        lap_udp_status(udp, &s);
    }
    if (browse >= 0) {
        lap_mdns_service_t service;
        int r = lap_mdns_next(browse, &service);
        if (r == 1)
            snprintf(message, sizeof(message), "mDNS %.50s:%u", service.instance, service.port);
    }
    if (operation >= 0) {
        lap_operation_t s;
        if (!lap_operation_status(operation, &s) && s.state == LAP_OPERATION_DONE) {
            result = s.error;
            lap_operation_close(operation);
            operation = -1;
        }
    }
    if (scan >= 0) {
        lap_wifi_ap_t aps[4];
        int n = lap_wifi_scan_result(scan, 0, aps, 4);
        if (n >= 0) {
            snprintf(message, sizeof(message), "Scan: %d APs, %.32s", n, n ? aps[0].ssid : "");
            lap_operation_close(scan);
            scan = -1;
        }
    }
    lap_clear(LAP_ATTR(LAP_WHITE, LAP_BLACK));
    lap_put_str(1, 1, "HTTPS / UDP PROBE", LAP_ATTR(LAP_CYAN | LAP_BRIGHT, LAP_BLACK));
    snprintf(line, sizeof(line), "Wi-Fi %.32s %.15s %d dBm", wifi.ssid, wifi.ip, wifi.rssi);
    lap_put_str(1, 3, line, LAP_ATTR(LAP_WHITE, LAP_BLACK));
    lap_put_str(1, 5, message, LAP_ATTR(LAP_WHITE, LAP_BLACK));
    snprintf(line, sizeof(line), "UDP packets %d  result %d  time source %u", datagrams, result, ts.source);
    lap_put_str(1, 7, line, LAP_ATTR(LAP_WHITE, LAP_BLACK));
    lap_put_str(1, 9, "H HTTPS U loopback S scan W join K saved", LAP_ATTR(LAP_YELLOW, LAP_BLACK));
    lap_put_str(1, 10, "D disconnect A/X AP T time M/N multicast V advert", LAP_ATTR(LAP_YELLOW, LAP_BLACK));
}
LAP_APP(.id = "tcp_probe", .name = "HTTPS UDP", .sub = "network services", .version = "2.0",
        .author = "LakeShark", .icon = LAP_ICON_TOWER, .color = LAP_CYAN, .flags = LAP_CAP_NETWORK,
        .enter = enter, .leave = leave, .draw = draw, .key = key);

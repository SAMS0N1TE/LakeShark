/* API 2.6 Bluetooth peripheral peer fixtures; never emits RF or opens a window. */
#include "ls_lap_ble_peripheral.h"
#include <stdlib.h>
#if !LAP_DEVICE && !defined(LS_LAP_PERIPH_HOST_TEST) && LAP_PERIPH_ADV
static int sim_busy, sim_error, sim_handle;
static bool sim_ack = true;
int ls_lap_peripheral_lease(bool on) { return on && sim_busy ? LAP_IO_EBUSY : 0; }
void ls_lap_peripheral_host_pump(void) {
    static ls_lap_peripheral_command_t c;
    static bool pending;
    if (pending) {
        if (c.kind == PERIPH_SEND && c.indicate && !sim_ack && ls_lap_peripheral_live(c.handle))
            return;
        int err = c.kind == PERIPH_CLOSE ? 0 : sim_error;
        if (!ls_lap_peripheral_complete(c.handle, c.kind, err, c.advert.tx_power_dbm))
            return;
        if (!err) {
            if (c.kind == PERIPH_ADVERTISE)
                fprintf(stderr, "peripheral simulator: advertising h=%d power=%d\n", c.handle,
                        c.advert.tx_power_dbm);
            if (c.kind == PERIPH_SEND) {
                fprintf(stderr, "peripheral simulator: sent id=%u indicate=%u data=", c.id,
                        c.indicate);
                for (unsigned i = 0; i < c.len; i++)
                    fprintf(stderr, "%02x", c.data[i]);
                fputc('\n', stderr);
            }
        }
        if (c.kind == PERIPH_CLOSE) {
            fprintf(stderr, "peripheral simulator: closed h=%d\n", c.handle);
            sim_handle = 0;
        }
        pending = false;
    }
    if (!ls_lap_peripheral_command(&c))
        return;
    if (c.kind == PERIPH_OPEN)
        sim_handle = c.handle;
    pending = true;
}
void ls_lap_peripheral_script(const char *c) {
    if (!strncmp(c, "periphbusy=", 11)) {
        sim_busy = atoi(c + 11);
        return;
    }
    if (!strncmp(c, "peripherror=", 12)) {
        sim_error = atoi(c + 12);
        return;
    }
    if (!strncmp(c, "periphack=", 10)) {
        sim_ack = atoi(c + 10) != 0;
        return;
    }
    if (!sim_handle)
        return;
    if (!strcmp(c, "periphconnect")) {
        ls_lap_peripheral_event(sim_handle, LAP_PERIPHERAL_CONNECTED, 0, 0, 0);
        return;
    }
    if (!strcmp(c, "periphdisconnect")) {
        ls_lap_peripheral_event(sim_handle, LAP_PERIPHERAL_DISCONNECTED, 0, 0, 0);
        return;
    }
    int value, id;
    if (sscanf(c, "periphmtu=%d", &value) == 1) {
        if (value >= 23 && value <= 517)
            ls_lap_peripheral_event(sim_handle, LAP_PERIPHERAL_MTU, 0, 0, value);
        return;
    }
    if (sscanf(c, "periphbond=%d", &value) == 1) {
        ls_lap_peripheral_event(sim_handle, LAP_PERIPHERAL_BOND, 0, 0, value ? 3 : 0);
        return;
    }
    if (sscanf(c, "periphsubscribe=%d,%d", &id, &value) == 2) {
        ls_lap_peripheral_event(sim_handle, LAP_PERIPHERAL_SUBSCRIBE, id, 0, value & 3);
        return;
    }
    bool write = !strncmp(c, "periphwrite=", 12), read = !strncmp(c, "periphread=", 11);
    if (write || read) {
        const char *a = c + (write ? 12 : 11);
        id = atoi(a);
        uint8_t data[128];
        size_t n = 0;
        if (write) {
            a = strchr(a, ',');
            if (!a)
                return;
            a++;
            unsigned v;
            while (n < 128 && a[0] && a[1] && sscanf(a, "%2x", &v) == 1) {
                data[n++] = v;
                a += 2;
            }
        } else
            n = sizeof(data);
        int rc = ls_lap_peripheral_peer_access(sim_handle, id, write, data, &n);
        fprintf(stderr, "peripheral fixture: %s id=%d ATT=%d bytes=%u\n", write ? "write" : "read",
                id, rc, (unsigned)n);
    }
}
#endif

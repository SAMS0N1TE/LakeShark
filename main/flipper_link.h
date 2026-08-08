/* flipper_link - serial control head protocol for LakeShark.
 *
 * Speaks a tiny newline-delimited ASCII protocol over a dedicated UART so a
 * Flipper Zero (or any 3V3 serial terminal) can drive the P25 receiver and
 * render its own UI from the telemetry stream.
 *
 * Default wiring, Waveshare ESP32-P4-NANO <-> Flipper Zero. Both pins are on
 * the LEFT header, bottom row, which is the safest place on this board to land
 * a jumper - see the pin note below.
 *
 *     P4 GPIO33 (RX)  <-- Flipper pin 13 (TX)   left header, bottom row, right column
 *     P4 GPIO32 (TX)  --> Flipper pin 14 (RX)   left header, bottom row, left column
 *     P4 GND          <-> GND (Flipper pin 8/11/18)  the pin directly below GPIO32
 *
 * Why not GPIO45/46, the original choice on the right header:
 *   - GPIO46 is the USB-host VBUS enable (headless_main.c USB_VBUS_GPIO). Using
 *     it as a UART TX fights the load-switch enable input.
 *   - GPIO45 sits directly above C6_IO12/C6_IO13, which belong to the on-board
 *     ESP32-C6, not the P4. A jumper one position off lands on silicon the P4
 *     cannot see, and no amount of pin scanning on the P4 can detect that.
 * GPIO32/33 have neither problem: they are plain GPIOs with no alternate
 * function, they are the last two signal pins on the left header so they can be
 * counted from the end, and their nearest neighbours are GND and GPIO36.
 *
 * Not GPIO37/38 either: those are UART0, wired to the on-board CH343 bridge and
 * therefore to the USB-C console port. Driving them from outside collides with
 * the console and routes the link straight through USB-C.
 */
#ifndef FLIPPER_LINK_H
#define FLIPPER_LINK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 2 adds: the md=ADSB telemetry frame, the na/ta/sa identity-age keys on the
 * P25 frame, the up/fi/fd sys keys on every frame, and the device-management
 * command set (REBOOT / SYS / C6 / SDR / BLE / SAVE). A head built for
 * version 1 still works - every addition is a new key or a new command. */
#define FLIPPER_LINK_PROTO_VERSION 2

typedef struct {
    int      uart_num;
    int      rx_gpio;
    int      tx_gpio;
    uint32_t baud;
    int      telemetry_hz;
} flipper_link_cfg_t;

/* UART0 is the USB console. UART1 used to belong to the P25 diag port, which
 * also wanted GPIO32/33; that port is now off by default (DIAG_UART_ENABLE in
 * diag.h) so the pins are ours. The link still asks for UART2, and
 * link_install() falls back to the next free HP UART if that ever changes. */
#define FLIPPER_LINK_CFG_DEFAULT() (flipper_link_cfg_t){ \
    .uart_num     = 2,                                   \
    .rx_gpio      = 33,                                  \
    .tx_gpio      = 32,                                  \
    .baud         = 115200,                              \
    .telemetry_hz = 5,                                   \
}

/* Hooks the host app provides so the link can drive mode switching without
 * flipper_link.c having to own that policy. */
typedef struct {
    void        (*select_mode_by_name)(const char *name); /* "p25"/"adsb"/"fm" */
    const char *(*current_mode_name)(void);
    /* Play a short tone now. Lets the head prove the speaker path from its own
     * UI - "no audio" is otherwise indistinguishable from "no signal", and the
     * console is not always reachable when the head is the only thing to hand. */
    void        (*play_test_sound)(void);

    /* Device management, for when the radio is not within arm's reach and has
     * no console attached - on a battery in another room, which is the normal
     * way this thing gets used. Every one of these is otherwise only reachable
     * over the USB-C console. All are optional; a NULL hook answers -ERR. */
    void        (*reboot)(void);              /* esp_restart(), after replying */
    void        (*c6_reset)(void);            /* pulse the co-processor's EN    */
    int         (*c6_up)(void);               /* re-run the ESP-Hosted handshake*/
    void        (*sdr_reset)(void);           /* park + unpark the RTL-SDR      */
    /* In-place USB recovery: exit the app, reopen the dongle, re-enter. Try
     * this before sdr_power_cycle - it costs a second rather than a reboot. */
    void        (*sdr_recover)(void);
    /* Cut the dongle's VBUS and restore it, forcing a USB re-enumeration. The
     * only recovery for a dongle whose connection was physically disturbed:
     * it keeps state across an SoC reset, so REBOOT does not help. */
    void        (*sdr_power_cycle)(void);
    void        (*ble_enable)(bool on);       /* radio-side BLE on/off          */
    /* One line of health: uptime, reset reason, heap. Written into `out`. */
    void        (*sys_info)(char *out, size_t len);
    /* Free bytes, so the health numbers can ride along in telemetry cheaply. */
    void        (*heap_stats)(uint32_t *internal, uint32_t *dma, uint32_t *psram);
    uint32_t    (*uptime_s)(void);
    /* Set a runtime log level. tag may be "*". Returns false on a bad level.
     * Logging is not free - every ESP_LOG blocks on the console UART on the
     * calling task - so being able to quieten a tag remotely is as useful as
     * being able to turn one up. */
    bool        (*set_log_level)(const char *tag, const char *level);
} flipper_link_host_t;

esp_err_t flipper_link_start(const flipper_link_cfg_t *cfg,
                             const flipper_link_host_t *host);

void flipper_link_stop(void);
bool flipper_link_running(void);

/* Retarget/restart the link at runtime (console `link` command). */
esp_err_t flipper_link_reconfigure(const flipper_link_cfg_t *cfg);

/* Current config, valid whether or not the link is running. */
void flipper_link_get_cfg(flipper_link_cfg_t *out);

/* Feed one protocol line in from somewhere other than the UART (the debug
 * console) and write the reply into `reply`. Used by the `fl` console command
 * so the whole protocol can be exercised without a Flipper attached. */
void flipper_link_inject(const char *line, char *reply, size_t reply_len);

/* Render the telemetry frame that would go out on the link, without sending
 * it. Lets the USB console see exactly what the Flipper is being told. */
int flipper_link_snapshot(char *buf, size_t len);

int flipper_link_eq_snapshot(char *buf, size_t len);

/* Sweep the free header GPIOs looking for an idle-high line that is actually
 * clocking bytes in, and report what each pin saw. Used to find where the
 * head's TX wire really landed. Returns the busiest pin, or -1. The link is
 * restored to its previous configuration on exit. */
int flipper_link_scan_rx(int dwell_ms);

/* Continuity probe. Unlike scan_rx, which engages a pull-UP and therefore reads
 * high whether or not anything is attached, this drives a pull-DOWN and reports
 * which pins are nonetheless held high. An idle UART TX from a powered head is
 * a stiff 3V3 source and wins against the ~45k pull-down; an unconnected pin
 * reads 0%. That is the difference that lets this find a wire that landed on
 * the wrong header position, which scan_rx structurally cannot do.
 *
 * Returns the pin with the highest sustained level, or -1 if every pin followed
 * the pull-down (i.e. nothing is connected anywhere we can see). */
int flipper_link_probe_rx(void);

/* Mirror every RX line to the ESP log, for bring-up. */
void flipper_link_set_verbose(bool en);
bool flipper_link_verbose(void);

/* Counters, for the console `link` status print. */
void flipper_link_stats(uint32_t *rx_lines, uint32_t *tx_lines, uint32_t *bad_lines);

/* Seconds the SDR has claimed to be ready while delivering no samples, 0 when
 * healthy. Exposed so the host can act on it - the telemetry builder is the
 * only thing that samples the byte rate, so it is where the evidence lives. */
int flipper_link_sdr_stall_s(void);

#ifdef __cplusplus
}
#endif

#endif /* FLIPPER_LINK_H */

/* LakeShark app API (LAP).
 *
 * A LakeShark app is one .lap file in /sdcard/apps: a position-independent
 * RISC-V ELF that the firmware loads into PSRAM when its tile is opened and
 * frees when it is closed. It can call the functions in this header and
 * nothing else; the loader refuses an app that asks for any other symbol.
 *
 * Build with app-sdk/lap.py (see app-sdk/README.md). One app = one C file is
 * enough:
 *
 *     #include "lakeshark_app.h"
 *     static void draw(void) { lap_put_str(1, 1, "hello", LAP_ATTR(LAP_GREEN, LAP_BLACK)); }
 *     LAP_APP(.id = "hello", .name = "HELLO", .sub = "first app", .version = "1.0",
 *             .author = "you", .icon = LAP_ICON_STAR, .color = LAP_GREEN,
 *             .draw = draw);
 *
 * Every callback runs on the screen task, one frame at a time (about 25 a
 * second). Return quickly: a callback that loops forever stops the screen and
 * the board restarts, and the next boot names the app that did it.
 */
#ifndef LAKESHARK_APP_H
#define LAKESHARK_APP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The API an app declares. Firmware with API 2.2 runs every 1.x app (1.0 to
   1.3), every 2.0, 2.1 and 2.2 app unchanged; older firmware refuses a
   newer app with a clear message. An app that wants to keep running on 1.3
   firmware (2.18 / 2.19) defines LAP_TARGET_API_1 before including this
   header: it is then built as a 1.3 app and the 2.x calls are not declared.
   LAP_TARGET_API_20 does the same for 2.0 firmware: the 2.1 calls (services,
   lap_emit, the shared controls in lakeshark_ui.h) are not declared.
   LAP_TARGET_API_21 builds for 2.1 firmware: the 2.2 calls are not declared. */
#if defined(LAP_TARGET_API_1)
#define LAP_API_MAJOR 1
#define LAP_API_MINOR 3 /* API 1.3: permission-gated asynchronous TCP. */
#elif defined(LAP_TARGET_API_20)
#define LAP_API_MAJOR 2
#define LAP_API_MINOR 0 /* API 2.0: radios, receiver audio, decoded events, actions, notices. */
#elif defined(LAP_TARGET_API_21)
#define LAP_API_MAJOR 2
#define LAP_API_MINOR 1 /* API 2.1: the board's own controls, background services, lap_emit. */
#define LAP_API_21 1
#else
#define LAP_API_MAJOR 2
#define LAP_API_MINOR 2 /* API 2.2: P25, sub-GHz and detector events; what the board's watchers see. */
#define LAP_API_21 1
#define LAP_API_22 1
#endif
#define LAP_MAGIC     0x3150414Cu   /* "LAP1" */

/* ---- colours and cells ------------------------------------------------ */

enum {
    LAP_BLACK = 0, LAP_RED, LAP_GREEN, LAP_YELLOW,
    LAP_BLUE, LAP_MAGENTA, LAP_CYAN, LAP_WHITE,
    LAP_BRIGHT = 8              /* OR into a colour: LAP_CYAN | LAP_BRIGHT */
};
#define LAP_ATTR(fg, bg) ((uint8_t)((((bg) & 0x0F) << 4) | ((fg) & 0x0F)))

/* What a cell can hold, for lap_put_cell. Printable ASCII is itself. */
#define LAP_QUAD(tl, tr, bl, br) ((int16_t)(0x80 | ((tl) ? 1 : 0) | ((tr) ? 2 : 0) | ((bl) ? 4 : 0) | ((br) ? 8 : 0)))
#define LAP_SEXT(bits)           ((int16_t)(0xC0 | ((bits) & 0x3F)))  /* bit row*2+col, 2 wide 3 high */
#define LAP_SHADE_25             ((int16_t)0x90)
#define LAP_SHADE_50             ((int16_t)0x91)
#define LAP_SHADE_75             ((int16_t)0x92)
#define LAP_BLOCK                ((int16_t)0x93)
#define LAP_BRAILLE(dots)        ((int16_t)(0x2800 + ((dots) & 0xFF)))  /* Unicode dot order */

/* ---- the app ----------------------------------------------------------- */

typedef enum {
    LAP_KEY_NONE = 0,
    LAP_KEY_CHAR,               /* the character is in `ch` */
    LAP_KEY_UP, LAP_KEY_DOWN, LAP_KEY_LEFT, LAP_KEY_RIGHT,
    LAP_KEY_ENTER, LAP_KEY_BACK, LAP_KEY_TAB, LAP_KEY_BACKSPACE,
    /* 1.1: the rest of what the keyboard and the remote terminal send */
    LAP_KEY_F1, LAP_KEY_F2, LAP_KEY_F3, LAP_KEY_F4, LAP_KEY_F5, LAP_KEY_F6,
    LAP_KEY_F7, LAP_KEY_F8, LAP_KEY_F9, LAP_KEY_F10, LAP_KEY_F11,
    LAP_KEY_ALT, LAP_KEY_CTRL, LAP_KEY_FN, LAP_KEY_META,
    LAP_KEY_MIC, LAP_KEY_MIC_HOLD,      /* the RECORD key, tapped and held */
    /* 1.2: the mouse wheel on a remote screen; not taken (return false) it comes again as UP or DOWN */
    LAP_KEY_WHEEL_UP, LAP_KEY_WHEEL_DOWN,
} lap_key_t;

/* Tile pictures already in the firmware; or draw your own in icon_art. */
enum {
    LAP_ICON_STAR = 11, LAP_ICON_TOWER = 1, LAP_ICON_WAVE = 2, LAP_ICON_PLANE = 3,
    LAP_ICON_RECORD = 4, LAP_ICON_FALLS = 5, LAP_ICON_MAP = 6, LAP_ICON_FILES = 7,
    LAP_ICON_GEAR = 8, LAP_ICON_PAGER = 9, LAP_ICON_CHIP = 10, LAP_ICON_SHARK = 12,
};

#define LAP_ICON_W 10
#define LAP_ICON_H 6

/* API 1.1: a colour icon in dots, drawn the way COMPASS and DICE draw: 20 x 18
   dots (the tile's 10 x 6 cells, 2 x 3 dots each), every dot one of the
   sixteen theme colours. Rows of 20 characters: '0'-'9', 'a'-'f' are colour
   0-15 (LAP_* | LAP_BRIGHT), '.' is clear. Each cell shows its two commonest
   colours, so shade with neighbouring inks and keep edges clean. Optional;
   without it the tile shows icon_art or the built-in icon.

       LAP_ICON_PX("....................",
                   "......ffffffff......",
                   ... 18 rows ...);
*/
#define LAP_PX_W 20
#define LAP_PX_H 18
typedef struct {
    char magic[4];                          /* "LIC1" */
    char rows[LAP_PX_H][LAP_PX_W + 1];
} lap_icon_px_t;
#if defined(__riscv)
#define LAP_ICON_PX_ATTR __attribute__((section(".lap_icon"), used, retain, aligned(4)))
#elif !defined(LAP_ICON_PX_ATTR)            /* lssim names each app's icon to show it */
#define LAP_ICON_PX_ATTR static __attribute__((used))
#endif
#define LAP_ICON_PX(...) \
    LAP_ICON_PX_ATTR const lap_icon_px_t lap_icon_px = { { 'L', 'I', 'C', '1' }, { __VA_ARGS__ } }

/* The app's descriptor. The loader reads the fields up to icon_art straight
   from the file, before any code runs, to put the tile on HOME. */
typedef struct {
    uint32_t magic;             /* LAP_MAGIC, set by LAP_APP */
    uint16_t api_major, api_minor;
    char id[32];                /* [a-z0-9_.-], except "."/"..": names the data folder */
    char name[16];              /* tile and tab title, upper case reads best */
    char sub[32];               /* one line under the name */
    char version[16];
    char author[24];
    uint8_t icon;               /* LAP_ICON_*, used when icon_art is empty */
    uint8_t color;              /* LAP_* colour of the tile */
    uint8_t flags;              /* LAP_CAP_* capabilities, approved before enter */
    uint8_t reserved;
    char icon_art[LAP_ICON_H][LAP_ICON_W + 1];  /* optional 10x6 ASCII picture */

    void (*enter)(void);        /* the tile was opened; allocate here */
    void (*leave)(void);        /* closing; whatever is still allocated is freed after this */
    void (*draw)(void);         /* draw one frame with the lap_put_* calls */
    bool (*key)(lap_key_t key, char ch);  /* true when used; an unused BACK returns to HOME */
    bool (*touch)(int col, int row);      /* a tap, in the app's own cells */

    /* The receiver to keep running while the app is open, so lap_value_* has
       live numbers: LAP_RADIO_FM, _P25, _ADSB or _REC. Empty: none, and the
       board parks the radio as it does for any screen that does not need it. */
    char radio[8];
} lap_app_t;

#define LAP_RADIO_FM   "FM"
#define LAP_RADIO_P25  "P25"
#define LAP_RADIO_ADSB "ADS-B"
#define LAP_RADIO_REC  "REC"

/* API 1.3: asks on EVERY launch, including signed apps. BACK denies and
   leaves the app closed. Old apps without this flag cannot use TCP. */
#define LAP_CAP_NETWORK 1u
/* API 2.0: receive. Lists the radios, opens an I/Q session, tunes, sets gain
   and rate, reads I/Q and spectrum lines, taps the receiver's audio,
   subscribes to decoded pages, APRS, AIS, ADS-B and mesh messages, and calls
   the board's tuning actions (fm.freq, p25.tune ...). Asked ONCE per install:
   the board remembers the answer for this id, version and file size, and asks
   again when any of them changes. BACK denies and leaves the app closed. */
#define LAP_CAP_RADIO_RX 2u
/* API 2.0: transmit actions (mesh.send, mesh.advert). Asked with RADIO_RX.
   The board's own transmit rules still decide: MeshCore sends only with
   TRANSMIT armed in MESH. There is no raw transmit call in 2.0. */
#define LAP_CAP_RADIO_TX 4u
/* API 2.1: a background service. The app may start a small tick
   (lap_service_start) that keeps running after its screen closes, until the
   app stops it, the owner presses STOP in RADIOS, another app opens, or a
   tick runs too long. Asked once per install, with RADIO_RX. */
#define LAP_CAP_SERVICE 8u

typedef struct {
    bool connected;            /* LINK station interface has IPv4; worker snapshot */
    bool allowed;              /* this launch has NETWORK permission */
    char ip[16];               /* local IPv4, empty when disconnected */
} lap_net_status_t;
enum {
    LAP_TCP_PENDING = 0, LAP_TCP_CONNECTED = 1,
    LAP_NET_EINVAL = -1, LAP_NET_EACCES = -2, LAP_NET_EBUSY = -3,
    LAP_NET_EAGAIN = -4, LAP_NET_EDNS = -5, LAP_NET_ETIMEOUT = -6,
    LAP_NET_EIO = -7, LAP_NET_EOF = -8, LAP_NET_ENETDOWN = -9
};
/* All calls return immediately. Two outbound IPv4 sockets per app. No Wi-Fi
   credentials or join operation: first connect using LINK. Connect returns
   a handle >=0 or an error; poll state until CONNECTED or a negative error.
   Timeout (1..30000 ms) covers asynchronous DNS + connect, not stream lifetime.
   Host is DNS name or numeric IPv4, <=253 chars. IPv6/TLS/listening unsupported.
   send copies up to 4096 bytes into a bounded queue and returns bytes accepted
   (may be partial), EAGAIN if full/pending. Acceptance is not delivery.
   recv copies up to 4096 bytes, EAGAIN when empty/pending, 0 on peer EOF after
   queued bytes drain. Check negative errors; retry EAGAIN in later frames.
   Buffers/host/status must be app globals or app malloc allocations, not stack.
   Close invalidates the handle immediately; worker releases it asynchronously.
   Closing the app closes every socket and discards queued bytes. */
int lap_net_status(lap_net_status_t *out);
int lap_tcp_connect(const char *host, int port, uint32_t timeout_ms);
int lap_tcp_state(int handle);
int lap_tcp_send(int handle, const void *buf, size_t len);
int lap_tcp_recv(int handle, void *buf, size_t len);
void lap_tcp_close(int handle);

/* Declares the app: manifest fields and callbacks, in any order. */
#define LAP_APP(...)                                                              \
    __attribute__((section(".lap_app"), used, visibility("default")))             \
    const lap_app_t lap_app = { .magic = LAP_MAGIC, .api_major = LAP_API_MAJOR,  \
                                .api_minor = LAP_API_MINOR, __VA_ARGS__ };        \
    __attribute__((visibility("default"))) const lap_app_t *lap_main(void)       \
    { return &lap_app; }                                                          \
    typedef int lap_app_declared_

/* ---- drawing: only inside draw() --------------------------------------- */

int  lap_cols(void);            /* the app's area, in cells */
int  lap_rows(void);
void lap_clear(uint8_t attr);
void lap_put_str(int x, int y, const char *s, uint8_t attr);
void lap_put_cell(int x, int y, int16_t ch, uint8_t attr);
void lap_fill(int x, int y, int w, int h, int16_t ch, uint8_t attr);
void lap_hline(int x, int y, int len, int16_t ch, uint8_t attr);
void lap_vline(int x, int y, int len, int16_t ch, uint8_t attr);
void lap_box(int x, int y, int w, int h, const char *title, uint8_t attr);
/* A 3x5 character made of blocks (0-9, A-Z); false when there is no figure. */
bool lap_big_char(int x, int y, char c, uint8_t attr);
/* An RGB565 picture over the cells x,y,w,h, scaled to fit. `pixels` must stay
   valid until the next draw; change `serial` whenever the pixels change. */
void lap_image(int x, int y, int w, int h, const uint16_t *pixels, int pw, int ph, uint32_t serial);

/* ---- time, chance, logging --------------------------------------------- */

uint32_t lap_millis(void);      /* since the app was opened */
uint32_t lap_frame(void);       /* draws since the app was opened */
uint32_t lap_random(void);      /* hardware random */
void     lap_log(const char *fmt, ...);  /* to the console, prefixed with the app id */
void     lap_exit(void);        /* back to HOME after this callback returns */

/* ---- the radio, read only ---------------------------------------------- */

/* The same named values .lsapp layouts read: p25.freq, fm.level, adsb.aircraft,
   sys.uptime ... (USER_APPS.md has the list). False when there is no value. */
bool lap_value_text(const char *path, char *out, size_t cap);
bool lap_value_float(const char *path, float *out);

/* ---- sound ------------------------------------------------------------- */

enum { LAP_SOUND_KEY = 0, LAP_SOUND_ROGER, LAP_SOUND_CHIRP, LAP_SOUND_ERROR };
void lap_sound(int which);      /* at the board's volume; silent when muted */
/* API 1.2: speak, in the voice chosen in SETTINGS, at the board's volume. Queues
   and returns at once; false when there is no voice or the queue is full (a few
   lines wait). Keep it short: one line of plain words. */
bool lap_say(const char *text);
void lap_say_stop(void);        /* stop speaking and drop what waits */

/* ---- files: the app's own folder only ---------------------------------- */

/* Names are plain file names inside /sdcard/apps_data/<id>/ - no slashes, no
   "..". Up to 4 open at once; all are closed when the app closes. Files
   are cached in the 4 MB app PSRAM budget before enter(). Changes persist
   after leave(), on the IO worker; a crash can lose unflushed changes. */
int  lap_file_open(const char *name, const char *mode);   /* "r", "w", "a"; <0 on error */
int  lap_file_read(int fd, void *buf, size_t n);
int  lap_file_write(int fd, const void *buf, size_t n);
void lap_file_close(int fd);
bool lap_file_remove(const char *name);
long lap_file_size(const char *name);                    /* <0 when absent */

/* ======================================================================
   API 1.1: the board itself. Everything below needs firmware 2.19 or newer;
   an app that calls any of it will not load on 2.18.
   ====================================================================== */

/* ---- touch, as a finger rather than a tap ------------------------------ */

/* touch() gets completed taps. These follow the finger while it is down,
   for sliders, joysticks and games. Positions are in the app's own pixels:
   x 0..lap_cols()*cell width, y 0..lap_rows()*cell height (lap_cell_px). */
typedef struct {
    bool down;                  /* a finger is on the glass */
    int16_t x, y;               /* where it is now */
    int16_t x0, y0;             /* where it went down */
    uint32_t held_ms;           /* how long it has been down */
} lap_touch_t;
/* False when no finger has touched the app since it opened. */
bool lap_touch(lap_touch_t *out);

enum { LAP_SWIPE_NONE = 0, LAP_SWIPE_UP, LAP_SWIPE_DOWN, LAP_SWIPE_LEFT, LAP_SWIPE_RIGHT };
/* The direction of a swipe that ended since the last call, once each.
   A swipe is a drag of at least a cell and a half that is not a tap. */
int lap_swipe(void);

/* ---- the screen ------------------------------------------------------- */

void lap_cell_px(int *w, int *h);       /* one cell, in pixels */
/* The board theme's colour 0-15 (LAP_* | LAP_BRIGHT) as RGB565, so a
   picture drawn with lap_image matches the text around it. */
uint16_t lap_palette(int colour);

/* Dots, the way COMPASS draws: a cell holds 2 x 3 dots and shows the two
   commonest colours among its six, so ordered dither and sharp edges both
   survive. `dots` is (w*2) x (h*3) bytes, row by row, each a colour 0-15
   or LAP_DOT_CLEAR (black). Draws over cells x,y,w,h. */
#define LAP_DOT_CLEAR 0xFF
void lap_dots(int x, int y, int w, int h, const uint8_t *dots);

/* Text over the lap_image picture: the glyph in fg with a one pixel halo
   in bg, and the picture everywhere else in the cell. */
void lap_put_glass(int x, int y, const char *s, uint8_t attr);

/* Hide the tab strip: more rows for the app. The board then draws a close
   button over the top right 7 x 3 cells and takes any tap in the top right
   10 x 5 to go HOME; keep that corner clear. */
void lap_fullscreen(bool on);
/* API 1.2: the whole screen, tab strip and close button both gone - for games
   that draw their own cockpit. The app must give a visible way out on the glass
   that calls lap_exit(); an unused BACK key still goes HOME. */
void lap_immersive(bool on);
void lap_hold_rotation(bool on);        /* keep the current orientation while tilting */
void lap_keep_awake(bool on);           /* no auto-dim while the app is open */
void lap_brightness(int percent);       /* 5..100 for now, -1 the user's level; restored on close */

/* ---- motion: the 9-axis sensor ---------------------------------------- */

/* In the axes of the screen as it is showing now: +x to its right edge, +y
   to its top edge, +z out of the glass toward you. Turning the board to
   landscape turns the axes with the picture. */
typedef struct {
    float ax, ay, az;           /* g: the "up" the board feels; flat face up is (0,0,1).
                                   Something sliding on the glass goes toward (-ax, -ay). */
    float gx, gy, gz;           /* deg/s about x, y, z */
    float mx, my, mz;           /* uT, raw field; see mag_valid */
    float pitch;                /* deg, top edge raised is positive */
    float roll;                 /* deg, right edge lowered is positive */
    float heading;              /* deg 0-360 magnetic, tilt compensated; -1 when unknown */
    float true_heading;         /* deg 0-360 with declination; -1 when unknown */
    float temp_c;               /* the sensor's die */
    bool mag_valid;
    uint32_t age_ms;            /* since the sample was taken */
} lap_motion_t;
/* False when the board has no motion sensor. Samples arrive at 20 Hz;
   asking more often returns the same one. The first call starts the
   compass's calibrated solver, so heading may be -1 for the first second. */
bool lap_motion(lap_motion_t *out);

/* ---- sound and touch feedback ----------------------------------------- */

enum { LAP_WAVE_SINE = 0, LAP_WAVE_SQUARE, LAP_WAVE_TRIANGLE, LAP_WAVE_SAW, LAP_WAVE_NOISE };
/* Queue one note: hz 0 is a rest. Notes play back to back, without gaps,
   at the board's volume; up to 32 wait. volume 0..100 of full scale. */
bool lap_tone(int hz, int ms, int volume, int wave);
void lap_tone_stop(void);               /* drop whatever is still queued */
bool lap_tone_busy(void);

enum { LAP_HAPTIC_TICK = 0, LAP_HAPTIC_CLICK, LAP_HAPTIC_CONFIRM, LAP_HAPTIC_ALERT, LAP_HAPTIC_WARN };
bool lap_haptic(int effect);            /* false without a motor */
bool lap_vibrate(int ms, int strength); /* ms up to 2000, strength 0..100 */

/* ---- where and when --------------------------------------------------- */

typedef struct {
    bool fix;                   /* a current position */
    double lat, lon;            /* degrees */
    float alt_m, speed_kmh, course_deg, hdop;
    int sats_used, sats_visible;
    int64_t utc;                /* Unix seconds from the satellites, 0 without */
} lap_gps_t;
bool lap_gps(lap_gps_t *out);           /* false when there is no receiver */

typedef struct {
    int year, month, day, hour, minute, second, weekday;  /* local; weekday 0 is Sunday */
    int64_t unix;
    bool synced;                /* set from GPS, the network or the RTC */
} lap_time_t;
void lap_time(lap_time_t *out);

typedef struct {
    int percent;                /* 0..100 */
    int millivolts, milliamps;  /* milliamps positive while charging */
    float temp_c;
    bool charging;
} lap_battery_t;
bool lap_battery(lap_battery_t *out);   /* false without a gauge */

/* ---- MeshCore --------------------------------------------------------- */

/* The board's own mesh node. Reads are a copy the app worker refreshes
   every two seconds while the app asks; nothing here blocks a frame. */
typedef struct {
    bool running;               /* the mesh has the radio */
    bool tx_armed;              /* TRANSMIT is on in MESH; sends need it */
    char name[24];
    char id[17];                /* hex, first 8 bytes of the public key */
    uint32_t rx_packets, tx_packets;
    float last_rssi, last_snr;
} lap_mesh_self_t;
bool lap_mesh_self(lap_mesh_self_t *out);   /* false until the first refresh */

typedef struct {
    char id[17];
    char name[20];
    uint8_t type;               /* 1 chat, 2 repeater, 3 room, 4 sensor */
    bool has_loc;
    double lat, lon;
    float rssi, snr;
    uint32_t heard_s;           /* seconds since last heard */
    uint32_t adverts;
} lap_mesh_node_t;
int lap_mesh_nodes(lap_mesh_node_t *out, int max);   /* most recently heard first, up to 12 */

typedef struct {
    uint32_t id;                /* stays the same while the board is up */
    bool mine, direct;
    uint8_t channel;            /* 0 is public */
    uint8_t state;              /* 0 received, 1 sending, 2 sent, 3 heard relayed, 4 acked, 5 queued, 6 failed */
    char peer[17];              /* the other end of a direct message */
    char sender[20];
    char text[186];             /* "name: message" as it went on air */
    uint32_t age_s;
} lap_mesh_msg_t;
/* Oldest first, up to 24. lap_mesh_seq() changes whenever a message comes
   or goes, so an app can skip the copy when nothing did. */
int lap_mesh_messages(lap_mesh_msg_t *out, int max);
uint32_t lap_mesh_seq(void);
/* Send on a channel slot 0..3, or to a node by id (lap_mesh_dm). Queued for
   the app worker; lap_mesh_send_state says what became of the last one:
   1 waiting, 0 handed to the mesh, <0 refused (-1 transmit off, -2 busy,
   -3 too long, -4 no mesh). Text up to 150 bytes. */
bool lap_mesh_send(int channel, const char *text);
bool lap_mesh_dm(const char *id, const char *text);
bool lap_mesh_advert(void);
int  lap_mesh_send_state(void);

/* ---- Wi-Fi and Bluetooth, listening ------------------------------------ */

typedef struct {
    char ssid[33];
    int8_t rssi;
    uint8_t channel;
    bool secure;
} lap_wifi_ap_t;
bool lap_wifi_scan(void);               /* start a scan; results arrive in a few seconds */
int  lap_wifi_aps(lap_wifi_ap_t *out, int max);  /* the last scan, up to 16 */
uint32_t lap_wifi_seq(void);            /* changes with each finished scan */

typedef struct {
    uint8_t addr[6];
    char name[20];
    int8_t rssi;
    float smooth_rssi;
    uint32_t adverts;
    uint32_t age_ms;
} lap_ble_dev_t;
void lap_ble_scan(bool on);             /* listen for advertisers while the app is open */
int  lap_ble_devices(lap_ble_dev_t *out, int max);  /* strongest first, heard in the last 30 s, up to 24 */

/* ---- the SD card ------------------------------------------------------ */

/* The whole card, not only the app's folder, as on a Flipper. Paths start
   with /sdcard/. Reading works anywhere; writing, making folders and
   removing work anywhere except /sdcard/apps, where installed apps live.
   The card is never touched on the screen task: each call starts a job on
   the app worker and returns its number (<0 when refused). lap_sd_poll
   says how it went: LAP_SD_PENDING while it runs, then the job's result,
   which is a byte or entry count, or a negative LAP_SD_E*. The buffer a
   read or list fills is written during the lap_sd_poll call that reports
   it done, and only if it is still the app's memory. One job runs at a
   time and up to four wait; 64 KB per read or write. */
#define LAP_SD_PENDING  (-1000)
#define LAP_SD_ENOENT   (-2)
#define LAP_SD_EIO      (-5)
#define LAP_SD_EACCES   (-13)
#define LAP_SD_EBUSY    (-16)
#define LAP_SD_EINVAL   (-22)
#define LAP_SD_APPEND   0xFFFFFFFFu
typedef struct {
    char name[64];
    bool dir;
    uint32_t size;
} lap_sd_entry_t;
int lap_sd_read(const char *path, uint32_t offset, void *buf, uint32_t len);
/* offset 0 replaces the file, LAP_SD_APPEND adds to its end, anything else
   writes over the bytes there. The data is copied when the call is made. */
int lap_sd_write(const char *path, uint32_t offset, const void *data, uint32_t len);
int lap_sd_list(const char *dir, lap_sd_entry_t *out, int max);   /* up to 256 */
int lap_sd_size(const char *path);      /* result: the size in bytes */
int lap_sd_mkdir(const char *path);
int lap_sd_remove(const char *path);    /* a file or an empty folder */
int lap_sd_poll(int job);

/* API 1.2: copies of the receiver's current aircraft and position history. */
typedef struct {
    uint32_t icao;              /* 24-bit address */
    char callsign[9];           /* empty until heard */
    bool has_pos, has_alt, has_speed, has_track, has_vrate;
    double lat, lon;            /* degrees */
    int32_t alt_ft;             /* barometric */
    int16_t speed_kt, track_deg, vrate_fpm;
    uint16_t squawk;            /* as read: 7700; 0 unknown */
    uint8_t emergency;          /* 0 none, 1 general, 2 medical, 3 fuel,
                                  4 no comms, 5 unlawful, 6 downed */
    char category[3];           /* emitter set and number; empty if unknown */
    uint32_t age_ms;
    float range_km, bearing_deg; /* range < 0 without GPS fix or position */
} lap_aircraft_t;
int lap_aircraft(lap_aircraft_t *out, int max); /* nearest first; unknown ranges last */
int lap_aircraft_track(uint32_t icao, float *lat, float *lon, int32_t *alt_ft, int max); /* oldest first */
void lap_aircraft_demo(bool on); /* receiver synthetic traffic, not live reception */

#if !defined(LAP_TARGET_API_1)
/* ======================================================================
   API 2.0: the radios. Firmware 2.20 or newer. Everything here returns at
   once: the board's lap_io worker opens, tunes and closes radios, and its
   own capture tasks fill a bounded queue in PSRAM that these calls copy
   from. No app code ever runs in a capture task. Closing the app (or
   lap_exit, or a crash) closes the session, stops the capture and gives the
   radio back. Buffers passed here must be app globals or app malloc memory.
   ====================================================================== */

/* ---- which radios there are -------------------------------------------- */

enum {
    LAP_RADIO_KIND_RTL = 1,     /* RTL-SDR dongle on USB */
    LAP_RADIO_KIND_HACKRF,      /* HackRF One on USB */
    LAP_RADIO_KIND_LR2021,      /* the LR2021 in the LoRa socket: 125 kHz I/Q */
    LAP_RADIO_KIND_SX1262,      /* the SX1262 in the LoRa socket: no I/Q */
    LAP_RADIO_KIND_SIM,         /* the host simulator's test signal */
};
/* What a radio can do for an app. */
enum {
    LAP_RXF_IQ = 1,             /* lap_rx_open and lap_rx_read work */
    LAP_RXF_SPECTRUM = 2,       /* lap_rx_spectrum works */
    LAP_RXF_GAIN = 4,           /* lap_rx_gain works */
    LAP_RXF_RATE = 8,           /* lap_rx_rate works; otherwise min_rate is the only rate */
    LAP_RXF_WIDE = 16,          /* LAP_RX_WIDE is accepted (LR2021 wide front end) */
};
#define LAP_RADIOS_MAX 6
typedef struct {
    char name[16];              /* "RTL-SDR", "HackRF", "LR2021", "SX1262" */
    uint8_t kind;               /* LAP_RADIO_KIND_* */
    uint8_t features;           /* LAP_RXF_* */
    bool present;               /* plugged in and answering */
    bool busy;                  /* another receiver holds it right now */
    uint64_t min_hz, max_hz;    /* tuning range */
    uint32_t min_rate, max_rate;/* I/Q sample rates, pairs a second */
    int16_t gain_min, gain_max; /* tenths of a dB */
    char why[32];               /* empty when usable, else the reason in plain words */
} lap_radio_info_t;
/* Copies up to max radios (LAP_RADIOS_MAX is plenty) and returns how many,
   or a negative LAP_RX_E*. The index into this list is what lap_rx_open
   takes. The list is the worker's snapshot, refreshed about once a second;
   right after the app opens it may still be empty for a frame. */
int lap_radio_list(lap_radio_info_t *out, int max);

/* ---- a receive session ------------------------------------------------- */

enum {
    LAP_RX_OPENING = 0, LAP_RX_RUNNING = 1,
    LAP_RX_EINVAL = -1, LAP_RX_EACCES = -2, LAP_RX_EBUSY = -3, LAP_RX_EAGAIN = -4,
    LAP_RX_ENODEV = -5, LAP_RX_EUNSUPPORTED = -6, LAP_RX_EIO = -7, LAP_RX_ERANGE = -8,
    LAP_RX_ENOMEM = -9,
};
#define LAP_GAIN_AUTO (-32768)
#define LAP_RX_WIDE 1u              /* lap_rx_config_t.flags: LR2021 wide mode (576k front end, 125k out) */
#define LAP_RX_SPECTRUM_ONLY 2u     /* no I/Q queue: only lap_rx_spectrum lines (cheapest at wide rates) */
typedef struct {
    uint64_t center_hz;
    uint32_t rate_hz;           /* 0: the radio's lowest rate */
    int gain_tenths_db;         /* LAP_GAIN_AUTO or tenths of a dB */
    uint32_t flags;             /* LAP_RX_WIDE, LAP_RX_SPECTRUM_ONLY */
} lap_rx_config_t;
/* One session per app. Returns a handle >0 at once, or a negative error;
   the radio opens in the background, so poll lap_rx_state until RUNNING
   (or a negative error). The radio the app did not ask for stays parked,
   and a radio another receiver holds answers LAP_RX_EBUSY. */
int lap_rx_open(int radio, const lap_rx_config_t *cfg);
int lap_rx_state(int h);
typedef struct {
    int state;                  /* LAP_RX_OPENING, LAP_RX_RUNNING or a negative error */
    int radio;                  /* index in lap_radio_list */
    uint64_t center_hz;         /* what the radio is tuned to now */
    uint32_t rate_hz;
    int gain_tenths_db;         /* LAP_GAIN_AUTO when automatic */
    uint32_t queued, capacity;  /* blocks waiting / the queue's size (2048 pairs a block) */
    uint64_t delivered;         /* pairs lap_rx_read has handed over */
    uint64_t dropped;           /* pairs lost because the queue was full */
    uint32_t overflows;         /* times the queue filled up */
    uint32_t gaps;              /* blocks that start after a loss, retune or restart */
} lap_rx_status_t;
int lap_rx_status(int h, lap_rx_status_t *out);
/* Each change is queued for the worker and returns 0, or a negative error
   (ERANGE outside the radio's limits, EUNSUPPORTED where the radio cannot). */
int lap_rx_tune(int h, uint64_t hz);
int lap_rx_gain(int h, int tenths_db);
int lap_rx_rate(int h, uint32_t rate_hz);

/* I/Q comes as interleaved int16 pairs (I, Q, I, Q ...), full scale +-32767,
   at the rate in the block's meta. The queue holds 32 blocks of 2048 pairs;
   when the app falls behind the newest samples are dropped, counted, and the
   next block carries LAP_IQ_GAP. first_sample counts every pair the radio
   produced, dropped ones included, so a jump in it is the size of a gap. */
#define LAP_IQ_GAP     1u       /* samples were lost before this block */
#define LAP_IQ_RETUNED 2u       /* first block after a tune, rate or gain change */
typedef struct {
    uint64_t first_sample;
    uint64_t center_hz;
    uint32_t rate_hz;
    uint32_t flags;             /* LAP_IQ_* */
} lap_iq_meta_t;
/* Copies up to max_pairs pairs from the oldest waiting block (never across
   two blocks) and returns how many, 0 when nothing waits, or a negative error.
   meta may be NULL. Read in a loop until 0, within a frame's budget. */
int lap_rx_read(int h, int16_t *iq, int max_pairs, lap_iq_meta_t *meta);
/* The latest averaged power spectrum, dB relative to full scale, low
   frequency first (centre - rate/2 to centre + rate/2), squeezed to `bins`
   (8..512) by keeping each group's peak. Returns bins, or 0 before the first
   line. *seq (may be NULL) changes with every new line, about 10 a second. */
int lap_rx_spectrum(int h, float *db, int bins, uint32_t *seq);
void lap_rx_close(int h);

/* ---- audio ------------------------------------------------------------- */

#define LAP_AUDIO_RATE 16000    /* mono int16, both ways */
enum { LAP_AUDIO_EACCES = -2, LAP_AUDIO_EINVAL = -1 };
/* The demodulated audio of the receiver the app keeps running (its manifest
   `radio`, such as LAP_RADIO_FM), as it goes to the speaker, before volume.
   Needs LAP_CAP_RADIO_RX. A 1 second ring: read every frame or lose the oldest. */
int lap_audio_listen(bool on);
int lap_audio_read(int16_t *pcm, int max);  /* samples copied, 0 when none */
/* Play PCM on the speaker at the board's volume. Copied into a 1/2 second
   queue; returns how many samples were taken (0 when full: offer the rest next
   frame). Muted, or while live radio audio has the speaker, the samples are
   taken and dropped at playing speed. Speech is never interrupted by it. */
int lap_audio_write(const int16_t *pcm, int n);
typedef struct {
    bool listening;             /* lap_audio_listen is on */
    bool muted;                 /* the board is muted */
    bool live;                  /* live radio audio has the speaker */
    int volume;                 /* the board's volume, 0..100 */
    uint32_t heard_dropped;     /* receiver samples lost because nobody read them */
    uint32_t play_queued;       /* samples waiting to play */
    uint32_t play_dropped;      /* samples taken and dropped (muted or live radio) */
} lap_audio_status_t;
int lap_audio_status(lap_audio_status_t *out);

/* ---- decoded data ------------------------------------------------------ */

/* What the board's own decoders hear, as they hear it. Needs
   LAP_CAP_RADIO_RX. The decoders run only while their receiver does, so an
   app that wants pages or AIS names that receiver in its manifest (`radio`,
   e.g. LAP_RADIO_FM with FM on the pager or AIS channel). */
enum { LAP_EV_PAGE = 1, LAP_EV_APRS = 2, LAP_EV_AIS = 4, LAP_EV_ADSB = 8, LAP_EV_MESH = 16 };
typedef struct {
    uint8_t kind;               /* LAP_EV_*; 0 for kinds past 128 (see kind16) */
    bool has_pos;
    uint16_t kind16;            /* API 2.1: the whole LAP_EV_* kind, LAP_EV_LORA and LAP_EV_TEXT too */
    uint32_t seq;               /* counts every event this app was offered */
    uint32_t address;           /* pager RIC, AIS MMSI, ADS-B ICAO; 0 otherwise */
    uint32_t age_ms;            /* since it was decoded */
    double lat, lon;            /* when has_pos */
    int32_t alt_ft;             /* ADS-B barometric altitude; 0 otherwise */
    char from[24];              /* callsign, vessel or sender name, "POCSAG"/"FLEX" */
    char text[160];             /* page text, APRS comment or message, mesh text */
} lap_event_t;
/* Which kinds to hear; 0 stops. Events from before the call are not replayed.
   The board keeps one timeline of the last 256 decodes for every reader; an
   app that falls further behind loses the oldest, counted in lap_event_dropped. */
int lap_event_subscribe(uint32_t mask);
int lap_event_next(lap_event_t *out);       /* 1 copied, 0 none, <0 error */
uint32_t lap_event_dropped(void);

/* ---- decoder output ---------------------------------------------------- */

/* What a decoder hands the board: the fields its own decoders put on the
   timeline. A 2.0 app's lap_emit answers LAP_RX_EUNSUPPORTED; from 2.1 it
   puts the decode on the board's timeline (below). Kinds after LAP_EV_MESH,
   in the same bit order: SAME 32, ACARS 64, SUBGHZ 128, LORA 256, TEXT 512.
   lap_decoder_t is still reserved: the shape the radio head loads decoders in. */
typedef struct {
    uint16_t kind;              /* LAP_EV_* */
    bool has_pos;
    uint32_t address;
    double lat, lon;
    int32_t alt_ft;
    char from[24];
    char text[160];
} lap_decoded_t;
int lap_emit(const lap_decoded_t *d);
/* Inputs a decoder can take, preference order (the head's ls_rx_plugin.h). */
enum {
    LAP_IN_IQ_U8 = 1, LAP_IN_IQ_S16 = 2, LAP_IN_DISC = 4, LAP_IN_ENV = 8, LAP_IN_PCM16 = 16,
    LAP_IN_BITS = 32, LAP_IN_BATCH = 64, LAP_IN_PACKET = 128, LAP_IN_EDGES = 256, LAP_IN_RSSI = 512,
};
enum { LAP_FRONT_NONE = 0, LAP_FRONT_NFM, LAP_FRONT_AM, LAP_FRONT_WFM };
/* What a decoder needs, field for field the head's ls_rx_needs_t. */
typedef struct {
    uint32_t inputs;            /* LAP_IN_* */
    uint8_t front;              /* LAP_FRONT_* */
    uint32_t channel_bw_hz;
    uint32_t min_rate_hz;
    int32_t offset_hz;
    uint64_t freq_lo_hz, freq_hi_hz;
    uint32_t radios;            /* radio bits the head's picker uses */
    int16_t rsel_job;           /* the board's radio-choice job, -1 none */
    uint32_t psram_bytes, internal_bytes;
    uint16_t cpu_permille;
    uint32_t flags;             /* AUDIO, TUNES, EXCLUSIVE, NEEDS_LR2021, OWNS_RADIO, TRYING */
} lap_decoder_t;

/* ---- the board's actions ----------------------------------------------- */

/* The same named actions the console and .lsapp files use: "fm.freq" "162.4",
   "p25.tune", "fm.gain" "30" ... `args` is the arguments as text, separated by
   spaces (NULL or "" for none). `out` (may be NULL) gets the action's answer
   as text. An app may always call read-only actions; LAP_CAP_RADIO_RX adds
   tuning actions and LAP_CAP_RADIO_TX the transmit ones (which still need
   TRANSMIT armed). Actions that move the screen, write settings or switch
   power are refused. */
enum {
    LAP_ACT_OK = 0, LAP_ACT_UNKNOWN = -1, LAP_ACT_DENIED = -2, LAP_ACT_BADARG = -3,
    LAP_ACT_UNAVAILABLE = -4, LAP_ACT_BUSY = -5, LAP_ACT_FAILED = -6,
};
int lap_action(const char *name, const char *args, char *out, size_t cap);
/* The table, for discovery: false past the end. sig is one letter an
   argument: i integer, f number, b on/off, s text. */
int  lap_action_count(void);
bool lap_action_info(int index, char *name, size_t name_cap, char *sig, size_t sig_cap);

/* ---- notices ----------------------------------------------------------- */

/* A banner over whatever screen is showing, in the app's colour, titled with
   the app's name. Quiet: no sound. At most one a second. */
bool lap_notify(const char *title, const char *body);
/* A detector-style alert: a steady banner shown even over the app itself,
   with the board's ring and vibrate settings. At most one every 5 seconds. */
enum { LAP_ALERT_INFO = 0, LAP_ALERT_WARN, LAP_ALERT_ALARM };
bool lap_alert(const char *title, const char *body, int level);

#if defined(LAP_API_21)
/* ======================================================================
   API 2.1: background services and decoder output. Firmware 2.21 or newer.
   The board's own controls (keys, OPTIONS, lists, keypads, the radio glass,
   meters, the waterfall) are in lakeshark_ui.h.
   ====================================================================== */

/* ---- decoder output ---------------------------------------------------- */

enum { LAP_EV_SAME = 32, LAP_EV_ACARS = 64, LAP_EV_SUBGHZ = 128, LAP_EV_LORA = 256, LAP_EV_TEXT = 512 };
/* lap_emit (declared above) puts one decode on the board's timeline, the
   one lap_event_next reads and the radio head's decoded list shows, tagged
   with this app's id. Needs LAP_CAP_RADIO_RX. `kind` is one LAP_EV_* bit;
   from and text are cut at their size. Up to 4 a second, in bursts of up to
   8: more answers LAP_RX_EAGAIN and is counted; nothing waits. 0 when it
   went on, else LAP_RX_EINVAL, LAP_RX_EACCES or LAP_RX_EAGAIN. As with the
   board's own decoders, a decode nobody is reading is not kept. A 2.1 app
   subscribes to the new kinds with lap_event_subscribe like the old ones. */
uint32_t lap_emit_dropped(void);        /* decodes refused by the rate limit */
/* Who decoded the event lap_event_next returned last: "" the board's own
   decoders, else the emitting app's id. Returns its length. */
int lap_event_source(char *out, size_t cap);

/* ---- background service ------------------------------------------------ */

/* A service is a small tick the board keeps calling after the app's screen
   closes: a decoder that keeps listening, a watcher that posts a notice.
   It needs LAP_CAP_SERVICE in the manifest (asked once per install).

   While the screen is closed the app stays loaded: its memory, its radio
   session and its event subscription stay as they were. Closing the screen
   calls leave() as usual but frees nothing; opening the tile again calls
   enter() again, with everything still there (check what you already have).
   The tick runs on the screen task whether the screen is open or not, every
   period_ms. It cannot draw (the drawing calls do nothing in it) and gets
   no keys; notices, events, the radio and the clock all work.

   Budget: a tick must return within LAP_SERVICE_BUDGET_US. Three ticks in a
   row over it, or one over ten times it, stop the service with a notice that
   says so. A tick that never returns stops the board; it restarts, names the
   app, and asks before the app runs again. No service starts by itself.

   One service runs at a time (LAP_SERVICES_MAX), and it is the app that is
   loaded: opening another native app stops it, with a notice. The owner
   sees it in RADIOS, with STOP, and on the console (`app svc`, `app stop`).
   When it stops while the screen is closed, `stopped` (if set) is called,
   then everything is freed and given back as when an app closes. */
#define LAP_SERVICES_MAX 1
#define LAP_SERVICE_BUDGET_US 4000u
typedef struct {
    void (*tick)(void);         /* required */
    void (*stopped)(void);      /* optional: the service ended while the screen was closed */
    uint32_t period_ms;         /* 50..10000; 0 is 100 */
} lap_service_t;
enum { LAP_SVC_OK = 0, LAP_SVC_EINVAL = -1, LAP_SVC_EACCES = -2, LAP_SVC_EBUSY = -3 };
/* From any callback. Copies *svc; calling it again changes the tick or the
   period. 0, or LAP_SVC_EACCES without LAP_CAP_SERVICE. */
int lap_service_start(const lap_service_t *svc);
/* Ends the service. Inside a tick with the screen closed, the app is
   unloaded after the tick returns, as lap_exit does. */
void lap_service_stop(void);
bool lap_service_running(void);
/* False inside a tick while the app's screen is closed. */
bool lap_screen_open(void);
typedef struct {
    uint32_t ticks;             /* since the service started */
    uint32_t last_us, worst_us; /* time a tick took */
    uint32_t over;              /* ticks over the budget */
} lap_service_stats_t;
int lap_service_stats(lap_service_stats_t *out);
#endif /* LAP_API_21 */

#if defined(LAP_API_22)
/* ======================================================================
   API 2.2: P25, sub-GHz and detector events, and what the board's own
   watchers see, for a detector. Firmware 2.22 or newer.
   ====================================================================== */

/* ---- more decoded kinds ------------------------------------------------- */

/* On the same timeline as the others, for lap_event_subscribe and lap_emit.
   LAP_EV_P25: a P25 call starting; address = talkgroup, from = "TG n",
   text = "unit n" when the source is known. LAP_EV_REC: something new on
   REC's sub-GHz watch; from = the radio, text = what it was. LAP_EV_DETECT:
   a detector's alert; from = the source, text = the rules that matched. */
enum { LAP_EV_P25 = 1024, LAP_EV_REC = 2048, LAP_EV_DETECT = 4096 };

/* ---- sightings ---------------------------------------------------------- */

/* What the board's watchers see, as they see it: SWEEP's Bluetooth and Wi-Fi
   categories, Remote ID, the LoRa drone-link listener, DRONES' drone watch,
   REC's watch and CATCH, the pager decoder and P25. Needs LAP_CAP_RADIO_RX.
   lap_seen_watch turns on the watchers the board starts for a listener
   (SWEEP's Bluetooth scan and Remote ID, the drone-link listener); the
   decoders report only while their own app runs them. 0 stops; closing the
   app stops it too, unless its service still runs.

   An address is never handed over: `entity` is a pseudonym that holds for
   this boot only. 64 wait; past that the newest are lost and counted in
   lap_seen_dropped. While an app watches LAP_SEEN_DRONE, drone watch's
   alerts are its to raise; otherwise the board raises them itself. */
enum {
    LAP_SEEN_RID = 1, LAP_SEEN_BLE = 2, LAP_SEEN_WIFI = 4, LAP_SEEN_LORA = 8,
    LAP_SEEN_DRONE = 32, LAP_SEEN_REC = 64, LAP_SEEN_CATCH = 128, LAP_SEEN_PAGER = 256,
    LAP_SEEN_P25 = 512,
};
/* SWEEP's categories, and where a device sits in its list. */
enum { LAP_CAT_CAMERA = 0, LAP_CAT_BODYCAM, LAP_CAT_DRONE, LAP_CAT_TRACKER, LAP_CAT_ATTACK, LAP_CAT_NONE = 255 };
enum { LAP_SECT_WITH_YOU = 0, LAP_SECT_NEW, LAP_SECT_PASSING, LAP_SECT_NONE = 255 };
enum {
    LAP_SEEN_F_VALIDATED = 1,   /* RID: a validated message; DRONE: rssi is calibrated dBm */
    LAP_SEEN_F_FLOOD = 2,       /* BLE, Wi-Fi: part of a burst of rotating addresses */
    LAP_SEEN_F_NEW = 4,         /* REC, CATCH: a pattern not heard before */
    LAP_SEEN_F_EMERGENCY = 8,   /* P25: the emergency flag */
    LAP_SEEN_F_APPROACHING = 16 /* DRONE: the signal is rising */
};
typedef struct {
    uint16_t source;            /* one LAP_SEEN_* bit */
    uint8_t category;           /* LAP_CAT_*: BLE, Wi-Fi; DRONE for LORA and DRONE */
    uint8_t section;            /* LAP_SECT_*: BLE, Wi-Fi */
    int8_t rssi;                /* dBm; DRONE without F_VALIDATED: relative dB; 0 unknown */
    uint8_t flags;              /* LAP_SEEN_F_* */
    uint16_t band;              /* LORA, DRONE: 868, 915 or 2400 (MHz) */
    uint64_t entity;            /* RID, BLE, Wi-Fi: this boot's pseudonym; DRONE: the link; else 0 */
    uint32_t a, b;              /* REC, CATCH: Hz and pattern; PAGER: capcode and function;
                                   P25: talkgroup and unit; LORA: channel and rate guess (Hz) */
    uint32_t hits_per_min;      /* LORA */
    uint32_t age_ms;            /* since it was seen */
    char text[64];              /* PAGER: the page; REC, CATCH: "OOK24" or the radio */
} lap_seen_t;
int lap_seen_watch(uint32_t sources);       /* 0, LAP_RX_EACCES or LAP_RX_EINVAL */
int lap_seen_next(lap_seen_t *out);         /* 1 copied, 0 none, <0 error */
uint32_t lap_seen_dropped(void);

/* A mesh channel slot (1..3) only those holding its key can read: not the
   public channel and not a hashtag channel anyone can derive. Fills its
   name when `name` is not NULL. From the copy the app worker refreshes. */
bool lap_mesh_channel_private(int slot, char *name, size_t cap);
#endif /* LAP_API_22 */
#endif /* !LAP_TARGET_API_1 */

/* ---- memory and the C library ------------------------------------------ */

/* The SDK wraps non-PIC compiler helpers as __wrap___<libgcc-name> imports.
   These are resolved only to the board's matching soft-double and 64-bit
   arithmetic helpers, never to an app-supplied address. */

/* malloc, calloc, realloc and free work and come from PSRAM; whatever is not
   freed is freed when the app closes. Also available: memcpy memset memmove
   memcmp strlen strnlen strcmp strncmp strcpy strncpy strcat strchr strrchr
   strstr snprintf vsnprintf atoi strtol strtoul strtof abs qsort, and from
   math.h sinf cosf tanf atan2f atanf sqrtf fabsf floorf ceilf roundf powf
   expf logf fmodf, and since 1.1 asinf acosf fminf fmaxf hypotf. Convert a
   64-bit integer to float through a 32-bit one or a double: the direct
   conversion is a libgcc helper the board does not export. */
void *malloc(size_t n);
void *calloc(size_t n, size_t size);
void *realloc(void *p, size_t n);
void  free(void *p);

#ifdef __cplusplus
}
#endif
#endif /* LAKESHARK_APP_H */

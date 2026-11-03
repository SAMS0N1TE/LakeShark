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
 * Screen/service callbacks run on the screen task (about 25 frames a second).
 * Return quickly: a screen callback that loops forever restarts the board;
 * the next boot names the app. API 2.6 custom DSP has a separate, preemptible
 * worker callback and its own pure-computation contract below.
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

/* The API an app declares. Firmware with API 2.6 runs every 1.x app (1.0 to
   1.3), every 2.0 through 2.6 app unchanged; older firmware refuses a
   newer app with a clear message. An app that wants to keep running on 1.3
   firmware (2.18 / 2.19) defines LAP_TARGET_API_1 before including this
   header: it is then built as a 1.3 app and the 2.x calls are not declared.
   LAP_TARGET_API_20 does the same for 2.0 firmware: the 2.1 calls (services,
   lap_emit, the shared controls in lakeshark_ui.h) are not declared.
   LAP_TARGET_API_21 builds for 2.1 firmware: the 2.2 calls are not declared.
   LAP_TARGET_API_22 builds for 2.2 firmware: the 2.3 calls are not declared.
   LAP_TARGET_API_23 builds for 2.3 firmware: the 2.4 calls are not declared.
   LAP_TARGET_API_24 builds for 2.4 firmware: the 2.5 calls are not declared.
   LAP_TARGET_API_25 builds for 2.5 firmware: the 2.6 calls are not declared.
   API 2.5 games/assets: include lib/lap_assets.h, lap_game.h, lap_music.h.
   These helpers compile into the app and add no firmware imports. PNG assets
   use caller-owned bounded arenas; SD reads and PCM use existing services. */
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
#elif defined(LAP_TARGET_API_22)
#define LAP_API_MAJOR 2
#define LAP_API_MINOR 2 /* API 2.2: P25, sub-GHz and detector events; what the board's watchers see. */
#define LAP_API_21 1
#define LAP_API_22 1
#elif defined(LAP_TARGET_API_23)
#define LAP_API_MAJOR 2
#define LAP_API_MINOR 3
#define LAP_API_21 1
#define LAP_API_22 1
#define LAP_API_23 1
#elif defined(LAP_TARGET_API_24)
#define LAP_API_MAJOR 2
#define LAP_API_MINOR 4
#define LAP_API_21 1
#define LAP_API_22 1
#define LAP_API_23 1
#define LAP_API_24 1
#elif defined(LAP_TARGET_API_25)
/* API 2.5: system services, games/assets, input devices and sensors.
   Helpers compile into apps; descriptor stays 228 bytes. */
#define LAP_API_MAJOR 2
#define LAP_API_MINOR 5
#define LAP_API_21 1
#define LAP_API_22 1
#define LAP_API_23 1
#define LAP_API_24 1
#define LAP_API_25 1
#else
/* API 2.6: HackRF TX / desktop host and Bluetooth peripheral roles.
   API 2.5: system services, games/assets, input devices and sensors.
   Helpers compile into apps; descriptor stays 228 bytes. */
/* API 2.6 services: concurrent residents and budgeted custom DSP. */
#define LAP_API_MAJOR 2
#define LAP_API_MINOR 6
#define LAP_API_21 1
#define LAP_API_22 1
#define LAP_API_23 1
#define LAP_API_24 1
#define LAP_API_25 1
#define LAP_API_26 1
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
    LAP_KEY_F12, LAP_KEY_HOME, LAP_KEY_END, LAP_KEY_PGUP, LAP_KEY_PGDN,
    LAP_KEY_INSERT, LAP_KEY_DELETE, LAP_KEY_MENU, LAP_KEY_SHIFT,
    LAP_KEY_CAPS_LOCK, LAP_KEY_NUM_LOCK, LAP_KEY_MEDIA_PLAY, LAP_KEY_MEDIA_STOP,
    LAP_KEY_MEDIA_NEXT, LAP_KEY_MEDIA_PREV, LAP_KEY_VOLUME_UP,
    LAP_KEY_VOLUME_DOWN, LAP_KEY_MUTE,
    LAP_KEY_HID_USAGE_BASE = 0x100,
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
/* Transmit actions and direct mesh sends require this declaration. No separate
   TX confirmation is required. Hardware power/duty/PA protection still applies;
   MeshCore also requires its own TRANSMIT switch armed. No raw TX call in 2.0. */
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
/* All calls return immediately. Six outbound IPv4 sockets per app (two on
   firmware before API 2.4). Join using LINK or the API 2.4 Wi-Fi service. Connect returns
   a handle >=0 or an error; poll state until CONNECTED or a negative error.
   Timeout (1..30000 ms) covers asynchronous DNS + connect, not stream lifetime.
   Host is DNS name or numeric IPv4, <=253 chars. IPv6/TLS/listening unsupported.
   send copies up to 4096 bytes into a bounded queue and returns bytes accepted
   (may be partial), EAGAIN if full/pending. Acceptance is not delivery.
   recv copies up to 4096 bytes, EAGAIN when empty/pending, 0 on peer EOF after
   queued bytes drain. Check negative errors; retry EAGAIN in later frames.
   Host strings and send/receive/status buffers are copied synchronously and may live on the callback stack.
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
   radio back. Synchronous buffers may use the active callback stack;
   only retained data requires app globals or live app allocations.
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
/* Synchronous inputs and outputs (including I/Q metadata and spectrum seq)
   may live on the active callback stack: they are checked and copied before
   return. Retained pointers, such as SD read/list destinations, must remain
   in the app image or live app malloc memory until the job completes. */
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

   API 2.6 firmware retains several services with a foreground app. Query
   lap_service_limit() on a 2.6 target; older firmware retained only one.
   The owner sees them in SETTINGS > SERVICES, with individual STOP, and
   on the console (`app svc`, `app stop <id>`).
   When it stops while the screen is closed, `stopped` (if set) is called,
   then everything is freed and given back as when an app closes. */
#if defined(LAP_API_26)
#define LAP_SERVICES_MAX 4 /* compile-time ceiling; query lap_service_limit */
#else
#define LAP_SERVICES_MAX 1
#endif
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


#if defined(LAP_API_24)
/* ---- API 2.4: radio I/O (R6-R11) ----------------------------------------
 * All calls run in app callbacks, return immediately, and copy inputs/outputs
 * synchronously (callback-stack buffers are valid). No caller buffer is retained.
 * Positive handles are generation-owned; unload closes all services. One packet,
 * edge OR scanner session, one recording/replay and one TX operation at a time.
 * QUEUED is admission, not hardware success; poll status for the actual result.
 * Configurations require size=sizeof(struct), version=1 and zero reserved fields.
 * Metadata timestamps are firmware monotonic arrival times, NOT RF sample clocks.
 * Read returns count/bytes, 0 empty, negative LAP_RX_E* or LAP_IO_E*.
 */
enum { LAP_RADIO_KIND_MIXRF = 6 };
enum {
    LAP_HW_PACKET = 1u << 8, LAP_HW_EDGES = 1u << 9,
    LAP_HW_RECORD = 1u << 10, LAP_HW_NFC = 1u << 11,
    LAP_HW_NDEF_WRITE = 1u << 12
};
enum { LAP_IO_QUEUED = 1, LAP_IO_RUNNING, LAP_IO_DONE, LAP_IO_FAILED, LAP_IO_CANCELLED };
enum {
    LAP_IO_ESIZE = -20, LAP_IO_EPOWER = -21, LAP_IO_EDUTY = -22,
    LAP_IO_ETHERMAL = -23, LAP_IO_ERXPORT = -24, LAP_IO_EFORMAT = -25,
    LAP_IO_ECANCELLED = -26, LAP_IO_ENOSPC = -27
};
enum { LAP_MOD_LORA = 1, LAP_MOD_FSK, LAP_MOD_OOK };
enum { LAP_PORT_AUTO, LAP_PORT_TX, LAP_PORT_RX_ONLY }; /* LR2021 MMCX1 is RX_ONLY */
enum {
    LAP_MODE_LORA = 1u, LAP_MODE_FSK = 2u, LAP_MODE_OOK = 4u,
    LAP_MODE_EDGES = 8u, LAP_MODE_RSSI = 16u,
    LAP_MODE_TX_PACKET = 32u, LAP_MODE_TX_IQ = 64u, LAP_MODE_TX_EDGES = 128u
};
enum { LAP_DATA_REPLAY = 1u, LAP_DATA_SYNTHETIC = 2u, LAP_DATA_GAP = 4u, LAP_DATA_RETUNED = 8u };
#define LAP_IQ_REPLAY 4u
#define LAP_IQ_SYNTHETIC 8u
#define LAP_PACKET_MAX 255
#define LAP_SCAN_MAX 64

typedef struct {
    uint16_t size, version;
    uint32_t modes;                 /* implemented backend modes, not silicon claims */
    int16_t min_power_dbm, max_power_dbm;
    uint32_t max_tx_us, duty_window_ms, duty_airtime_ms;
} lap_radio_modes_t;
/* Discovery needs no grant. Radio index is the index returned by lap_radio_list.
 * lap_caps.hardware advertises packet/edges/NFC/record service presence; this
 * query refines each radio. Unsupported modes return EUNSUPPORTED, absent ENODEV. */
int lap_radio_modes(int radio, lap_radio_modes_t *out);

typedef struct {
    uint16_t size, version;
    uint64_t frequency_hz;
    uint32_t bitrate, bandwidth_hz, deviation_hz, sync_word;
    uint16_t preamble;             /* LoRa symbols; FSK/OOK bits */
    uint8_t modulation, sync_bits; /* FSK 1..32; OOK 1..16 (bit 0 first) */
    uint8_t crc, sf, coding_rate, payload_bytes; /* LoRa: SF5..12, CR5..8; 0=explicit RX */
    int8_t power_dbm;              /* TX only; not clamped */
    uint8_t port, reserved[2];
} lap_packet_config_t;
typedef struct {
    uint16_t size, version;
    uint64_t timestamp_us, frequency_hz;
    uint32_t sequence, dropped, flags;
    int16_t rssi_tenths_dbm, snr_tenths_db; /* INT16_MIN when unavailable */
    uint16_t length, reserved;
} lap_packet_meta_t;
typedef struct {
    uint16_t size, version;
    int32_t state, error;
    uint32_t queued, dropped;
} lap_io_status_t;
/* RX grant required. Eight packets wait; overflow drops newest and is counted.
 * Too-small destinations return ESIZE WITHOUT consuming the packet. FSK CRC
 * unsupported by the current streaming backend is refused, never ignored. */
int lap_packet_open(int radio, const lap_packet_config_t *cfg);
int lap_packet_read(int h, void *data, size_t cap, lap_packet_meta_t *meta);
int lap_packet_status(int h, lap_io_status_t *out);
void lap_packet_close(int h);
/* Edge mode uses the OOK config above. Signed durations: positive high, negative
 * low, microseconds. LR OOK decisions are run-length encoded at cfg.bitrate;
 * MIX-RF delivers captured CC1101 edges. Each read is one bounded batch (<=256).
 * Queue gaps are explicit. Close with lap_packet_close; status is shared. */
int lap_edges_open(int radio, const lap_packet_config_t *cfg);
int lap_edges_read(int h, int32_t *durations_us, int max, lap_packet_meta_t *meta);
/* Cached RSSI from the worker; timestamp_us identifies the sample time. EAGAIN before first sample. */
int lap_radio_rssi(int h, lap_packet_meta_t *out);

typedef struct {
    uint16_t size, version;
    uint64_t frequency_hz;
    uint32_t id;
    uint8_t mode, flags, bank, reserved; /* firmware scan mode 0=P25,1=NFM,2=WFM */
    int32_t lat_e7, lon_e7;
    uint32_t radius_m;
    uint16_t tone, reserved2;
    char name[16];
} lap_channel_t;
/* Asynchronous paged firmware channel database access. bank="active" uses
 * firmware's live channel store; named banks are firmware scan binary profiles.
 * Query returns a job, result read returns EAGAIN until complete then 0..64 rows.
 * Writes replace a bank with <=64 rows in a staged firmware transaction.
 * Query first selects a page; no app path can escape the firmware scan folder. */
int lap_channels_query(const char *bank, uint32_t first, int count);
int lap_channels_read(int job, lap_channel_t *out, int max);
int lap_channels_write(const char *bank, const lap_channel_t *rows, int count);
/* Generic job status also covers channel writes. Completed job survives until
 * the next database request. No hardware or SD work happens in the callback. */
int lap_io_status(int job, lap_io_status_t *out);
typedef struct {
    uint16_t size, version;
    int32_t radio;
    lap_packet_config_t receiver;
    uint32_t dwell_ms, hang_ms;   /* 20..10000; 0..10000 */
    int16_t squelch_tenths_dbm;   /* radio-relative RSSI, not a calibrated meter */
    uint16_t count;              /* 1..64 copied frequencies */
    uint64_t frequencies[LAP_SCAN_MAX];
} lap_scan_config_t;
typedef struct {
    uint16_t size, version;
    int32_t state, error;
    uint32_t channel, sweeps, hits, dropped;
    uint64_t frequency_hz;
    uint8_t held, squelch_open, reserved[6];
} lap_scan_status_t;
enum { LAP_SCAN_HOLD = 1, LAP_SCAN_NEXT, LAP_SCAN_SKIP, LAP_SCAN_RESUME };
/* Scanner owns its selected packet/edge radio; frequency lists may come from
 * the database or be app-defined. Hits are rising squelch events, sixteen wait.
 * Dwell starts AFTER tuning, hang extends while carrier exceeds threshold.
 * SKIP suppresses this index until stop; NEXT leaves a manual hold. */
int lap_scan_start(const lap_scan_config_t *cfg);
int lap_scan_status(int h, lap_scan_status_t *out);
int lap_scan_control(int h, int command);
int lap_scan_next(int h, lap_packet_meta_t *hit);
void lap_scan_stop(int h);

enum { LAP_RECORD_IQ16 = 1, LAP_RECORD_AUDIO16, LAP_RECORD_PACKETS, LAP_RECORD_EDGES };
typedef struct {
    uint16_t size, version;
    int32_t source;              /* packet/edge handle; 0 for app RX I/Q/audio tap */
    uint32_t format, rate_hz, max_bytes; /* file cap 1KiB..64MiB; rate matches source */
    uint64_t frequency_hz;
    char name[33];               /* app-local plain filename, no slash; extension optional */
} lap_record_config_t;
typedef struct {
    uint16_t size, version;
    int32_t state, error;
    uint32_t format, blocks, dropped, flags;
    uint64_t bytes, position;    /* position: replay block index; seek uses block index */
} lap_record_status_t;
/* Records to /sdcard/apps_data/<app-id>/<name>, LSR2 little-endian framed
 * format (see RADIO_IO.md). Eight 2048-byte blocks wait; SD stalls count gaps.
 * Config/timestamps/RSSI/SNR, tuning/gaps and marks are stored with payloads.
 * Existing files are refused. Stop drains then closes; errors preserve partial
 * files. Poll status for completion before opening local replay. */
int lap_record_start(const lap_record_config_t *cfg);
int lap_record_status(int h, lap_record_status_t *out);
int lap_record_mark(int h, const char *label); /* <=63 bytes; queued metadata */
int lap_record_stop(int h);
/* Local replay NEVER emits RF. Packet/edge replay creates a virtual RX source
 * read with lap_packet_read/lap_edges_read using the replay handle. I/Q/audio
 * replay feeds the same app receive taps (open lap_rx_open(-1,...) for virtual
 * I/Q). All replay blocks carry REPLAY; decoders must propagate provenance.
 * Read failures/truncation are EFORMAT/EIO; completion is DONE. One block is
 * injected per worker pass; this is analysis replay, not sample-clock timing. */
int lap_replay_open(const char *name);
/* Format-neutral block read (<=2048 bytes), with REPLAY metadata. Use status.format
 * to interpret IQ16 pairs, mono audio16, packets or signed edge durations. */
int lap_replay_read(int h, void *data, size_t cap, lap_packet_meta_t *meta);
int lap_replay_seek(int h, uint64_t block);
int lap_replay_status(int h, lap_record_status_t *out);
void lap_replay_close(int h);

/* TX requires ONLY the manifest's LAP_CAP_RADIO_TX and hardware protection.
 * No region/band/licence or confirmation UI. One immutable copied operation.
 * Packet <=255 bytes, raw signed IQ8 <=32767 pairs, edges <=4096/10 seconds.
 * Hardware power ceiling, PA heat/duty budget and RX-only port are enforced
 * again at the physical send boundary, also for firmware LoRa/MeshCore/MIX-RF.
 * status error: EPOWER, EDUTY, ETHERMAL, ERXPORT or normal LAP_RX_E*.
 * Cancelling/unloading stops queued work; in-flight packet finishes its bounded
 * frame. Raw IQ observes cancel between FIFO chunks. No automatic retries. */
int lap_tx_packet(int radio, const lap_packet_config_t *cfg, const void *data, size_t len);
int lap_tx_iq(int radio, const lap_packet_config_t *cfg, const int8_t *iq, int pairs, uint32_t rate_hz);
int lap_tx_replay(int radio, const lap_packet_config_t *cfg, const char *name);
int lap_tx_status(int h, lap_io_status_t *out);
void lap_tx_cancel(int h);

/* Bounded NFC service. Availability is lap_caps LAP_HW_NFC; NDEF writes are
 * advertised separately. No register/raw-bus access. Read-only frontends return
 * EUNSUPPORTED for write; absent ones ENODEV. UID <=10 bytes, NDEF <=2048 bytes.
 * Eight events wait; overflow counts drops. Type-2 writes only on unlocked,
 * capacity-checked tags; no authentication keys or emulation exposed. */
typedef struct {
    uint16_t size, version;
    uint64_t timestamp_us;
    uint32_t kind, error, dropped; /* 1 discovered,2 read,3 removed,4 error,5 written */
    uint16_t ndef_bytes, capacity, atqa;
    uint8_t uid[10], uid_len, sak, writable, flags; /* LAP_DATA_SYNTHETIC on host fixtures */
} lap_nfc_event_t;
int lap_nfc_open(void);
int lap_nfc_next(int h, lap_nfc_event_t *out, void *ndef, size_t cap);
int lap_nfc_write(int h, const void *ndef, size_t len);
void lap_nfc_close(int h);
#endif /* LAP_API_24: radio I/O */

#if defined(LAP_API_23)
/* ---- API 2.3: discovery, input, dialogs and durable app data ------------ */
/* Queries are snapshots, never start hardware. Hardware flags describe firmware
   support/presence, independently of app grants. Radio masks use list indices;
   radio_kinds uses (1u << LAP_RADIO_KIND_*). Discovery needs no RX grant.
   Call from app callbacks. Synchronous outputs may live on the callback stack. */
enum {
    LAP_HW_WIFI = 1u, LAP_HW_BLE = 2u, LAP_HW_GPS = 4u, LAP_HW_IMU = 8u,
    LAP_HW_SD = 16u, LAP_HW_SPEAKER = 32u, LAP_HW_MIC = 64u,
    LAP_HW_DESKTOP = 128u,
    /* Optional SDK services; absent bits mean the calls return unsupported. */
    LAP_HW_WORKER_DSP = 1u << 22, LAP_HW_MDNS = 1u << 23
};
enum { LAP_SCREEN_SMALL, LAP_SCREEN_MEDIUM, LAP_SCREEN_LARGE };
typedef struct {
    uint16_t size, version; /* firmware fills size, version=1 */
    uint32_t hardware, granted, radio_present, radio_tx, radio_kinds;
    uint16_t width_px, height_px;
    uint8_t screen_class, radio_count;
    uint16_t reserved;
    uint8_t radio_kind[LAP_RADIOS_MAX]; /* kind at each radio-list index */
} lap_caps_t;
uint32_t lap_api_version(void); /* (major << 16) | minor */
int lap_caps(lap_caps_t *out, size_t size); /* 0 or LAP_RX_E* */

/* Key IDs: LAP_KEY_* for special keys; printable keys use 128 + unsigned
   ASCII (case folded to lowercase); raw HID keys use their 0x100+usage ID.
   Legacy terminals without release reports emit a down/up pulse. */
#define LAP_KEY_CHAR(ch) (128u + (uint8_t)(ch))
#define LAP_INPUT_KEY_WORDS 16
/* First seven event kinds mirror the firmware input channel. */
enum {
    LAP_INPUT_KEY_DOWN, LAP_INPUT_KEY_UP, LAP_INPUT_POINTER_MOVE,
    LAP_INPUT_POINTER_DOWN, LAP_INPUT_POINTER_UP, LAP_INPUT_WHEEL,
    LAP_INPUT_RELEASE_ALL, LAP_INPUT_FOCUS_LOST, LAP_INPUT_FOCUS_GAINED,
    LAP_INPUT_PAUSE, LAP_INPUT_RESUME, LAP_INPUT_RESIZE
};
enum { LAP_BUTTON_LEFT = 1, LAP_BUTTON_MIDDLE = 2, LAP_BUTTON_RIGHT = 4, LAP_BUTTON_X1 = 8, LAP_BUTTON_X2 = 16 };
typedef struct {
    uint16_t size, version;
    uint8_t kind, mods, button, buttons; /* button index: 0 left, 1 middle, 2 right */
    uint16_t key;
    int16_t x, y, wheel; /* client pixels; wheel positive down */
    uint16_t width, height; /* RESIZE client pixels */
    uint8_t clicks;
    char ch;
} lap_input_event_t;
typedef struct {
    uint16_t size, version;
    uint32_t keys[LAP_INPUT_KEY_WORDS];
    int16_t x, y;
    uint8_t buttons, mods;
    uint16_t reserved;
    uint32_t dropped;
} lap_input_state_t;
bool lap_key_down(unsigned key);
int lap_input_state(lap_input_state_t *out);
int lap_input_next(lap_input_event_t *out); /* 1 event, 0 empty, <0 error */
/* 64 events wait; overflow drops oldest and increments state.dropped.
   Held state always reflects the newest input. Focus loss/pause clears it. */

/* One asynchronous modal at a time. Inputs are copied, including strings/list.
   max=1..768 bytes excluding NUL; flags LOWER (0 keeps the default keyboard case). Poll returns 0 pending,
   1 accepted, 2 cancelled, <0 error; text needs room for result plus NUL.
   Prompt <=95 bytes; confirm message <=159; choose 1..48 items of <=31
   bytes. Confirm value: 1 yes, 0 no; choose value: zero-based index. Closing/hiding
   the app cancels its modal. No callbacks run on worker tasks. */
enum { LAP_TEXT_LOWER = 1 };
int lap_text_input(const char *prompt, const char *initial, int max, unsigned flags);
int lap_confirm(const char *message);
int lap_choose(const char *const *list, int count);
int lap_dialog_poll(int handle, char *text, size_t cap, int *value);
void lap_dialog_cancel(int handle);

/* App-local data, preserved across updates. Slot/key: 1..32 ASCII plain-name
   characters. Save envelope v1 carries payload length and checksum; schema
   is app-defined. lap_save uses schema 1, lap_load ignores schema.
   Saves <=64KiB; settings <=256 bytes. Inputs are copied before return.
   Writes return a job (>=0), polled via lap_sd_poll; completion means temp +
   recoverable rename finished on SD. While a flush is pending, cached-file
   mutation returns failure. A failed job preserves dirty cache for retry.
   FAT power loss is recoverable replacement, not a multi-file transaction. */
int lap_save(const char *slot, const void *data, size_t len);
int lap_load(const char *slot, void *data, size_t cap);
int lap_save_write(const char *slot, const void *data, size_t len, uint32_t schema);
int lap_save_read(const char *slot, void *data, size_t cap, uint32_t *schema);
int lap_settings_set(const char *key, const void *data, size_t len);
int lap_settings_get(const char *key, void *data, size_t cap);
int lap_file_flush(void); /* async job for all dirty cached files; keeps handles */
#endif /* LAP_API_23 */

/* ---- Radio RX API 2.4: discovery, managed DSP and typed decodes ---------- */
#if defined(LAP_API_24)
#define LAP_RADIO_RANGES_MAX 8
#define LAP_RADIO_GAINS_MAX 64
/* Controls 0..7 match the firmware front end. Antenna 0 is the sole RX port
   on current radios; other values return EUNSUPPORTED. No register access. */
enum { LAP_RX_CTL_BIAS_T, LAP_RX_CTL_PPM, LAP_RX_CTL_AGC, LAP_RX_CTL_OFFSET,
       LAP_RX_CTL_AMP, LAP_RX_CTL_LNA_DB, LAP_RX_CTL_VGA_DB, LAP_RX_CTL_DIRECT,
       LAP_RX_CTL_ANTENNA, LAP_RX_CTL_TUNE, LAP_RX_CTL_GAIN, LAP_RX_CTL_RATE,
       LAP_RX_CTLS };
typedef struct { uint64_t min_hz, max_hz; } lap_radio_range_t;
typedef struct {
    uint16_t size, version; /* returned size, version=1 */
    uint32_t controls;      /* 1 << LAP_RX_CTL_*; only actually supported controls */
    char id[32], owner[24];
    uint8_t present, busy, frequency_count, rate_count;
    uint16_t gain_count, reserved;
    lap_radio_range_t frequencies[LAP_RADIO_RANGES_MAX];
    lap_radio_range_t rates[LAP_RADIO_RANGES_MAX];
    int16_t gains[LAP_RADIO_GAINS_MAX]; /* tuner steps, tenths dB; not an envelope */
    int32_t values[LAP_RX_CTLS]; /* last successfully applied front-end values */
} lap_radio_info2_t;
/* Cached worker snapshot, no USB calls on the screen. 0 or LAP_RX_E*.
   Synchronous copied inputs/outputs accept the active callback stack.
   Call lap_caps first: radio_count/present may be zero on another board. */
int lap_radio_info2(int index, lap_radio_info2_t *out, size_t size);
enum { LAP_RX_COMMAND_QUEUED=1, LAP_RX_COMMAND_APPLIED, LAP_RX_COMMAND_FAILED };
typedef struct {
    uint16_t size, version;
    uint32_t id, state;
    int32_t control, result;
    int64_t requested, applied;
} lap_rx_command_t;
/* One outstanding command per session; positive ticket means QUEUED, never
   applied. Poll returns 0 and a terminal APPLIED/FAILED result, or EAGAIN.
   Last 8 completions retained; stale tickets return EINVAL. Hardware errors
   remain command results. Close/unload invalidates tickets. Tune is uint64
   Hz up to INT64_MAX; gain accepts LAP_GAIN_AUTO. Rate is samples/s.
   FE values: bias/offset 0|1, PPM -100..100 RTL/-200..200 HackRF, AGC
   0 follow/1 on/2 off, amp 0 auto/1 off/2 on, LNA -1 auto or 0..40 step 8,
   VGA -1 auto or 0..62 step 2, direct 0..2 only when supported. */
int lap_rx_control(int h, int control, int64_t value);
int lap_rx_control_get(int h, int control, int64_t *value);
int lap_rx_command_poll(int ticket, lap_rx_command_t *out, size_t size);

typedef struct {
    uint16_t size, version; /* sizeof, 1 */
    int32_t offset_hz;
    uint32_t output_rate_hz, bandwidth_hz;
} lap_channel_config_t;
typedef struct {
    uint16_t size, version;
    uint32_t epoch, flags;
    uint64_t first_sample, center_hz, arrival_us;
    uint32_t rate_hz, dropped_pairs;
} lap_iq_meta2_t;
typedef struct {
    uint16_t size, version;
    int32_t state;
    uint32_t output_rate_hz, queued_pairs, dropped_pairs, input_dropped_pairs;
} lap_channel_status_t;
/* One channel per session, prepared by the I/O worker. Integer decimation
   1..64; output >=8 kS/s. 127-tap Hann FIR, bandwidth <= output_rate/2;
   callers must allow the transition band. |offset|+bandwidth/2 < input/2.
   32 x 2048-pair input slots; 16K-pair output ring, drop newest, no waiting.
   No app callbacks on capture/DSP tasks. Read on the screen is cheap after
   decimation. Status reports OPENING/RUNNING/errors. Close is asynchronous.
   Arrival is host monotonic time, NOT a hardware sample clock; positions are
   nominal output samples. GAP/epoch changes reset filter alignment. */
int lap_rx_channel_open(int h, const lap_channel_config_t *cfg);
int lap_rx_channel_read(int channel, int16_t *iq, int max_pairs, lap_iq_meta2_t *meta, size_t size);
int lap_rx_channel_status(int channel, lap_channel_status_t *out, size_t size);
void lap_rx_channel_close(int channel);

enum { LAP_SPEC_RECT, LAP_SPEC_HANN, LAP_SPEC_BLACKMAN };
typedef struct {
    uint16_t size, version;
    uint16_t fft_size, window; /* 64,128,256,512,1024,2048; LAP_SPEC_* */
    uint16_t averages, lines_per_second; /* 1..64, 1..50 */
} lap_spectrum_config_t;
typedef struct {
    uint16_t size, version;
    uint32_t sequence, epoch;
    uint64_t center_hz, arrival_us;
    uint32_t span_hz;
    float bin_hz;
    int32_t gain_tenths_db; /* LAP_GAIN_AUTO when unknown/automatic */
    uint16_t fft_size, window;
    uint32_t input_dropped_pairs;
} lap_spectrum_meta_t;
/* Config copied/queued, 0 means accepted; next line carries the settings.
   Linear-power averages, coherent-window-gain normalization, shifted dBFS
   bins. Read returns configured FFT bins, 0 until a line, or E*. Small
   buffers return ERANGE without consuming. FFT * averages * lines/s must fit
   the input sample rate. Old lap_rx_spectrum is unchanged. */
int lap_rx_spectrum_config(int h, const lap_spectrum_config_t *cfg);
int lap_rx_spectrum_read(int h, float *db, int max_bins, lap_spectrum_meta_t *meta, size_t size);

enum { LAP_DEC_FM=1, LAP_DEC_AM, LAP_DEC_USB, LAP_DEC_LSB, LAP_DEC_CW,
       LAP_DEC_POCSAG, LAP_DEC_APRS, LAP_DEC_ACARS, LAP_DEC_AIS, LAP_DEC_ADSB,
       LAP_DEC_SAME, LAP_DEC_P25, LAP_DEC_FLEX, LAP_DEC_WFM };
typedef struct {
    uint16_t size, version;
    uint32_t supported; /* 1 << LAP_DEC_*; compiled session adapters */
    uint32_t lr125_supported; /* decoder mask also accepting LR2021 125 kS/s */
    uint32_t rate_hz[15]; /* preferred/fixed rate indexed by LAP_DEC_* */
} lap_decoder_catalog_t;
typedef struct {
    uint16_t size, version;
    uint32_t decoder;
    uint32_t flags; /* reserved: zero */
} lap_decoder_config_t;
typedef struct {
    uint16_t size, version;
    int32_t state;
    uint32_t decoder, events, dropped, input_dropped_pairs;
} lap_decoder_status_t;
/* Actual firmware decoder implementations, exclusive one per session.
   FM/AM/USB/LSB/CW/POCSAG/APRS/ACARS/AIS/SAME: 256 kS/s;
   FM/AM/POCSAG/FLEX/APRS/ACARS/SAME also accept LR2021 125 kS/s;
   ADS-B: exactly 2 MS/s. P25: 240 kS/s C4FM Phase 1 NID/TSBK/grants,
   no automatic tuning or P25 vocoder. WFM/FLEX: 256 kS/s. The catalog
   reports compiled adapters, including on a board currently without RF.
   Attachment is queued: inspect status before relying on it. PCM goes to
   lap_audio_read only after lap_audio_listen(true); no automatic speaker.
   Polling/legacy timeline subscriptions never activate a decoder. */
int lap_rx_decoder_catalog(lap_decoder_catalog_t *out, size_t size);
int lap_rx_decoder_attach(int h, const lap_decoder_config_t *cfg);
int lap_rx_decoder_status(int decoder, lap_decoder_status_t *out, size_t size);
void lap_rx_decoder_detach(int decoder);

enum { LAP_EVENT_LIVE=1, LAP_EVENT_SYNTHETIC=2, LAP_EVENT_POSITION_VALID=4,
       LAP_EVENT_CRC_OK=8 };
/* P25 support: 0 none, 1 Phase 1, 2 unsupported, 3 missing IDEN,
   4 invalid, 5 Phase 2. carrier_hz=0 means unresolved; opcode=255 is NID only.
   All fields/payloads fixed width; version=1. Protocol fields are values
   supplied by the decoder, never parsed out of a summary string. */
typedef struct {
    uint16_t size, version;
    uint32_t decoder, sequence, epoch, flags;
    int32_t radio;
    uint64_t center_hz, arrival_us;
    uint32_t payload_size, dropped;
} lap_event_header_t;
typedef struct { uint32_t address; uint16_t baud; uint8_t function, protocol; char text[160]; } lap_page_event_t;
typedef struct { char call[12], recipient[10], text[160]; int32_t lat_e7, lon_e7; uint32_t kind, fields; } lap_aprs_event_t;
typedef struct { char registration[8], label[3], text[221]; uint16_t bcs; uint8_t mode, block_id; } lap_acars_event_t;
typedef struct { uint32_t mmsi; int32_t lat_e7, lon_e7; uint16_t sog, cog, heading; uint8_t message, fields, channel, ship_type; char name[21], callsign[8]; } lap_ais_event_t;
/* ADS-B speed is knots, heading degrees; INT32_MIN means unavailable.
   non_icao preserves DF18 address qualification; frame carries wire bytes. */
typedef struct { uint32_t address; int32_t altitude_ft, speed, heading; uint8_t df, crc_ok, corrected, non_icao; char callsign[9]; uint8_t frame[14]; } lap_adsb_event_t;
typedef struct { uint16_t nac; uint8_t duid, corrected_bits; uint8_t opcode, mfid, reserved[2]; uint8_t block[12]; uint32_t talkgroup, source; uint64_t carrier_hz; uint16_t channel; uint8_t slot, slots_per_carrier, support, system_valid; uint16_t reserved2; uint32_t wacn, sysid; } lap_p25_event_t;
typedef struct { char originator[4], event[4], sender[9]; uint8_t location_count; uint16_t valid_minutes, day; uint8_t hour, minute, ended, test; uint32_t locations[31]; } lap_same_event_t;
/* Only events from this explicitly attached decoder. 32 entries; full drops
   newest. 1 copied, 0 empty, ERANGE leaves event queued, EINVAL stale handle.
   No implicit global timeline or replay. Synthetic fixtures are tagged.
   Detach/session close/unload discards all unread events. */
int lap_rx_decoder_next(int decoder, lap_event_header_t *header, size_t header_size, void *payload, size_t capacity);
#endif /* LAP_API_24: Radio RX */


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

/* API 2.4 — networking, Bluetooth and audio. All operations copy inputs before
   returning; outputs can be on the active callback stack. No app callbacks run
   on workers. Handles belong to one loaded app, are invalid after close/unload,
   and must be closed even after failure. Negative LAP_IO_E* values are errors.
   Query lap_caps first: missing hardware returns ENOTSUP. NETWORK is required
   for Wi-Fi, HTTP, UDP, mDNS and SNTP; BLE uses the existing BLE hardware lease.
   Fixed limits keep Nano internal RAM bounded; large queues live in PSRAM. */
#if defined(LAP_API_24)
/* Microphone access is declared in the existing one-byte flags field. */
#define LAP_CAP_MIC 16u
enum {
    LAP_IO_EINVAL = -100, LAP_IO_EBUSY = -101, LAP_IO_EACCES = -102,
    LAP_IO_ENOTSUP = -103, LAP_IO_EAGAIN = -104, LAP_IO_EIO = -105,
    LAP_IO_ELIMIT = -106, LAP_IO_ETIMEOUT = -107, LAP_IO_ECLOSED = -108,
    LAP_IO_ETLS = -109, LAP_IO_EAUTH = -110, LAP_IO_ECONFLICT = -111
};
enum { LAP_OPERATION_PENDING, LAP_OPERATION_READY, LAP_OPERATION_DONE };
typedef struct {
    uint16_t size, version;
    int32_t state, error;
    uint32_t transferred, dropped;
} lap_operation_t;
/* Poll Wi-Fi/time operations; close releases completed or pending operations.
   Four operations across Wi-Fi/time; scan returns up to 16 cached AP records. */
int lap_operation_status(int operation, lap_operation_t *out);
void lap_operation_close(int operation);
typedef struct {
    uint16_t size, version;
    uint8_t connected, ap_running, channel, reserved;
    int16_t rssi;
    uint16_t saved_count;
    char ssid[33], ip[16];
} lap_wifi_status_t;
/* Cached SSID/IP/RSSI, never credentials. Disconnected RSSI is -127. */
int lap_wifi_status(lap_wifi_status_t *out);
int lap_wifi_scan_start(void);
int lap_wifi_scan_result(int operation, int first, lap_wifi_ap_t *out, int max);
/* NULL password joins a firmware-saved SSID. A supplied password is copied,
   erased after the join and saved by firmware only on success. No getter. */
int lap_wifi_connect(const char *ssid, const char *password);
int lap_wifi_disconnect(void);
/* Start the firmware's own SoftAP identity/password; never discloses its key. */
int lap_wifi_ap_request(bool on);
enum { LAP_HTTP_GET, LAP_HTTP_POST, LAP_HTTP_PUT, LAP_HTTP_DELETE, LAP_HTTP_HEAD, LAP_HTTP_PATCH };
typedef struct {
    uint16_t size, version;
    uint8_t method, reserved[3];
    uint32_t timeout_ms, response_limit; /* 100..30000 ms; 1..1048576 bytes */
    char url[256]; /* http:// or https://; no credentials; no insecure TLS flag */
    char headers[512]; /* Name: value\r\n; no Host/length/transfer overrides */
    const void *body; /* copied, max 4096 bytes; NULL with body_size=0 */
    uint32_t body_size;
} lap_http_request_t;
typedef struct {
    uint16_t size, version;
    int32_t state, error, status_code;
    uint32_t received, queued;
    int32_t content_length; /* -1 unknown */
} lap_http_status_t;
/* Two requests. TLS always uses the firmware CA bundle and hostname checking.
   Redirects are refused (3xx status is delivered). Response headers <=2 KiB,
   8 KiB stream queue with backpressure; timeout covers the entire request. */
int lap_http_open(const lap_http_request_t *request);
int lap_http_status(int h, lap_http_status_t *out);
int lap_http_header(int h, const char *name, char *out, size_t cap);
/* Positive bytes, EAGAIN while pending, 0 only when stream fully drained.
   On terminal failure queued bytes drain before the error is returned. */
int lap_http_read(int h, void *out, size_t cap);
void lap_http_close(int h);
typedef struct {
    uint16_t size, version;
    uint16_t port, reserved;
    char ip[16]; /* numeric IPv4, empty local address = any */
} lap_socket_address_t;
/* Four datagram sockets, 4 packets per direction, max 1472 bytes each.
   Broadcast allowed. join/leave enables IPv4 multicast, TTL=1. recv leaves an
   oversized packet queued and returns ELIMIT. Empty datagrams return 0. */
int lap_udp_open(const lap_socket_address_t *local);
int lap_udp_send(int h, const lap_socket_address_t *to, const void *data, size_t len);
int lap_udp_recv(int h, lap_socket_address_t *from, void *data, size_t cap);
int lap_udp_join(int h, const char *group, bool on);
int lap_udp_status(int h, lap_operation_t *out);
void lap_udp_close(int h);
typedef struct {
    uint16_t size, version, port, reserved;
    uint32_t ttl;
    char instance[64], hostname[64], ip[16], txt[256];
} lap_mdns_service_t;
typedef struct {
    uint16_t size, version, port, reserved;
    char instance[64], service[32], protocol[8], txt[256]; /* TXT: key=value;key=value */
} lap_mdns_registration_t;
/* One browse and one registration per app, at most eight results. Browse is
   a bounded 2 second snapshot. next returns EAGAIN, 1 record, or 0 at end.
   Registration names are scoped to this lease; stop sends service goodbye. */
int lap_mdns_browse(const char *service, const char *protocol);
int lap_mdns_next(int browse, lap_mdns_service_t *out);
int lap_mdns_register(const lap_mdns_registration_t *service);
void lap_mdns_stop(int h);
typedef struct {
    uint16_t size, version;
    uint8_t synced, source, reserved[2]; /* 0 unknown, 1 RTC/local, 2 SNTP */
    uint32_t last_sync_ms, uncertainty_ms; /* UINT32_MAX if backend cannot estimate */
} lap_time_status_t;
int lap_time_status(lap_time_status_t *out);
int lap_time_sync_request(void);
#define LAP_BLE_AD_MAX 1650
/* Passive scan lease shared with LINK/HID/RID. Full AD TLVs include service
   UUIDs/manufacturer data; repeated reports preserve timestamped RSSI history.
   Queue depth 8. Overlong reports are dropped whole, never silently truncated. */
typedef struct {
    uint16_t size, version, payload_size;
    uint8_t addr[6], addr_type, scan_response;
    int16_t rssi;
    uint32_t at_ms;
} lap_ble_advert_t;
int lap_ble_scan_start(void);
int lap_ble_advert_next(int scan, lap_ble_advert_t *out, void *payload, size_t cap);
int lap_ble_scan_status(int scan, lap_operation_t *out);
void lap_ble_scan_stop(int scan);
typedef struct { uint8_t bytes[16], length, reserved[3]; } lap_uuid_t; /* little endian; length=2,4,16 */
enum { LAP_BLE_CONNECTED, LAP_BLE_DISCONNECTED, LAP_GATT_SERVICE,
       LAP_GATT_CHARACTERISTIC, LAP_GATT_DESCRIPTOR, LAP_GATT_VALUE,
       LAP_GATT_WRITTEN, LAP_GATT_NOTIFY, LAP_GATT_COMPLETE };
typedef struct {
    uint16_t size, version;
    int32_t connection, operation, error;
    uint16_t kind, attribute, end_handle, properties, payload_size, reserved;
    uint32_t dropped;
    lap_uuid_t uuid;
} lap_ble_event_t;
/* One additional GATT client; never borrows HID/head connections. 20-byte
   values (default MTU=23), 16 events, one procedure in flight. No new NimBLE
   host or task. Pairing-required errors are returned; bond keys stay private. */
int lap_ble_connect(const uint8_t addr[6], uint8_t addr_type);
/* Services when start=end=0; otherwise characteristics in the given range. */
int lap_gatt_discover(int connection, uint16_t start, uint16_t end);
/* Discover descriptors between a characteristic value and its end handle. */
int lap_gatt_descriptors(int connection, uint16_t start, uint16_t end);
int lap_gatt_read(int connection, uint16_t attribute);
int lap_gatt_write(int connection, uint16_t attribute, const void *data, size_t len);
/* CCCD must come from descriptor discovery; write request, completion event. */
int lap_gatt_subscribe(int connection, uint16_t cccd, bool on);
int lap_ble_event_next(lap_ble_event_t *out, void *data, size_t cap);
void lap_ble_disconnect(int connection);
enum { LAP_MIX_MUSIC, LAP_MIX_EFFECTS, LAP_MIX_VOICE, LAP_MIX_CHANNELS };
/* Opt-in mixer: three 2048-sample mono 16 kHz s16 channels. Legacy lap_tone
   and lap_audio_write keep their queues. Mixer yields to live radio/mute.
   Gain and duck are 0..100%; duck attenuates this channel while voice queues.
   write returns accepted sample count (0 if full). */
int lap_mixer_config(int channel, int volume, int duck);
int lap_mixer_write(int channel, const int16_t *pcm, int samples);
void lap_mixer_clear(int channel);
/* transferred = consumed samples; dropped = muted/live/media/busy speaker. */
int lap_mixer_status(int channel, lap_operation_t *out);
/* One capture lease, 16 kHz mono s16 PCM, 4096 samples, drop newest/count.
   Firmware displays MIC ON while active, including when the app is hidden.
   No mic on Nano variants without a fitted ADC. Close stops capture/record. */
int lap_mic_open(void);
int lap_mic_read(int h, int16_t *pcm, int max, lap_operation_t *status);
void lap_mic_close(int h);
/* Names are single plain basenames in this app's apps_data/<id>/ directory.
   WAV: PCM signed 16-bit, mono 16 kHz; RIFF chunks and odd padding supported.
   One sample stream, one recorder, max 30 seconds / 960000 PCM bytes. Worker
   owns SD handles. Record stop finalizes WAV; unload also finalizes recording.
   Audio status/close work for sample and record operations. */
int lap_sample_play(int channel, const char *name);
int lap_audio_record_start(int mic, const char *name, uint32_t max_ms);
/* Stop/finalize asynchronously; poll lap_audio_operation until DONE, then close. */
int lap_audio_record_stop(int h);
int lap_audio_operation(int h, lap_operation_t *out);
void lap_audio_close(int h);
#endif /* LAP_API_24: networking, Bluetooth and audio */

/* ---- API 2.5: system services (Y1, D2/U2, Y2, Y5) ---------------------- */
#if defined(LAP_API_25)
enum { LAP_SYS_EINVAL=-1, LAP_SYS_EBUSY=-2, LAP_SYS_EACCES=-3,
       LAP_SYS_ENOENT=-4, LAP_SYS_EIO=-5, LAP_SYS_EUNSUPPORTED=-6,
       LAP_SYS_ELIMIT=-7, LAP_SYS_ECANCEL=-8 };
/* Fixed, copied notice; at most two nonempty action labels. Data is opaque.
   Eight notices system-wide, one post/second/app; no pointers into app code.
   Notices survive unload until dismissed/activated (not reboot). Activation
   uses the normal launcher including signature/permission review. */
typedef struct {
    uint16_t size, version;
    char title[20], body[128], actions[2][16];
    uint16_t data_len, reserved;
    uint8_t data[128];
} lap_notification_t;
typedef struct {
    uint16_t size, version;
    uint32_t notification;
    int32_t action; /* -1=body tap, 0/1=action button */
    uint16_t data_len, reserved;
    uint8_t data[128];
} lap_launch_data_t;
int lap_notify_post(const lap_notification_t *notice);
int lap_notify_dismiss(int notification);
/* 1 consumes a launch, 0=ordinary launch/no new activation. Retry after enter
   too: an already resident app receives activation on its next tick/draw. */
int lap_launch_data(lap_launch_data_t *out);

enum { LAP_FILE_READ=1, LAP_FILE_WRITE=2, LAP_FILE_FOLDER=4 };
typedef struct { uint16_t size, version; uint32_t flags; char title[32]; } lap_file_picker_config_t;
typedef struct { uint16_t size, version; uint32_t bytes; uint8_t dir, reserved[3]; char name[64]; } lap_dir_entry_t;
/* Scope 0 is apps_data/<id>/files; positive scope handles are user-selected
   files/folders valid until app unload. Relative UTF-8 paths, max 127 bytes;
   no dot components, hidden files, backslashes or symlinks. Protected app,
   credentials and firmware folders can never be granted. READ mandatory;
   WRITE permits overwrite/creation. Four streams and four grants/instance.
   Each instance has one outstanding storage job, max 4096 bytes or 16 rows.
   Calls never perform filesystem I/O on the UI task. Open reserves a logical
   handle; existence/I/O errors arrive from read/write/list completion.
   Poll: 0=pending, 1=done (result is bytes/rows or negative error); completion
   consumes the job. Inputs are copied; output buffers are used only by poll.
   Reads/writes advance stream position on successful completion; seek and
   close return EBUSY while that stream has an outstanding job. Offset range
   is 0..INT32_MAX. Directory cursor is an entry offset, zero starts; next=0
   means end. Directory mutations can invalidate pagination. Close is logical;
   writes complete only after fflush/fsync/close (FAT power loss still matters).
   Cached lap_file_* data is separate from the streaming files/ subfolder. */
int lap_file_picker(const lap_file_picker_config_t *cfg);
/* 0=pending, positive=scope, negative=cancel/error; consumes completion. */
int lap_file_picker_poll(int picker);
int lap_file_scope_close(int scope);
int lap_stream_open(int scope, const char *path, uint32_t flags);
int lap_stream_read(int stream, uint32_t bytes);
int lap_stream_write(int stream, const void *data, uint32_t bytes);
int lap_stream_seek(int stream, uint64_t offset);
int lap_stream_close(int stream);
int lap_dir_list(int scope, const char *path, uint32_t cursor);
int lap_storage_poll(int job, void *out, uint32_t capacity, int32_t *outcome, uint32_t *next);
/* Best effort: queued work is skipped; a write already running may complete.
   Poll still consumes the cancelled job. Cancellation does not roll back. */
int lap_storage_cancel(int job);

/* Boot-local topic mailboxes: 8 app identities, 4 subscriptions/app, 8 messages
   each, 192-byte payload, 8 publishes/second/app. Topic: 1..31 ASCII letters,
   digits, dot, slash, underscore or dash. Type is an app-defined schema ID.
   Sender identity is supplied by firmware. Subscribe is idempotent. Queues
   and subscriptions survive unload so sequential apps can exchange messages
   with today's single resident service. No auto-launch. Full queues drop
   newest; publish returns delivered count; dropped is reported on next read.
   Unsubscribe clears matching queued messages. Topics are public, not secret.
   No additional resident service is enabled by this API. */
typedef struct {
    uint16_t size, version;
    uint32_t type, dropped;
    char sender[32], topic[32];
    uint16_t data_len, reserved;
    uint8_t data[192];
} lap_ipc_message_t;
int lap_ipc_subscribe(const char *topic);
int lap_ipc_unsubscribe(const char *topic);
int lap_ipc_publish(const char *topic, uint32_t type, const void *data, uint32_t len);
int lap_ipc_next(lap_ipc_message_t *out);

enum { LAP_DESKTOP_CONNECTED=1, LAP_DESKTOP_DISCONNECTED, LAP_DESKTOP_RESIZE,
       LAP_DESKTOP_SCALE, LAP_DESKTOP_FOCUS };
typedef struct {
    uint16_t size, version;
    uint32_t kind, dropped;
    int32_t width_px, height_px;
    uint16_t cell_width_px, cell_height_px;
    uint16_t scale_milli; /* 1000=1x host DPI; 0=viewer DPI unknown */
    uint8_t connected, focused;
} lap_desktop_event_t;
/* Snapshot returns 0; next returns 1/event, 0/empty. 16 events/instance, oldest
   dropped on overflow. Connection covers attached serial/web/network viewers;
   protocol does not identify the Desktop brand. Negotiated host DPI is in
   scale_milli (0 on older viewers). Size is
   own client pixels; scale events reflect firmware font/cell changes. */
int lap_desktop_info(lap_desktop_event_t *out);
int lap_desktop_next(lap_desktop_event_t *out);
/* API 2.6 viewers negotiate URL/save support. Returns positive request ID;
   poll/cancel with lap_desktop_request_status/cancel. Older viewers return
   EUNSUPPORTED. HTTPS only; viewer displays the URL before opening. Save reads
   a scoped file on the worker (max 65536 bytes), then asks the PC save dialog. */
int lap_desktop_open_url(const char *url);
int lap_desktop_save_file(int scope, const char *path, const char *suggested_name);
#endif /* LAP_API_25: system services */
#if defined(LAP_API_26)
/* ---- API 2.6: HackRF TX / desktop host -------------------------------- */
typedef struct {
    uint16_t size, version; /* sizeof, 1 */
    uint8_t vga_db, amp, reserved[2]; /* VGA 0..47 dB, amp 0/1; not calibrated dBm */
} lap_hackrf_tx_config_t;
/* Copies settings for subsequent lap_tx_iq/replay on this HackRF. Requires
   RADIO_TX, idle TX queue; defaults VGA=0/amp=off on app load. Frequency and
   sample rate come from the existing TX calls (2..20 MS/s, signed I8/Q8).
   Owner firmware settings cap VGA/amp; duty/thermal/RX ownership still apply.
   power_dbm remains a conservative hardware ceiling, not an output calibration. */
int lap_tx_hackrf_config(int radio, const lap_hackrf_tx_config_t *cfg);
/* Features: bit 0 URL, bit 1 save; absent/older viewers return 0. */
uint32_t lap_desktop_features(void);
/* 0 pending, 1 success, negative LAP_SYS_E* failure. Terminal results consumed.
   120s timeout, disconnect/unload cancel. Only the requesting app can poll. */
int lap_desktop_request_status(int request);
int lap_desktop_request_cancel(int request);
#endif /* API 2.6: HackRF TX / desktop host */
#if defined(LAP_API_25)
/* ---- API 2.5: input devices and sensors ------------------------------- */
enum {
    LAP_HW_HID_BLE = 1u << 13, LAP_HW_GAMEPAD_BLE = 1u << 14,
    LAP_HW_GAMEPAD_USB = 1u << 15, LAP_HW_MULTITOUCH = 1u << 16,
    LAP_HW_NAVIGATION = 1u << 17, LAP_HW_GAMEPAD_DESKTOP = 1u << 18
};
enum { LAP_INPUT_DEVICE_CONNECTED = 12, LAP_INPUT_DEVICE_DISCONNECTED,
       LAP_INPUT_GAMEPAD, LAP_INPUT_CONTACT_DOWN, LAP_INPUT_CONTACT_MOVE, LAP_INPUT_CONTACT_UP };
enum { LAP_DEVICE_KEYBOARD = 1, LAP_DEVICE_MOUSE = 2, LAP_DEVICE_GAMEPAD = 4,
       LAP_DEVICE_TOUCH = 8 };
enum { LAP_TRANSPORT_LOCAL, LAP_TRANSPORT_DESKTOP, LAP_TRANSPORT_BLE, LAP_TRANSPORT_USB };
#define LAP_INPUT_DEVICES_MAX 8
#define LAP_CONTACTS_MAX 10
typedef struct {
    uint16_t size, version; /* version 1; firmware fills all new output structs */
    uint32_t id; /* boot-local identity, never a Bluetooth address; zero reserved */
    uint8_t classes, transport, connected, reserved;
    char name[32];
} lap_input_device_t;
typedef struct {
    lap_input_event_t event; /* unchanged 2.3 prefix; size is sizeof this struct, version 2 */
    uint32_t device;
    uint16_t contact; /* controller/desktop ID; stable for the contact lifetime */
    uint8_t classes, transport;
    int16_t axes[6]; /* LX,LY,RX,RY: -32767..32767; LT,RT: 0..32767; +Y down */
    uint32_t gamepad_buttons; /* HID button usages 1..32 map to bits 0..31 */
    int8_t dpad_x, dpad_y; /* -1..1; right/down positive */
    uint16_t reserved;
} lap_input_event2_t;
typedef struct {
    uint16_t id;
    int16_t x, y; /* client pixels */
    uint16_t reserved;
} lap_input_contact_t;
typedef struct {
    uint16_t size, version;
    uint32_t device;
    lap_input_state_t input;
    int16_t axes[6];
    uint32_t gamepad_buttons;
    int8_t dpad_x, dpad_y;
    uint8_t connected, contact_count;
    lap_input_contact_t contacts[LAP_CONTACTS_MAX];
} lap_input_device_state_t;
/* Same focused queue as lap_input_next; choose one reader. Old reader skips
   new kinds. Overflow counted by lap_input_state; recover from device_state.
   Focus loss, modal entry, disconnect and queue reset release held state.
   Lists never scan/pair. BLE pairing remains in LINK. All buffers synchronous.
   USB gamepads are unavailable until a USB HID host is implemented. */
int lap_input_next2(lap_input_event2_t *out, size_t size);
int lap_input_devices(lap_input_device_t *out, int max);
int lap_input_device_state(uint32_t device, lap_input_device_state_t *out, size_t size);

enum { LAP_GPS_ABSENT, LAP_GPS_OFF, LAP_GPS_ACQUIRING, LAP_GPS_FIX, LAP_GPS_STALE };
typedef struct {
    uint16_t size, version;
    uint8_t state, quality, antenna, leased; /* antenna: 0 unknown,1 OK,2 open,3 short */
    uint32_t sentence_age_ms, fix_age_ms, sky_age_ms; /* UINT32_MAX when never seen */
    lap_gps_t position; /* coordinates may be last known; check state and age */
    uint8_t tracked, reserved[3];
    int32_t error; /* asynchronous receiver start error, 0 when none */
} lap_gps_status_t;
typedef struct {
    uint16_t size, version;
    uint16_t prn, azimuth;
    uint8_t constellation, elevation, snr, used; /* NMEA system ID; dB-Hz */
    uint32_t age_ms;
} lap_gps_satellite_t;
/* One app acquisition lease; flags must be zero. Returns positive lease or
   LAP_RX_E*. Repeated acquire returns the same lease. Worker starts/feeds the
   UART; status reports pending/off/acquiring/fix/error. Release/unload drops
   only this app's demand, never firmware demand. No reset/configuration access. */
int lap_gps_acquire(uint32_t flags);
void lap_gps_release(int lease);
int lap_gps_status(lap_gps_status_t *out);
int lap_gps_satellites(lap_gps_satellite_t *out, int max); /* up to 48; stale sky omitted */

enum { LAP_MOTION_ACCEL = 1, LAP_MOTION_GYRO = 2, LAP_MOTION_MAG = 4,
       LAP_MOTION_HEADING = 8, LAP_MOTION_RAW = 16 };
enum { LAP_MOTION_CALIBRATED = 1, LAP_MOTION_DISTURBED = 2,
       LAP_MOTION_LOW_CONFIDENCE = 4, LAP_MOTION_TRUE_NORTH = 8 };
typedef struct {
    uint16_t size, version;
    uint32_t available, subscribed, period_ms, age_ms, sequence;
    uint32_t quality; /* LAP_MOTION_* flags above */
    float heading_sigma_deg; /* learner's 1-sigma, -1 when unknown */
} lap_motion_status_t;
typedef struct {
    uint16_t size, version;
    uint32_t sequence, fields, quality, age_ms;
    lap_motion_t motion; /* existing calibrated screen-axis view */
    float raw_accel[3], raw_gyro[3], raw_mag[3]; /* uncalibrated board axes, g/deg s/uT */
    float heading_sigma_deg; /* -1 unknown; do not infer accuracy from validity */
} lap_motion_sample_t;
/* Latest-only delivery, no hardware callbacks. period 50..10000 ms (20 Hz
   maximum), fields nonzero subset above. No duplicate until the next period;
   slow readers skip samples. RAW includes available accel/gyro/mag, never ADC
   counts. Invalid mag omitted. Subscribe(0,0) stops delivery. Unload cleans up. */
int lap_motion_subscribe(uint32_t fields, uint32_t period_ms);
int lap_motion_status(lap_motion_status_t *out);
int lap_motion_next(lap_motion_sample_t *out); /* 1 sample,0 not due,<0 error */

enum { LAP_NAV_TRACK = 1, LAP_NAV_WAYPOINT = 2, LAP_NAV_PLACE = 3 };
typedef struct {
    uint16_t size, version;
    uint32_t id, time;
    double lat, lon;
    float altitude_m;
    uint8_t kind, flags; /* bit 0 track time is Unix seconds; bit 1 place is tile-cover centre; bit 2 queued place */
    uint16_t reserved;
    char name[64];
} lap_navigation_point_t;
/* Read-only, bounded snapshot of the firmware track ring (4096 points), saved
   map waypoints (48), or installed place library (32). One request at a time;
   open returns positive handle, read returns LAP_RX_EAGAIN while worker reads
   SD, then page count (0 at end). All snapshots are immutable until close.
   Waypoint IDs are snapshot-local; place IDs are the saved library keys.
   No arbitrary path, no recording/marker mutation. Close/unload abandons work.
   CartoCore tile/feature rendering is not exported: see INPUT_SENSORS.md. */
int lap_navigation_open(int kind);
int lap_navigation_read(int handle, uint32_t first, lap_navigation_point_t *out, int max);
void lap_navigation_close(int handle);
#endif /* LAP_API_25: input devices and sensors */

#ifdef LAP_API_26
/* API 2.6: Bluetooth peripheral roles (B3/B4).
   One exclusive BLE peripheral lease. LINK, HID host, observers and SDK central
   leases return EBUSY while owned; existing owners are never stopped. Optional
   builds expose these symbols but omit the lap_caps hardware bits and return
   ENOTSUP. Starts return a positive handle; poll status for worker completion.
   All input is copied synchronously (callback stack allowed), no app callbacks
   run on NimBLE. Close/unload stops advertising, disconnects and drops the DB.
   Emission requires the declared LAP_CAP_RADIO_TX; HID also declares HID_DEVICE.
   No region, licence or confirmation gate. See docs/BLE_PERIPHERAL.md. */
#define LAP_CAP_HID_DEVICE 32u
#define LAP_BLE_SERVER_ATTRS 16
#define LAP_BLE_VALUE_MAX 128
#define LAP_BLE_REPORT_MAX 20
#define LAP_BLE_ADV_MAX 31
/* Independent build features; BLE alone does not imply peripheral support. */
enum { LAP_HW_BLE_ADVERTISER = 1u << 19, LAP_HW_GATT_SERVER = 1u << 20,
       LAP_HW_HID_DEVICE = 1u << 21 };
typedef struct {
    uint16_t size, version; /* sizeof, 1 */
    int32_t server; /* 0 creates beacon session; server handle attaches advertising */
    uint16_t interval_min_ms, interval_max_ms; /* 20..10240, min<=max */
    int8_t tx_power_dbm; /* -24..20 (C5/C6 PA); actual controller power in status */
    uint8_t connectable, data_len, scan_response_len;
    uint8_t data[31], scan_response[31];
} lap_ble_advert_config_t;
/* Legacy AD TLVs (length,type,value): name 0x09, manufacturer 0xff, UUID lists
   0x03/0x07, service data 0x16/0x21. Both 31-byte fields validated; no silent
   truncation. A connectable advert requires a ready GATT server. Reconfigure
   existing server by setting server=h; beacon stop also closes its session. */
int lap_ble_advertise(const lap_ble_advert_config_t *cfg);
void lap_ble_advertise_stop(int handle);
enum { LAP_GATTS_SERVICE = 1, LAP_GATTS_CHARACTERISTIC, LAP_GATTS_DESCRIPTOR };
enum { LAP_GATTS_READ = 1u, LAP_GATTS_WRITE = 2u, LAP_GATTS_WRITE_NR = 4u,
       LAP_GATTS_NOTIFY = 8u, LAP_GATTS_INDICATE = 16u,
       LAP_GATTS_ENCRYPT = 32u, LAP_GATTS_AUTHENTICATE = 64u };
typedef struct {
    uint16_t size, version; /* sizeof, 1 */
    uint16_t id, parent; /* app IDs, nonzero unique; preceding service/char parent */
    uint8_t kind, reserved;
    uint16_t flags, value_len;
    lap_uuid_t uuid;
    uint8_t value[LAP_BLE_VALUE_MAX];
} lap_gatt_attribute_t;
/* Flat ordered tree: service, its characteristics, each followed by descriptors.
   16 entries total, UUID length 2/4/16, values <=128 bytes. CCCD 0x2902 is
   automatic for notify/indicate; do not declare it. 0x1800/0x1801 are reserved.
   READ returns the current copied value immediately and queues a READ event;
   WRITE validates permissions/length, queues an event and updates the value.
   App handles reads by refreshing values before the next ATT read. There is no
   deferred ATT reply or app-dependent write veto. Overflow rejects ATT access;
   no screen-task waits or multi-attribute app transaction. */
int lap_gatt_server_open(const lap_gatt_attribute_t *attributes, int count);
int lap_gatt_server_set(int server, uint16_t id, const void *data, size_t len);
/* One peer, notifications/indications <=20 bytes independent of negotiated MTU.
   Requires subscription; returns EAGAIN before connected/subscribed, EBUSY if
   queue/indication in flight. Accepted data also updates the cached value.
   0 means queued; SENT reports host submission/indication acknowledgement. */
int lap_gatt_notify(int server, uint16_t id, const void *data, size_t len, bool indicate);
enum { LAP_PERIPHERAL_READY = 1, LAP_PERIPHERAL_CONNECTED, LAP_PERIPHERAL_DISCONNECTED,
       LAP_PERIPHERAL_MTU, LAP_PERIPHERAL_BOND, LAP_PERIPHERAL_SUBSCRIBE,
       LAP_PERIPHERAL_READ, LAP_PERIPHERAL_WRITE, LAP_PERIPHERAL_SENT,
       LAP_PERIPHERAL_ADVERTISING };
typedef struct {
    uint16_t size, version;
    uint16_t kind, id; /* app attribute ID; never a NimBLE pointer/handle */
    int32_t error;
    uint16_t payload_size, value; /* MTU; subscription bits notify=1,indicate=2; bond=1,encrypted=2,authenticated=4 */
    uint32_t dropped;
} lap_gatt_server_event_t;
typedef struct {
    uint16_t size, version;
    int32_t state, error; /* LAP_OPERATION_PENDING/READY/DONE */
    uint32_t dropped;
    uint16_t mtu;
    uint8_t advertising, connected, bonded, encrypted, authenticated;
    int8_t tx_power_dbm;
} lap_ble_peripheral_status_t;
int lap_ble_peripheral_status(int handle, lap_ble_peripheral_status_t *out);
/* 1 event, EAGAIN empty, ELIMIT leaves event queued when cap is too small. */
int lap_gatt_server_next(int server, lap_gatt_server_event_t *out, void *data, size_t cap);
void lap_gatt_server_close(int server); /* also closes beacon and HID sessions */
enum { LAP_HID_KEYBOARD = 1, LAP_HID_MOUSE, LAP_HID_GAMEPAD };
/* Built-in HOGP maps, report ID 1 (ID is NOT included in report payload):
   keyboard=8 bytes: modifiers,reserved,6 USB key usages; mouse=4 bytes:
   buttons(3 bits),signed x,y,wheel; gamepad=6 bytes: LE buttons16,signed x,y,z,rz.
   Firmware owns bonding keys; Just Works encryption, no MITM claim or app key
   export. Open builds HID service and connectable advertising (100ms, 0dBm).
   Focus loss/hide queues a neutral report, so held controls release.
   Peer output reports (e.g. keyboard LEDs) arrive as WRITE events. */
int lap_hid_device_open(int kind, const char *name); /* UTF-8 name <=24 bytes */
int lap_hid_device_send(int handle, const void *report, size_t len);
void lap_hid_device_close(int handle);
/* Copy the exact standard report map; returns byte count or ELIMIT/ENOTSUP. */
int lap_hid_device_report_map(int kind, void *out, size_t cap);
#endif /* LAP_API_26: Bluetooth peripheral roles */

/* ---- API 2.6: services and worker DSP ----------------------------------- */
#if defined(LAP_API_26)
typedef struct {
    uint16_t size, version; /* sizeof, 1 */
    char id[32], name[16];
    uint32_t shown, period_ms, memory_bytes, memory_limit, tick_budget_us;
    uint32_t pool_bytes, pool_limit, handles, handle_limit;
    lap_service_stats_t stats;
} lap_service_info_t;
/* Resident snapshots; list returns rows copied, query 1 found / 0 absent.
   The build selects 2 services on Nano, 3 on P4 by default, plus a foreground
   app. Each app retains its 4 MiB heap/cache cap and bounded private handle
   tables: TCP 6, HTTP 2, UDP 4, mDNS 2, BLE scan/client 1 each, mic 1,
   mixer 3, WAV 2, IQ/channel/decoder 1 each, packet/scan/TX/record 1 each,
   cached files 4, streams/scopes/storage jobs 4 each, IPC subscriptions 4.
   memory_bytes includes tracked heap/cache. pool_bytes accounts firmware
   handle tables, copied queues and live radio/audio buffers separately
   (2 MiB ceiling per slot); handles reports currently published handles.
   Exclusive hardware returns busy; opening a screen never steals a lease.
   No service auto-starts at boot. Stop residents from SETTINGS > SERVICES. */
int lap_service_list(lap_service_info_t *out, int max);
int lap_service_query(const char *app_id, lap_service_info_t *out);
int lap_service_limit(void);

#define LAP_DSP_OUTPUT_MAX 192u
enum { LAP_DSP_RUNNING=1, LAP_DSP_STOPPED=2, LAP_DSP_OVERRUN=3, LAP_DSP_FAILED=4 };
/* Pure computation on a PRIVATE WORKER task, never the screen/capture task.
   No SDK calls, blocking, locks, heap allocation or firmware access here.
   Return output bytes (0..cap), negative to discard. Input and output are
   borrowed for this block. State must be image/tracked allocation storage;
   only the worker may mutate it while attached. An overrun terminates the
   worker and disables this hook: state may be partial and must be reset.
   Budget is conservative elapsed time, including scheduling, rounded up to
   a scheduler tick on device; no callback may run past that deadline.
   Native machine code is trusted; this is scheduling isolation, not a sandbox. */
typedef int (*lap_dsp_process_t)(const int16_t *iq, uint32_t pairs,
    const lap_iq_meta2_t *meta, void *state, void *output, uint32_t cap);
typedef struct {
    uint16_t size, version; /* sizeof, 1 */
    uint32_t budget_us; /* 1000..10000; default 2000 */
    lap_dsp_process_t process;
    void *state;
    uint32_t state_bytes; /* 0 permits NULL */
} lap_dsp_config_t;
typedef struct {
    uint16_t size, version;
    uint32_t state, blocks, dropped, overruns, last_us, worst_us, queued;
    int32_t error; /* asynchronous admission failure, e.g. LAP_RX_ENOMEM */
} lap_dsp_status_t;
typedef struct {
    uint16_t size, version;
    uint32_t bytes, dropped;
    lap_iq_meta2_t iq;
} lap_dsp_result_t;
/* One hook per instance, on its own channel. Two copied input blocks and
   eight output messages; overflow drops newest and increments dropped.
   Channel reading remains available independently. Start returns a positive
   handle; errors use LAP_RX_*. next returns 1 result / 0 empty; too-small
   output returns ERANGE without consuming. status/next copy to callback
   stack or owned buffers. Close is async: retain state until STOPPED,
   OVERRUN or FAILED is reported. Channel close/unload cancels and joins the worker
   before image/state memory is freed. Stale/foreign handles are rejected. */
int lap_rx_dsp_start(int channel, const lap_dsp_config_t *config);
int lap_rx_dsp_status(int worker, lap_dsp_status_t *out);
int lap_rx_dsp_next(int worker, lap_dsp_result_t *out, void *data, uint32_t cap);
void lap_rx_dsp_close(int worker);
#endif /* API 2.6 services and worker DSP */
#ifdef __cplusplus
}
#endif
#endif /* LAKESHARK_APP_H */

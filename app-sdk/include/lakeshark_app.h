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

/* An app built against major M runs on firmware whose major is M and whose
   minor is at least the app's minor. */
#define LAP_API_MAJOR 1
#define LAP_API_MINOR 2 /* lap3d.h: firmware dot engine is available in API 1.2. */
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
    uint8_t flags;              /* reserved, 0 */
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

#include "ls_lora_port.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ls_board.h"
#include "settings.h"

#define PORT_VERSION 1
#define GPIO_MAX 54                 /* ESP32-P4 GPIO0..GPIO54 */
#define LR_DIO_MIN 5
#define LR_DIO_MAX 11
#define RFSW_TX_HF 0x10             /* SetDioRfSwitchConfig bit 4 */

/* ------------------------------------------------------------ the board -- */

static const int8_t TCXO_DV[8] = { 16, 17, 18, 22, 24, 27, 30, 33 };

int ls_lora_port_tcxo_dv(uint8_t code) { return code < 8 ? TCXO_DV[code] : -1; }
int ls_lora_port_tcxo_code(int dv)
{
    for (int i = 0; i < 8; i++) if (TCXO_DV[i] == dv) return i;
    return -1;
}

void ls_lora_port_board(ls_lora_port_t *out)
{
    memset(out, 0, sizeof(*out));
    out->header = false;
#ifdef LS_BOARD_LORA_CS_GPIO
    out->cs = LS_BOARD_LORA_CS_GPIO;
#else
    out->cs = LS_LORA_PORT_NONE;
#endif
#ifdef LS_BOARD_LORA_BUSY_GPIO
    out->busy = LS_BOARD_LORA_BUSY_GPIO;
#else
    out->busy = LS_LORA_PORT_NONE;
#endif
    out->rst = LS_LORA_PORT_RST_BOARD;
    out->irq_gpio = LS_LORA_PORT_NONE;
#ifdef LS_BOARD_LR20XX_IRQ_DIO
    out->irq_dio = LS_BOARD_LR20XX_IRQ_DIO;
#else
    out->irq_dio = LS_LORA_PORT_NONE;
#endif
    out->tcxo_code = 7;             /* 3.3 V, as the board's module wants */
#ifdef LS_BOARD_LR20XX_RFSW_TABLE
    static const struct { uint8_t dio, config; } rfsw[] = { LS_BOARD_LR20XX_RFSW_TABLE };
    for (size_t i = 0; i < sizeof(rfsw) / sizeof(rfsw[0]) && i < LS_LORA_PORT_RFSW_MAX; i++) {
        out->rfsw[i].dio = rfsw[i].dio;
        out->rfsw[i].config = rfsw[i].config;
        out->rfsw_n = (uint8_t)(i + 1);
    }
#endif
}

/* GPIOs the board itself drives, which a header radio cannot have. The
   radio bus (SCLK/MOSI/MISO) is shared with the header module, not taken by
   it, and stays listed: a chip select cannot be one of those lines. */
static const int BOARD_PINS[] = {
#ifdef LS_BOARD_I2C_SDA_GPIO
    LS_BOARD_I2C_SDA_GPIO, LS_BOARD_I2C_SCL_GPIO,
#endif
#ifdef LS_BOARD_I2C2_SDA_GPIO
    LS_BOARD_I2C2_SDA_GPIO, LS_BOARD_I2C2_SCL_GPIO,
#endif
#ifdef LS_BOARD_I2S_MCLK_GPIO
    LS_BOARD_I2S_MCLK_GPIO, LS_BOARD_I2S_BCK_GPIO, LS_BOARD_I2S_WS_GPIO,
    LS_BOARD_I2S_DOUT_GPIO, LS_BOARD_I2S_DIN_GPIO,
#endif
#ifdef LS_BOARD_SDIO_CLK_GPIO
    LS_BOARD_SDIO_CLK_GPIO, LS_BOARD_SDIO_CMD_GPIO, LS_BOARD_SDIO_D0_GPIO,
    LS_BOARD_SDIO_D1_GPIO, LS_BOARD_SDIO_D2_GPIO, LS_BOARD_SDIO_D3_GPIO,
#endif
#ifdef LS_BOARD_LCD_BL_GPIO
    LS_BOARD_LCD_BL_GPIO,
#endif
#ifdef LS_BOARD_SPI_SCLK_GPIO
    LS_BOARD_SPI_SCLK_GPIO, LS_BOARD_SPI_MOSI_GPIO, LS_BOARD_SPI_MISO_GPIO,
#endif
#ifdef LS_BOARD_GPS_RX_GPIO
    LS_BOARD_GPS_RX_GPIO, LS_BOARD_GPS_TX_GPIO,
#endif
    -1,
};

/* What the keyboard expansion and its MixRF radios use on the header. */
static const int KEYBOARD_PINS[] = {
#ifdef LS_BOARD_KEYBOARD_I2C_SDA_GPIO
    LS_BOARD_KEYBOARD_I2C_SDA_GPIO, LS_BOARD_KEYBOARD_I2C_SCL_GPIO,
#endif
#ifdef LS_BOARD_KEYPAD_INT_GPIO
    LS_BOARD_KEYPAD_INT_GPIO,
#endif
#ifdef LS_BOARD_KEYPAD_BL_GPIO
    LS_BOARD_KEYPAD_BL_GPIO,
#endif
#ifdef LS_BOARD_MIX_CC_CS
    LS_BOARD_MIX_CC_CS, LS_BOARD_MIX_CC_GDO0, LS_BOARD_MIX_NRF_CS, LS_BOARD_MIX_NRF_CE,
    LS_BOARD_MIX_NFC_CS,
#endif
    -1,
};

static bool listed(const int *list, int pin)
{
    for (int i = 0; list[i] >= 0; i++) if (list[i] == pin) return true;
    return false;
}

uint64_t ls_lora_port_keyboard_pins(const ls_lora_port_t *p)
{
    if (!p || !p->header) return 0;
    const int pins[] = { p->cs, p->busy, p->rst, p->irq_gpio };
    uint64_t mask = 0;
    for (size_t i = 0; i < sizeof(pins) / sizeof(pins[0]); i++)
        if (pins[i] >= 0 && listed(KEYBOARD_PINS, pins[i])) mask |= 1ull << pins[i];
    return mask;
}

/* ------------------------------------------------------------- checking -- */

bool ls_lora_port_check(const ls_lora_port_t *p, char *why, size_t n)
{
    if (!p) { snprintf(why, n, "no port"); return false; }
    const struct { const char *name; int pin; bool need; } pins[] = {
        { "cs", p->cs, true }, { "busy", p->busy, true },
        { "rst", p->rst, false }, { "irq", p->irq_gpio, false },
    };
    ls_lora_port_t board;
    ls_lora_port_board(&board);
    for (size_t i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
        const int g = pins[i].pin;
        if (g == LS_LORA_PORT_NONE && !pins[i].need) continue;
        if (g == LS_LORA_PORT_RST_BOARD && i == 2) continue;
        if (g < 0 || g > GPIO_MAX) { snprintf(why, n, "%s: GPIO 0-%d", pins[i].name, GPIO_MAX); return false; }
        /* The board's own socket lines are allowed: header mode pointed at
           them is how it is tested without another module. */
        const bool socket = (i == 0 && g == board.cs) || (i == 1 && g == board.busy);
        if (!socket && listed(BOARD_PINS, g)) {
            snprintf(why, n, "%s: GPIO %d is the board's own (bus, display, audio, SD or GPS)", pins[i].name, g);
            return false;
        }
        for (size_t j = 0; j < i; j++)
            if (pins[j].pin == g) { snprintf(why, n, "%s and %s are both GPIO %d", pins[j].name, pins[i].name, g); return false; }
    }
    if (p->rst == LS_LORA_PORT_RST_BOARD && p->header && p->cs != board.cs) {
        snprintf(why, n, "rst=board resets the board's socket, not a header module: give it a GPIO");
        return false;
    }
    if (p->irq_dio != LS_LORA_PORT_NONE && (p->irq_dio < LR_DIO_MIN || p->irq_dio > LR_DIO_MAX)) {
        snprintf(why, n, "dio: DIO%d-%d", LR_DIO_MIN, LR_DIO_MAX);
        return false;
    }
    if (p->tcxo_code > 7) { snprintf(why, n, "tcxo: code 0-7"); return false; }
    if (p->rfsw_n > LS_LORA_PORT_RFSW_MAX) { snprintf(why, n, "rfsw: at most %d entries", LS_LORA_PORT_RFSW_MAX); return false; }
    for (int i = 0; i < p->rfsw_n; i++) {
        const int d = p->rfsw[i].dio;
        if (d < LR_DIO_MIN || d > LR_DIO_MAX) { snprintf(why, n, "rfsw: DIO%d-%d", LR_DIO_MIN, LR_DIO_MAX); return false; }
        if (d == p->irq_dio) { snprintf(why, n, "rfsw: DIO%d is the interrupt", d); return false; }
        if (p->rfsw[i].config & ~0x1Fu) { snprintf(why, n, "rfsw: config is 5 bits (00-1F)"); return false; }
        /* the driver never transmits on the HF path */
        if (p->rfsw[i].config & RFSW_TX_HF) { snprintf(why, n, "rfsw: DIO%d sets the HF transmit state, which is refused", d); return false; }
        for (int j = 0; j < i; j++)
            if (p->rfsw[j].dio == d) { snprintf(why, n, "rfsw: DIO%d twice", d); return false; }
    }
    return true;
}

/* -------------------------------------------------------------- parsing -- */

static bool num(const char *s, long lo, long hi, long *out)
{
    char *end = NULL;
    const long v = strtol(s, &end, 10);
    if (end == s || *end || v < lo || v > hi) return false;
    *out = v;
    return true;
}

bool ls_lora_port_parse(int argc, char **argv, const ls_lora_port_t *base,
                        ls_lora_port_t *out, char *why, size_t n)
{
    if (argc < 1) { snprintf(why, n, "onboard | header cs=N busy=N rst=N ..."); return false; }
    if (!strcmp(argv[0], "onboard")) {
        if (argc != 1) { snprintf(why, n, "onboard takes nothing more"); return false; }
        ls_lora_port_board(out);
        return true;
    }
    if (strcmp(argv[0], "header")) { snprintf(why, n, "onboard | header"); return false; }
    *out = *base;
    out->header = true;
    for (int i = 1; i < argc; i++) {
        char key[8];
        const char *eq = strchr(argv[i], '=');
        if (!eq || eq == argv[i] || (size_t)(eq - argv[i]) >= sizeof(key)) { snprintf(why, n, "'%s': want key=value", argv[i]); return false; }
        memcpy(key, argv[i], (size_t)(eq - argv[i]));
        key[eq - argv[i]] = 0;
        const char *v = eq + 1;
        long x = 0;
        if (!strcmp(key, "cs") && num(v, 0, GPIO_MAX, &x)) out->cs = (int8_t)x;
        else if (!strcmp(key, "busy") && num(v, 0, GPIO_MAX, &x)) out->busy = (int8_t)x;
        else if (!strcmp(key, "rst") && !strcmp(v, "board")) out->rst = LS_LORA_PORT_RST_BOARD;
        else if (!strcmp(key, "rst") && num(v, 0, GPIO_MAX, &x)) out->rst = (int8_t)x;
        else if (!strcmp(key, "irq") && !strcmp(v, "none")) out->irq_gpio = LS_LORA_PORT_NONE;
        else if (!strcmp(key, "irq") && num(v, 0, GPIO_MAX, &x)) out->irq_gpio = (int8_t)x;
        else if (!strcmp(key, "dio") && !strcmp(v, "none")) out->irq_dio = LS_LORA_PORT_NONE;
        else if (!strcmp(key, "dio") && num(v, LR_DIO_MIN, LR_DIO_MAX, &x)) out->irq_dio = (int8_t)x;
        else if (!strcmp(key, "tcxo")) {
            /* "1.8" or "18": tenths of a volt */
            const char *dot = strchr(v, '.');
            long dv = -1;
            if (dot && dot[1] && !dot[2] && dot - v == 1 && v[0] >= '0' && v[0] <= '9' && dot[1] >= '0' && dot[1] <= '9')
                dv = (v[0] - '0') * 10 + (dot[1] - '0');
            else if (!dot) num(v, 0, 99, &dv);
            const int code = ls_lora_port_tcxo_code((int)dv);
            if (code < 0) { snprintf(why, n, "tcxo: 1.6 1.7 1.8 2.2 2.4 2.7 3.0 or 3.3"); return false; }
            out->tcxo_code = (uint8_t)code;
        } else if (!strcmp(key, "rfsw")) {
            /* "6:08,7:00,8:06,10:08" - DIO, then the config byte in hex */
            out->rfsw_n = 0;
            const char *s = v;
            while (*s) {
                char *end = NULL;
                const long d = strtol(s, &end, 10);
                if (end == s || *end != ':') { snprintf(why, n, "rfsw: DIO:hex,DIO:hex"); return false; }
                s = end + 1;
                const long c = strtol(s, &end, 16);
                if (end == s || (*end && *end != ',')) { snprintf(why, n, "rfsw: DIO:hex,DIO:hex"); return false; }
                if (out->rfsw_n >= LS_LORA_PORT_RFSW_MAX) { snprintf(why, n, "rfsw: at most %d entries", LS_LORA_PORT_RFSW_MAX); return false; }
                out->rfsw[out->rfsw_n].dio = (uint8_t)(d < 0 || d > 255 ? 255 : d);
                out->rfsw[out->rfsw_n].config = (uint8_t)(c < 0 || c > 255 ? 255 : c);
                out->rfsw_n++;
                s = *end ? end + 1 : end;
            }
        } else {
            snprintf(why, n, "'%s': cs busy rst irq dio tcxo rfsw", argv[i]);
            return false;
        }
    }
    return ls_lora_port_check(out, why, n);
}

void ls_lora_port_describe(const ls_lora_port_t *p, char *out, size_t n)
{
    if (!p->header) { snprintf(out, n, "onboard socket"); return; }
    char rst[12], irq[12], dio[8], sw[64] = "";
    if (p->rst == LS_LORA_PORT_RST_BOARD) snprintf(rst, sizeof(rst), "board");
    else snprintf(rst, sizeof(rst), "%d", p->rst);
    if (p->irq_gpio < 0) snprintf(irq, sizeof(irq), "none");
    else snprintf(irq, sizeof(irq), "%d", p->irq_gpio);
    if (p->irq_dio < 0) snprintf(dio, sizeof(dio), "none");
    else snprintf(dio, sizeof(dio), "%d", p->irq_dio);
    size_t at = 0;
    for (int i = 0; i < p->rfsw_n && at + 8 < sizeof(sw); i++)
        at += (size_t)snprintf(sw + at, sizeof(sw) - at, "%s%u:%02X", i ? "," : "", p->rfsw[i].dio, p->rfsw[i].config);
    const int dv = ls_lora_port_tcxo_dv(p->tcxo_code);
    snprintf(out, n, "header cs=%d busy=%d rst=%s irq=%s dio=%s tcxo=%d.%d rfsw=%s", p->cs, p->busy, rst, irq, dio,
             dv / 10, dv % 10, p->rfsw_n ? sw : "none");
}

/* --------------------------------------------------------------- storage -- */

void ls_lora_port_pack(const ls_lora_port_t *p, uint64_t *a, uint64_t *b)
{
    uint64_t x = PORT_VERSION;
    x |= (uint64_t)(p->header ? 1 : 0) << 8;
    x |= (uint64_t)((uint8_t)p->cs & 0x7F) << 9;
    x |= (uint64_t)((uint8_t)p->busy & 0x7F) << 16;
    x |= (uint64_t)((uint8_t)(p->rst + 2) & 0x7F) << 23;
    x |= (uint64_t)((uint8_t)(p->irq_gpio + 1) & 0x7F) << 30;
    x |= (uint64_t)((uint8_t)(p->irq_dio + 1) & 0x0F) << 37;
    x |= (uint64_t)(p->tcxo_code & 7) << 41;
    x |= (uint64_t)(p->rfsw_n & 7) << 44;
    uint64_t y = 0;
    for (int i = 0; i < p->rfsw_n && i < LS_LORA_PORT_RFSW_MAX; i++)
        y |= (uint64_t)((p->rfsw[i].dio & 0x0F) | (p->rfsw[i].config & 0x1F) << 4) << (9 * i);
    *a = x;
    *b = y;
}

bool ls_lora_port_unpack(uint64_t a, uint64_t b, ls_lora_port_t *out)
{
    if ((a & 0xFF) != PORT_VERSION) return false;
    memset(out, 0, sizeof(*out));
    out->header = (a >> 8) & 1;
    out->cs = (int8_t)((a >> 9) & 0x7F);
    out->busy = (int8_t)((a >> 16) & 0x7F);
    out->rst = (int8_t)((int)((a >> 23) & 0x7F) - 2);
    out->irq_gpio = (int8_t)((int)((a >> 30) & 0x7F) - 1);
    out->irq_dio = (int8_t)((int)((a >> 37) & 0x0F) - 1);
    out->tcxo_code = (uint8_t)((a >> 41) & 7);
    out->rfsw_n = (uint8_t)((a >> 44) & 7);
    for (int i = 0; i < out->rfsw_n; i++) {
        const unsigned e = (unsigned)((b >> (9 * i)) & 0x1FF);
        out->rfsw[i].dio = (uint8_t)(e & 0x0F);
        out->rfsw[i].config = (uint8_t)(e >> 4);
    }
    return true;
}

bool ls_lora_port_saved(ls_lora_port_t *out)
{
    uint64_t a = 0, b = 0;
    char why[96];
    if (settings_get_lora_port(&a, &b) && ls_lora_port_unpack(a, b, out) && ls_lora_port_check(out, why, sizeof(why)))
        return true;
    ls_lora_port_board(out);
    return false;
}

bool ls_lora_port_save(const ls_lora_port_t *p, char *why, size_t n)
{
    if (!ls_lora_port_check(p, why, n)) return false;
    uint64_t a, b;
    ls_lora_port_pack(p, &a, &b);
    if (!settings_set_lora_port(a, b)) { snprintf(why, n, "could not save to NVS"); return false; }
    return true;
}

bool ls_lora_port_toggle(char *why, size_t n)
{
    ls_lora_port_t p;
    ls_lora_port_saved(&p);
    p.header = !p.header;
    return ls_lora_port_save(&p, why, n);
}

static ls_lora_port_t s_port;
static bool s_port_read;

const ls_lora_port_t *ls_lora_port(void)
{
    /* Read once, at the first radio start; a saved change waits for a boot.
       A saved onboard choice keeps its header pins for the next switch and
       uses the board's own lines. */
    if (!s_port_read) {
        ls_lora_port_t p;
        if (!ls_lora_port_saved(&p) || !p.header) ls_lora_port_board(&p);
        s_port = p;
        __atomic_store_n(&s_port_read, true, __ATOMIC_RELEASE);
    }
    return &s_port;
}

bool ls_lora_port_is_header(void) { return ls_lora_port()->header; }
/* The keypad asks at board bring-up, before settings are loaded, so this
   reads afresh until the radio has fixed the boot's port; caching what an
   early read saw would hide a saved header port for the whole boot. */
bool ls_lora_port_blocks_keyboard(void)
{
    if (__atomic_load_n(&s_port_read, __ATOMIC_ACQUIRE)) return ls_lora_port_keyboard_pins(&s_port) != 0;
    ls_lora_port_t p;
    if (!ls_lora_port_saved(&p) || !p.header) return false;
    return ls_lora_port_keyboard_pins(&p) != 0;
}

/* -------------------------------------------------------------- console -- */

static void print_keyboard(const ls_lora_port_t *p)
{
    const uint64_t kb = ls_lora_port_keyboard_pins(p);
    if (!kb) return;
    printf("lora port: GPIO");
    for (int g = 0; g <= GPIO_MAX; g++) if (kb & (1ull << g)) printf(" %d", g);
    printf(" %s the keyboard's, so the keyboard and MixRF stay off in header mode\n",
           __builtin_popcountll(kb) > 1 ? "are" : "is");
}

int ls_lora_port_command(int argc, char **argv)
{
    /* argv[0] "lora", argv[1] "port" */
    char line[160], why[112];
    const ls_lora_port_t *now = ls_lora_port();
    ls_lora_port_t next;
    const bool saved = ls_lora_port_saved(&next);
    if (argc <= 2) {
        ls_lora_port_describe(now, line, sizeof(line));
        printf("lora port: this boot %s\n", line);
        ls_lora_port_t eff = next;
        if (!eff.header) ls_lora_port_board(&eff);
        ls_lora_port_describe(&eff, line, sizeof(line));
        printf("lora port: next boot %s%s\n", line, saved ? "" : " (nothing saved)");
        if (saved && !next.header) {
            ls_lora_port_t h = next;
            h.header = true;
            ls_lora_port_describe(&h, line, sizeof(line));
            printf("lora port: header pins kept: %s\n", line);
        }
        print_keyboard(now);
        puts("usage: lora port onboard | header [cs=N busy=N rst=N|board irq=N|none dio=5-11|none "
             "tcxo=1.6..3.3 rfsw=DIO:hex,...]");
        return 0;
    }
    if (argc == 3 && !strcmp(argv[2], "onboard")) {
        /* the header pins stay saved, for 'lora port header' or the settings
           toggle to come back to */
        ls_lora_port_t want = next;
        want.header = false;
        if (!ls_lora_port_save(&want, why, sizeof(why))) { printf("lora port: %s\n", why); return 1; }
        printf("lora port: saved onboard socket - reboot to use it\n");
        return 0;
    }
    /* A header line starts from what was saved, so changing one pin does not
       mean typing all of them. */
    ls_lora_port_t base = next;
    base.header = true;
    ls_lora_port_t want;
    if (!ls_lora_port_parse(argc - 2, argv + 2, &base, &want, why, sizeof(why))) {
        printf("lora port: %s\n", why);
        return 1;
    }
    if (!ls_lora_port_save(&want, why, sizeof(why))) {
        printf("lora port: %s\n", why);
        return 1;
    }
    ls_lora_port_describe(&want, line, sizeof(line));
    printf("lora port: saved %s - reboot to use it\n", line);
    print_keyboard(&want);
    return 0;
}

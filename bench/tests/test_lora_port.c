/* LS_TEST_SOURCES: ls_lora_port.c
 *
 * Where the LoRa radio is wired (issue 11): the board's socket, or a module
 * on the feature header with its own CS, BUSY, reset, interrupt DIO, TCXO
 * supply and RF switch table. The parse, the checks, what the keyboard loses,
 * and the two NVS words.
 */

#include "ls_test.h"
#include "ls_lora_port.h"

#include <string.h>

static uint64_t s_a, s_b;
static bool s_have;
bool settings_get_lora_port(uint64_t *a, uint64_t *b)
{
    if (!s_have) return false;
    *a = s_a; *b = s_b;
    return true;
}
bool settings_set_lora_port(uint64_t a, uint64_t b) { s_a = a; s_b = b; s_have = true; return true; }

static int words(char *line, char **argv)
{
    int n = 0;
    for (char *t = strtok(line, " "); t && n < 16; t = strtok(NULL, " ")) argv[n++] = t;
    return n;
}

static bool parse(const char *text, ls_lora_port_t *out, char *why, size_t n)
{
    char line[200];
    snprintf(line, sizeof line, "%s", text);
    char *argv[16];
    const int argc = words(line, argv);
    ls_lora_port_t base;
    ls_lora_port_board(&base);
    return ls_lora_port_parse(argc, argv, &base, out, why, n);
}

LS_CASE(the_board_port_is_the_socket_the_variant_describes)
{
    ls_lora_port_t p;
    ls_lora_port_board(&p);
    LS_CHECK(!p.header);
    LS_EQ_INT(p.cs, 24);
    LS_EQ_INT(p.busy, 6);
    LS_EQ_INT(p.rst, LS_LORA_PORT_RST_BOARD);
    LS_EQ_INT(p.irq_dio, 11);
    LS_EQ_INT(ls_lora_port_tcxo_dv(p.tcxo_code), 33);
    LS_EQ_INT(p.rfsw_n, 4);
    LS_EQ_UINT(p.rfsw[2].dio, 8);
    LS_EQ_UINT(p.rfsw[2].config, 0x06);
    char why[96];
    LS_CHECK(ls_lora_port_check(&p, why, sizeof why));
    LS_EQ_UINT(ls_lora_port_keyboard_pins(&p), 0);
}

LS_CASE(a_header_module_with_every_field)
{
    ls_lora_port_t p;
    char why[112];
    LS_CHECK_MSG(parse("header cs=33 busy=32 rst=26 irq=31 dio=9 tcxo=1.8 rfsw=5:02,6:08,10:0C", &p, why, sizeof why),
                 "%s", why);
    LS_CHECK(p.header);
    LS_EQ_INT(p.cs, 33);
    LS_EQ_INT(p.busy, 32);
    LS_EQ_INT(p.rst, 26);
    LS_EQ_INT(p.irq_gpio, 31);
    LS_EQ_INT(p.irq_dio, 9);
    LS_EQ_INT(ls_lora_port_tcxo_dv(p.tcxo_code), 18);
    LS_EQ_INT(p.rfsw_n, 3);
    LS_EQ_UINT(p.rfsw[2].dio, 10);
    LS_EQ_UINT(p.rfsw[2].config, 0x0C);
    char line[160];
    ls_lora_port_describe(&p, line, sizeof line);
    LS_EQ_STR(line, "header cs=33 busy=32 rst=26 irq=31 dio=9 tcxo=1.8 rfsw=5:02,6:08,10:0C");
}

LS_CASE(what_a_header_line_cannot_say)
{
    ls_lora_port_t p;
    char why[112];
    LS_CHECK(!parse("header cs=7 busy=32 rst=26", &p, why, sizeof why));           /* the board's I2C */
    LS_CHECK_MSG(strstr(why, "board's own") != NULL, "%s", why);
    LS_CHECK(!parse("header cs=2 busy=32 rst=26", &p, why, sizeof why));           /* the bus clock */
    LS_CHECK(!parse("header cs=33 busy=33 rst=26", &p, why, sizeof why));          /* one pin twice */
    LS_CHECK_MSG(strstr(why, "both GPIO 33") != NULL, "%s", why);
    LS_CHECK(!parse("header cs=33 busy=32 rst=board", &p, why, sizeof why));       /* resets the socket instead */
    LS_CHECK(!parse("header cs=33 busy=32 rst=26 rfsw=7:10", &p, why, sizeof why)); /* HF transmit */
    LS_CHECK_MSG(strstr(why, "HF transmit") != NULL, "%s", why);
    LS_CHECK(!parse("header cs=33 busy=32 rst=26 rfsw=4:02", &p, why, sizeof why)); /* no DIO4 */
    LS_CHECK(!parse("header cs=33 busy=32 rst=26 dio=9 rfsw=9:02", &p, why, sizeof why)); /* DIO9 is the IRQ */
    LS_CHECK(!parse("header cs=33 busy=32 rst=26 tcxo=2.0", &p, why, sizeof why));
    LS_CHECK(!parse("header cs=99", &p, why, sizeof why));
    LS_CHECK(!parse("header pins=1", &p, why, sizeof why));
    LS_CHECK(!parse("socket", &p, why, sizeof why));
}

LS_CASE(the_socket_lines_as_a_header_port_are_the_bench_test)
{
    /* header mode pointed at the socket: how it is tried without a module */
    ls_lora_port_t p;
    char why[112];
    LS_CHECK_MSG(parse("header cs=24 busy=6 rst=board", &p, why, sizeof why), "%s", why);
    LS_CHECK(p.header);
    LS_EQ_UINT(ls_lora_port_keyboard_pins(&p), 0);
}

LS_CASE(keyboard_pins_turn_the_keyboard_off)
{
    ls_lora_port_t p;
    char why[112];
    LS_CHECK_MSG(parse("header cs=36 busy=48 rst=26", &p, why, sizeof why), "%s", why);
    LS_EQ_UINT(ls_lora_port_keyboard_pins(&p), (1ull << 36) | (1ull << 48));
    LS_CHECK_MSG(parse("header cs=33 busy=32 rst=26", &p, why, sizeof why), "%s", why);
    LS_EQ_UINT(ls_lora_port_keyboard_pins(&p), 0);
    p.header = false;
    p.cs = 36;
    LS_EQ_UINT(ls_lora_port_keyboard_pins(&p), 0);    /* only a header port counts */
}

LS_CASE(the_two_nvs_words_round_trip)
{
    ls_lora_port_t p, q;
    char why[112];
    LS_CHECK_MSG(parse("header cs=54 busy=1 rst=53 irq=none dio=none tcxo=3.0 rfsw=5:01,6:02,7:04,8:08,9:0F,10:00,11:0E",
                       &p, why, sizeof why), "%s", why);
    uint64_t a, b;
    ls_lora_port_pack(&p, &a, &b);
    LS_CHECK(ls_lora_port_unpack(a, b, &q));
    LS_CHECK(q.header);
    LS_EQ_INT(q.cs, 54);
    LS_EQ_INT(q.busy, 1);
    LS_EQ_INT(q.rst, 53);
    LS_EQ_INT(q.irq_gpio, LS_LORA_PORT_NONE);
    LS_EQ_INT(q.irq_dio, LS_LORA_PORT_NONE);
    LS_EQ_INT(q.tcxo_code, p.tcxo_code);
    LS_EQ_INT(q.rfsw_n, 7);
    for (int i = 0; i < 7; i++) {
        LS_EQ_UINT(q.rfsw[i].dio, p.rfsw[i].dio);
        LS_EQ_UINT(q.rfsw[i].config, p.rfsw[i].config);
    }
    ls_lora_port_board(&p);
    ls_lora_port_pack(&p, &a, &b);
    LS_CHECK(ls_lora_port_unpack(a, b, &q));
    LS_EQ_INT(q.rst, LS_LORA_PORT_RST_BOARD);
    LS_CHECK(!ls_lora_port_unpack(a & ~0xFFull, b, &q));   /* unknown version */
}

LS_CASE(the_toggle_keeps_the_header_pins_and_nothing_saved_means_the_socket)
{
    s_have = false;
    ls_lora_port_t p;
    LS_CHECK(!ls_lora_port_saved(&p));
    LS_CHECK(!p.header);
    LS_EQ_INT(p.cs, 24);

    char why[112];
    LS_CHECK(parse("header cs=33 busy=32 rst=26", &p, why, sizeof why));
    LS_CHECK(ls_lora_port_save(&p, why, sizeof why));
    LS_CHECK(ls_lora_port_toggle(why, sizeof why));          /* to onboard */
    ls_lora_port_t q;
    LS_CHECK(ls_lora_port_saved(&q));
    LS_CHECK(!q.header);
    LS_EQ_INT(q.cs, 33);                                     /* pins kept */
    LS_CHECK(ls_lora_port_toggle(why, sizeof why));          /* back */
    LS_CHECK(ls_lora_port_saved(&q));
    LS_CHECK(q.header);
    LS_EQ_INT(q.rst, 26);
}

LS_CASE(the_toggle_never_makes_a_header_port_out_of_the_socket)
{
    /* Issue 11: Settings > LoRa radio pressed with no module pins saved gave
       "header cs=24 busy=6 rst=board", the onboard radio under another name.
       Now it is the board's module on CON1. */
    s_have = false;
    char why[112] = "";
    LS_CHECK_MSG(ls_lora_port_toggle(why, sizeof why), "%s", why);
    ls_lora_port_t q;
    LS_CHECK(ls_lora_port_saved(&q));
    LS_CHECK(q.header);
    LS_CHECK(ls_lora_port_is_con1(&q));
    LS_CHECK(!ls_lora_port_on_socket(&q));
    LS_EQ_INT(q.irq_gpio, 25);
    LS_CHECK(ls_lora_port_toggle(why, sizeof why));          /* and back */
    LS_CHECK(ls_lora_port_saved(&q));
    LS_CHECK(!q.header);

    /* the same from a saved onboard choice that never had module pins */
    s_have = false;
    char a0[] = "lora", a1[] = "port", a2[] = "onboard";
    char *argv[] = { a0, a1, a2 };
    LS_EQ_INT(ls_lora_port_command(3, argv), 0);
    LS_CHECK(ls_lora_port_toggle(why, sizeof why));
    LS_CHECK(ls_lora_port_saved(&q));
    LS_CHECK(ls_lora_port_is_con1(&q));

    /* back from a socket header port is still allowed */
    LS_CHECK(parse("header cs=24 busy=6 rst=board", &q, why, sizeof why));
    LS_CHECK(ls_lora_port_on_socket(&q));
    LS_CHECK(ls_lora_port_save(&q, why, sizeof why));
    LS_CHECK(ls_lora_port_toggle(why, sizeof why));
    LS_CHECK(ls_lora_port_saved(&q));
    LS_CHECK(!q.header);
}

LS_CASE(the_reporters_header_module_line)
{
    /* the LR2021 module on CON1 in issue 11 */
    s_have = false;
    char a0[] = "lora", a1[] = "port", a2[] = "header", a3[] = "cs=36", a4[] = "busy=33", a5[] = "rst=27",
         a6[] = "irq=25", a7[] = "dio=11", a8[] = "tcxo=1.8", a9[] = "rfsw=6:08,8:06,10:08";
    char *argv[] = { a0, a1, a2, a3, a4, a5, a6, a7, a8, a9 };
    LS_EQ_INT(ls_lora_port_command(10, argv), 0);
    ls_lora_port_t q;
    LS_CHECK(ls_lora_port_saved(&q));
    LS_CHECK(q.header);
    LS_CHECK(!ls_lora_port_on_socket(&q));
    LS_EQ_INT(q.cs, 36);
    LS_EQ_INT(q.busy, 33);
    LS_EQ_INT(q.rst, 27);
    LS_EQ_INT(q.irq_gpio, 25);
    LS_EQ_INT(q.irq_dio, 11);
    LS_EQ_INT(ls_lora_port_tcxo_dv(q.tcxo_code), 18);
    LS_EQ_INT(q.rfsw_n, 3);
    LS_EQ_UINT(q.rfsw[1].dio, 8);
    LS_EQ_UINT(q.rfsw[1].config, 0x06);
    /* CS, GDO0 and the NFC select are MixRF lines: the keyboard goes off */
    LS_EQ_UINT(ls_lora_port_keyboard_pins(&q), (1ull << 36) | (1ull << 27) | (1ull << 25));
    /* and the Settings toggle goes off and on again without losing them */
    char why[112];
    LS_CHECK(ls_lora_port_toggle(why, sizeof why));
    LS_CHECK(ls_lora_port_toggle(why, sizeof why));
    LS_CHECK(ls_lora_port_saved(&q));
    LS_CHECK(q.header);
    LS_EQ_INT(q.cs, 36);
}

LS_CASE(con1_is_the_module_on_the_2x8_header)
{
    ls_lora_port_t p, board;
    char why[112];
    ls_lora_port_board(&board);
    LS_CHECK_MSG(parse("header con1", &p, why, sizeof why), "%s", why);
    LS_CHECK(p.header);
    LS_EQ_INT(p.cs, 36);
    LS_EQ_INT(p.busy, 33);
    LS_EQ_INT(p.rst, 27);
    LS_EQ_INT(p.irq_gpio, 25);
    LS_EQ_INT(p.irq_dio, board.irq_dio);
    LS_EQ_INT(p.tcxo_code, board.tcxo_code);
    LS_EQ_INT(p.rfsw_n, board.rfsw_n);
    for (int i = 0; i < p.rfsw_n; i++) {
        LS_EQ_UINT(p.rfsw[i].dio, board.rfsw[i].dio);
        LS_EQ_UINT(p.rfsw[i].config, board.rfsw[i].config);
    }
    /* later words still change it */
    LS_CHECK_MSG(parse("header con1 tcxo=1.8", &p, why, sizeof why), "%s", why);
    LS_EQ_INT(ls_lora_port_tcxo_dv(p.tcxo_code), 18);
    LS_EQ_INT(p.cs, 36);
}

LS_CASE(the_console_saves_for_the_next_boot)
{
    s_have = false;
    char a0[] = "lora", a1[] = "port", a2[] = "header", a3[] = "cs=33", a4[] = "busy=32", a5[] = "rst=26";
    char *argv[] = { a0, a1, a2, a3, a4, a5 };
    LS_EQ_INT(ls_lora_port_command(6, argv), 0);
    ls_lora_port_t q;
    LS_CHECK(ls_lora_port_saved(&q));
    LS_CHECK(q.header);
    LS_EQ_INT(q.cs, 33);
    /* a second line changes one pin and keeps the rest */
    char b3[] = "busy=30";
    char *argv2[] = { a0, a1, a2, b3 };
    LS_EQ_INT(ls_lora_port_command(4, argv2), 0);
    LS_CHECK(ls_lora_port_saved(&q));
    LS_EQ_INT(q.cs, 33);
    LS_EQ_INT(q.busy, 30);
    char c2[] = "onboard";
    char *argv3[] = { a0, a1, c2 };
    LS_EQ_INT(ls_lora_port_command(3, argv3), 0);
    LS_CHECK(ls_lora_port_saved(&q));
    LS_CHECK(!q.header);
    char bad[] = "cs=7";
    char *argv4[] = { a0, a1, a2, bad };
    LS_CHECK(ls_lora_port_command(4, argv4) != 0);
}

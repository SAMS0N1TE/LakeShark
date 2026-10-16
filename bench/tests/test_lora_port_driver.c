/* LS_TEST_SOURCES: ls_lora.c, ls_lora_sx126x.c, ls_lora_lr20xx.c, ls_lora_port.c and ls_spi.c
 *
 * The driver with a module on the header (issue 11): saved before the first
 * start, it resets the part on its own GPIO and never the expander, holds the
 * socket's select high, and sets up the module's TCXO supply, interrupt DIO
 * and RF switch table instead of the board's.
 */

#include "lora_fakes.h"
#include "ls_lora_lr20xx.h"
#include "ls_lora_port.h"

static int find_frame(uint16_t op, int a0)
{
    for (int i = 0; i < fk.nframes; i++)
        if (fk.frames[i].len >= 3 && fk.frames[i].b[0] == (op >> 8) && fk.frames[i].b[1] == (op & 0xFF) &&
            (a0 < 0 || fk.frames[i].b[2] == a0))
            return i;
    return -1;
}

LS_CASE(a_header_lr2021_resets_on_its_gpio_and_takes_its_own_wiring)
{
    ls_lora_port_t p;
    ls_lora_port_board(&p);
    char line[] = "header cs=33 busy=32 rst=26 irq=48 dio=9 tcxo=1.8 rfsw=5:02,6:08";
    char *argv[12];
    int argc = 0;
    for (char *t = strtok(line, " "); t; t = strtok(NULL, " ")) argv[argc++] = t;
    char why[112];
    LS_CHECK_MSG(ls_lora_port_parse(argc, argv, &p, &p, why, sizeof why), "%s", why);
    LS_CHECK(ls_lora_port_save(&p, why, sizeof why));

    /* The keypad asks first, at bring-up, before the radio has read the
       port: it must see the saved header (irq=48 is the keypad's interrupt
       line) and must not fix the boot's port as the socket. */
    LS_CHECK(ls_lora_port_blocks_keyboard());

    fk_rst_gpio = 26;
    fk_install(FK_LR);
    fk_gpio_log_n = 0;
    LS_EQ_INT(ls_lora_start(), ESP_OK);
    LS_EQ_INT(ls_lora_chip(), LS_LORA_CHIP_LR20XX);
    LS_CHECK(ls_lora_port_is_header());
    LS_CHECK(ls_lora_port_blocks_keyboard());

    /* reset low then high on GPIO 26; the expander's reset never touched */
    int lows = 0, highs = 0, socket_cs_high = 0;
    for (int i = 0; i < fk_gpio_log_n; i++) {
        if (fk_gpio_log[i].pin == 26 && !fk_gpio_log[i].level) lows++;
        if (fk_gpio_log[i].pin == 26 && fk_gpio_log[i].level) highs++;
        if (fk_gpio_log[i].pin == 24 && fk_gpio_log[i].level) socket_cs_high++;
    }
    LS_CHECK_MSG(lows >= 1 && highs >= 1, "reset pulses: %d low %d high", lows, highs);
    LS_EQ_INT(socket_cs_high, 1);
    for (int i = 0; i < fk_xl_log_n; i++)
        LS_CHECK_MSG(fk_xl_log[i].pin != LS_BOARD_XL_RADIO_RST, "the expander reset was written");

    /* TCXO at 1.8 V (code 2), when the clock is the TCXO */
    const int tcxo = find_frame(LR20XX_OP_SET_TCXO_MODE, -1);
    LS_CHECK(tcxo >= 0);
    if (tcxo >= 0) LS_EQ_UINT(fk.frames[tcxo].b[2], 2);

    /* the module's DIOs, not the board's */
    const int before = fk.nframes;
    LS_EQ_INT(lr20xx_configure_dios(), ESP_OK);
    (void)before;
    LS_CHECK(find_frame(LR20XX_OP_SET_DIO_FUNCTION, 9) >= 0);       /* the IRQ */
    LS_CHECK(find_frame(LR20XX_OP_SET_DIO_IRQ, 9) >= 0);
    LS_CHECK(find_frame(LR20XX_OP_SET_DIO_RFSW, 5) >= 0);
    LS_CHECK(find_frame(LR20XX_OP_SET_DIO_RFSW, 6) >= 0);
    LS_CHECK(find_frame(LR20XX_OP_SET_DIO_FUNCTION, 11) < 0);       /* the board's IRQ DIO */
    LS_CHECK(find_frame(LR20XX_OP_SET_DIO_RFSW, 8) < 0);            /* the board's LF switch */
}

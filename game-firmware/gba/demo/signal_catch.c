#include <stdint.h>
#include <stdbool.h>

enum { WIDTH = 240, HEIGHT = 160, PLAYER_X = 18 };
#define REG16(address) (*(volatile uint16_t *)(address))
static volatile uint16_t *const video = (volatile uint16_t *)0x06000000;

static uint16_t color(int r, int g, int b)
{
    return r | (g << 5) | (b << 10);
}

static void rect(volatile uint16_t *page, int x, int y, int w, int h, uint16_t ink)
{
    for (int row = y; row < y + h; ++row)
        for (int col = x; col < x + w; ++col)
            page[row * WIDTH + col] = ink;
}

void game_main(void)
{
    /* Mode 3 uses one direct RGB555 page. Erase just the old three sprites. */
    REG16(0x04000000) = 0x0403;
    const uint16_t navy = color(2, 4, 9);
    const uint16_t teal = color(4, 30, 23);
    const uint16_t gold = color(31, 22, 8);
    const uint16_t faded = color(7, 12, 16);
    rect(video, 0, 0, WIDTH, HEIGHT, navy);
    rect(video, 0, 15, WIDTH, 1, faded);
    rect(video, 0, HEIGHT - 5, WIDTH, 1, faded);
    int player_y = 70, pulse_x = 220, pulse_y = 80, age = 0, score = 0;
    uint32_t seed = 10319;
    for (;;) {
        while (REG16(0x04000006) >= 160) {}
        while (REG16(0x04000006) < 160) {}
        rect(video, PLAYER_X, player_y, 6, 19, navy);
        rect(video, pulse_x, pulse_y, 7, 7, navy);

        uint16_t buttons = ~REG16(0x04000130);
        if ((buttons & (1 << 6)) && player_y > 19) player_y -= 2;
        if ((buttons & (1 << 7)) && player_y < 132) player_y += 2;
        pulse_x -= 2 + (score > 5) + (score > 10);
        if (pulse_x <= PLAYER_X + 6) {
            bool caught = pulse_y + 6 >= player_y && pulse_y <= player_y + 18;
            score = caught ? ((score + 1) & 15) : 0;
            rect(video, 8, 6, 128, 4, navy);
            for (int i = 0; i < score; ++i) rect(video, 8 + 8 * i, 6, 6, 4, gold);
            pulse_x = 220;
            seed = seed * 1664525u + 1013904223u;
            pulse_y = 22 + (int)((seed >> 16) & 127);
        }
        rect(video, PLAYER_X, player_y, 6, 19, teal);
        rect(video, pulse_x, pulse_y, 7, 7, (age++ & 16) ? gold : teal);
    }
}

#include <stdio.h>
#include "pico/stdlib.h"
#include "ea_dogl128.h"

int main(void) {
    stdio_init_all();
    sleep_ms(2000);
    printf("Display library test\n");

    DOGL128Config cfg = {
        .pin_sck  = 18,
        .pin_mosi = 19,
        .pin_cs   = 17,
        .pin_a0   = 20,
        .pin_rst  = 21,
        .contrast = 0x15,
    };

    dogl128_init(&cfg);
    printf("Init done\n");

    int screen = 0;
    while (true) {
        screen++;
        dogl128_clear();

        switch (screen % 5) {
            case 1:
                printf("All pixels ON\n");
                dogl128_fill_rect(0, 0, 128, 64);
                break;
            case 2:
                printf("All pixels OFF\n");
                // clear already done — just flush blank
                break;
            case 3:
                printf("Text screen\n");
                dogl128_print(0, 0, "TABLE FOOTBALL");
                dogl128_print(0, 1, "--------------");
                dogl128_print(0, 3, "Alice  vs  Bob");
                dogl128_print(0, 5, "Score:  3 - 5");
                dogl128_print(0, 7, "Fast: 28 km/h");
                break;
            case 4:
                printf("Scoreboard\n");
                dogl128_rect(0, 0, 128, 64);
                dogl128_hline(0, 12, 128);
                dogl128_vline(64, 12, 52);
                dogl128_draw_string(30, 2, "SCORE");
                dogl128_draw_string(8,  18, "ALICE");
                dogl128_draw_string(75, 18, "BOB");
                dogl128_draw_string(22, 34, "3");
                dogl128_draw_string(86, 34, "5");
                dogl128_hline(0, 52, 128);
                dogl128_draw_string(4,  55, "Fast: 28 km/h");
                break;
            case 0:
                printf("Checkerboard\n");
                for (int y = 0; y < 64; y++)
                    for (int x = 0; x < 128; x++)
                        if ((x + y) % 2 == 0)
                            dogl128_set_pixel(x, y, true);
                break;
        }

        dogl128_flush();
        sleep_ms(3000);
    }
}

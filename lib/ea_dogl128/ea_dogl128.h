#pragma once
#include "pico/stdlib.h"
#include <stdint.h>
#include <stdbool.h>

#define DOGL128_WIDTH   128
#define DOGL128_HEIGHT  64
#define DOGL128_PAGES   8

typedef struct {
    uint    pin_sck, pin_mosi, pin_cs, pin_a0, pin_rst;
    uint8_t contrast;
} DOGL128Config;

void dogl128_init(const DOGL128Config *cfg);
void dogl128_clear(void);
void dogl128_flush(void);
void dogl128_set_pixel(int x, int y, bool on);
void dogl128_draw_char(int x, int y, char c);
void dogl128_draw_char_2x(int x, int y, char c);    // 2x scaled
void dogl128_draw_string(int x, int y, const char *str);
void dogl128_draw_string_2x(int x, int y, const char *str);
void dogl128_print(int col, int row, const char *str);
void dogl128_hline(int x, int y, int width);
void dogl128_vline(int x, int y, int height);
void dogl128_rect(int x, int y, int w, int h);
void dogl128_fill_rect(int x, int y, int w, int h);
void dogl128_invert_rect(int x, int y, int w, int h);
void dogl128_set_contrast(uint8_t val);

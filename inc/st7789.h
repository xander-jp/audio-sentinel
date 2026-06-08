#pragma once
#include <stdint.h>
#include <stdbool.h>

#define LCD_W 240
#define LCD_H 240

// RGB565 colors
#define COLOR_BLACK    0x0000
#define COLOR_WHITE    0xFFFF
#define COLOR_RED      0xF800
#define COLOR_GREEN    0x07E0
#define COLOR_BLUE     0x001F
#define COLOR_YELLOW   0xFFE0
#define COLOR_CYAN     0x07FF
#define COLOR_MAGENTA  0xF81F
#define COLOR_GRAY     0x8410
#define COLOR_DARKGRAY 0x4208

void lcd_init(void);
void lcd_fill(uint16_t color);
void lcd_fill_rect(int x, int y, int w, int h, uint16_t color);
void lcd_draw_text(int x, int y, const char* s, int scale, uint16_t fg, uint16_t bg);

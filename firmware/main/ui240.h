/* ui240.h - drawing helpers for the native 240x240 pages (Air, Bus):
 * RGB565 (native endian), the tank's 5x7 pixel font via render_text. */
#ifndef UI240_H
#define UI240_H
#include <stdint.h>
#include <stdbool.h>

#define UI_W 240
#define UI_H 240

#define UI_BG     0x000000
#define UI_TEXT   0xf2f2f2
#define UI_MUTED  0x8a8a8a
#define UI_DIM    0x4a4a4a
#define UI_LINE   0x1e1e1e

void ui_clear(uint16_t *fb);
void ui_fill(uint16_t *fb, int x, int y, int w, int h, uint32_t rgb);
void ui_rfill(uint16_t *fb, int x, int y, int w, int h, int r, uint32_t rgb);
void ui_dot(uint16_t *fb, int cx, int cy, int r, uint32_t rgb);
int  ui_text_w(const char *s, int scale);
void ui_text(uint16_t *fb, int x, int y, int scale, uint32_t rgb, const char *s);          /* upper-cased, clipped right */
void ui_text_fit(uint16_t *fb, int x, int y, int scale, uint32_t rgb, const char *s, int maxw);
void ui_text_r(uint16_t *fb, int xr, int y, int scale, uint32_t rgb, const char *s);       /* right-aligned at xr */
void ui_text_c(uint16_t *fb, int y, int scale, uint32_t rgb, const char *s);               /* centred */
void ui_text_cx(uint16_t *fb, int cx, int y, int scale, uint32_t rgb, const char *s);      /* centred on cx */
void ui_header(uint16_t *fb, const char *title, const char *sub);                          /* title + clock + subtitle */
void ui_page_dots(uint16_t *fb, int n, int cur, uint32_t accent);
void ui_toast(uint16_t *fb, const char *msg, uint32_t rgb);                                 /* mode-change pill */
#endif

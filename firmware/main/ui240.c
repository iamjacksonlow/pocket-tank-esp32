/* ui240.c - see ui240.h */
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <time.h>
#include "ui240.h"
#include "render.h"

static uint16_t c565(uint32_t c) { return (uint16_t)(((c >> 8) & 0xf800) | ((c >> 5) & 0x07e0) | ((c >> 3) & 0x1f)); }

void ui_clear(uint16_t *fb) { memset(fb, 0, UI_W * UI_H * 2); }

void ui_fill(uint16_t *fb, int x, int y, int w, int h, uint32_t rgb) {
    uint16_t v = c565(rgb);
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > UI_W) w = UI_W - x;
    if (y + h > UI_H) h = UI_H - y;
    for (int j = 0; j < h; j++) { uint16_t *p = fb + (y + j) * UI_W + x; for (int i = 0; i < w; i++) p[i] = v; }
}
void ui_rfill(uint16_t *fb, int x, int y, int w, int h, int r, uint32_t rgb) {
    for (int j = 0; j < h; j++) {
        int in = 0;
        if (j < r) in = r - (int)sqrtf((float)(r * r - (r - j) * (r - j)));
        else if (j >= h - r) { int d = j - (h - r - 1); in = r - (int)sqrtf((float)(r * r - d * d)); }
        ui_fill(fb, x + in, y + j, w - 2 * in, 1, rgb);
    }
}
void ui_dot(uint16_t *fb, int cx, int cy, int r, uint32_t rgb) {
    for (int j = -r; j <= r; j++) for (int i = -r; i <= r; i++) if (i * i + j * j <= r * r) ui_fill(fb, cx + i, cy + j, 1, 1, rgb);
}
static void upper(char *d, const char *s, size_t n) {
    size_t i = 0; for (; s[i] && i < n - 1; i++) d[i] = (char)toupper((unsigned char)s[i]); d[i] = 0;
}
int ui_text_w(const char *s, int scale) { char t[48]; upper(t, s, sizeof t); return render_text_w(t, scale); }
void ui_text(uint16_t *fb, int x, int y, int sc, uint32_t rgb, const char *s) {
    if (x < 0 || y < 0 || y + 7 * sc > UI_H) return;
    char t[48]; upper(t, s, sizeof t);
    int i = (int)strlen(t);
    while (i > 0 && x + render_text_w(t, sc) > UI_W) t[--i] = 0;
    render_text(fb, UI_W, x, y, sc, rgb, t);
}
void ui_text_fit(uint16_t *fb, int x, int y, int sc, uint32_t rgb, const char *s, int maxw) {
    char t[48]; upper(t, s, sizeof t);
    int l = (int)strlen(t);
    while (l > 0 && render_text_w(t, sc) > maxw) t[--l] = 0;
    while (l > 0 && t[l - 1] == ' ') t[--l] = 0;
    ui_text(fb, x, y, sc, rgb, t);
}
void ui_text_r(uint16_t *fb, int xr, int y, int sc, uint32_t rgb, const char *s) { ui_text(fb, xr - ui_text_w(s, sc), y, sc, rgb, s); }
void ui_text_c(uint16_t *fb, int y, int sc, uint32_t rgb, const char *s) { ui_text(fb, (UI_W - ui_text_w(s, sc)) / 2, y, sc, rgb, s); }
void ui_text_cx(uint16_t *fb, int cx, int y, int sc, uint32_t rgb, const char *s) { ui_text(fb, cx - ui_text_w(s, sc) / 2, y, sc, rgb, s); }

void ui_header(uint16_t *fb, const char *title, const char *sub) {
    ui_text_fit(fb, 8, 8, 2, UI_TEXT, title, 160);
    time_t now = time(NULL); struct tm tm; localtime_r(&now, &tm);
    if (tm.tm_year + 1900 >= 2025) { char c[8]; snprintf(c, sizeof c, "%d:%02d", tm.tm_hour, tm.tm_min); ui_text_r(fb, UI_W - 8, 8, 2, UI_MUTED, c); }
    if (sub) ui_text_fit(fb, 8, 27, 2, UI_MUTED, sub, UI_W - 16);
}
void ui_page_dots(uint16_t *fb, int n, int cur, uint32_t accent) {
    int gap = 12, x0 = (UI_W - (n - 1) * gap) / 2;
    for (int i = 0; i < n; i++) ui_dot(fb, x0 + i * gap, UI_H - 6, i == cur ? 3 : 2, i == cur ? accent : UI_DIM);
}
void ui_toast(uint16_t *fb, const char *msg, uint32_t rgb) {
    int w = ui_text_w(msg, 3) + 28, h = 37, x = (UI_W - w) / 2, y = (UI_H - h) / 2;
    ui_rfill(fb, x - 2, y - 2, w + 4, h + 4, 12, 0x000000);
    ui_rfill(fb, x, y, w, h, 10, 0x18222e);
    ui_text_c(fb, y + 8, 3, rgb, msg);
}

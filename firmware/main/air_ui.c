/* air_ui.c - see air_ui.h. Bands follow NEA:
 *   PSI (24-hr): 0-50 Good, 51-100 Moderate, 101-200 Unhealthy,
 *                201-300 Very unhealthy, >300 Hazardous
 *   PM2.5 (1-hr): 0-55 Normal, 56-150 Elevated, 151-250 High, >250 Very high
 *   UV index: 0-2 Low, 3-5 Moderate, 6-7 High, 8-10 Very high, 11+ Extreme
 *   Heat stress (WBGT): Low / Moderate / High
 * Page 1 (start-up) is the 1-hr PM2.5, page 2 the 24-hr PSI: NEA's hourly figure is a
 * PM2.5 concentration, not a PSI, so the two pages look deliberately different
 * (solid blue 1H badge + chart vs outlined 24H badge + tiles). */
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include "air_ui.h"
#include "ui240.h"
#if __has_include("user_config.h")
#include "user_config.h"
#else
#include "user_config.example.h"
#endif

#define C_GOOD   0x3ddc84
#define C_MOD    0x4db8ff
#define C_UNH    0xffb020
#define C_VUNH   0xff6a3d
#define C_HAZ    0xff3b6b
#define C_ACCENT 0x7fd3ff

static void psi_band(int v, const char **name, const char **advice, uint32_t *col) {
    if (v <= 50)       { *name = "GOOD";           *advice = "NORMAL ACTIVITIES";  *col = C_GOOD; }
    else if (v <= 100) { *name = "MODERATE";       *advice = "NORMAL ACTIVITIES";  *col = C_MOD; }
    else if (v <= 200) { *name = "UNHEALTHY";      *advice = "REDUCE OUTDOOR TIME"; *col = C_UNH; }
    else if (v <= 300) { *name = "VERY UNHEALTHY"; *advice = "AVOID OUTDOORS";      *col = C_VUNH; }
    else               { *name = "HAZARDOUS";      *advice = "STAY INDOORS";        *col = C_HAZ; }
}
static uint32_t psi_col(int v) { const char *a, *b; uint32_t c; psi_band(v, &a, &b, &c); return c; }

static void pm25_band(int v, const char **name, uint32_t *col) {
    if (v <= 55)       { *name = "NORMAL";    *col = C_GOOD; }
    else if (v <= 150) { *name = "ELEVATED";  *col = C_UNH; }
    else if (v <= 250) { *name = "HIGH";      *col = C_VUNH; }
    else               { *name = "VERY HIGH"; *col = C_HAZ; }
}
static void uv_band(int v, const char **name, uint32_t *col) {
    if (v <= 2)       { *name = "LOW";       *col = C_GOOD; }
    else if (v <= 5)  { *name = "MODERATE";  *col = C_MOD; }
    else if (v <= 7)  { *name = "HIGH";      *col = C_UNH; }
    else if (v <= 10) { *name = "VERY HIGH"; *col = C_VUNH; }
    else              { *name = "EXTREME";   *col = C_HAZ; }
}
static bool has(const char *hay, const char *needle) {
    char a[40], b[16]; size_t i;
    for (i = 0; hay[i] && i < sizeof a - 1; i++) a[i] = (char)tolower((unsigned char)hay[i]);
    a[i] = 0;
    for (i = 0; needle[i] && i < sizeof b - 1; i++) b[i] = (char)tolower((unsigned char)needle[i]);
    b[i] = 0;
    return strstr(a, b) != NULL;
}
static uint32_t rain_col(const char *f) {
    if (!f[0]) return UI_DIM;
    if (has(f, "thunder")) return C_VUNH;
    if (has(f, "heavy")) return C_UNH;
    if (has(f, "rain") || has(f, "shower")) return C_MOD;
    return C_GOOD;
}
static uint32_t heat_col(const char *h) {
    if (!h[0]) return UI_DIM;
    if (has(h, "high")) return C_VUNH;
    if (has(h, "moderate")) return C_UNH;
    return C_GOOD;
}

/* header with a badge on the subtitle row: solid blue "1H" (live) or outlined grey "24H" */
static void header_badge(uint16_t *fb, const char *title, const char *badge, bool live, const char *sub) {
    ui_header(fb, title, NULL);
    int w = ui_text_w(badge, 2) + 10;
    if (live) { ui_rfill(fb, 8, 25, w, 18, 4, C_ACCENT); ui_text(fb, 13, 27, 2, 0x000000, badge); }
    else { ui_rfill(fb, 8, 25, w, 18, 4, UI_MUTED); ui_rfill(fb, 9, 26, w - 2, 16, 3, 0x000000); ui_text(fb, 13, 27, 2, 0xd0d0d0, badge); }
    ui_text_fit(fb, 8 + w + 6, 27, 2, UI_MUTED, sub, UI_W - 22 - w);
    ui_fill(fb, 8, 48, UI_W - 16, 1, UI_LINE);
}
static uint32_t dim(uint32_t c, int pct) {
    int r = (c >> 16) & 255, g = (c >> 8) & 255, b = c & 255;
    return ((uint32_t)(r * pct / 100) << 16) | ((uint32_t)(g * pct / 100) << 8) | (uint32_t)(b * pct / 100);
}
static uint32_t pm_col(int v) { const char *n; uint32_t c; pm25_band(v, &n, &c); return c; }
static void tri(uint16_t *fb, int x, int y, bool up, uint32_t c) {      /* 11 x 7 arrow */
    for (int i = 0; i < 7; i++) { int row = up ? i : 6 - i; ui_fill(fb, x + 5 - i * 5 / 6, y + row, 1 + 2 * (i * 5 / 6), 1, c); }
}

/* page 1 (start-up): 1-hr PM2.5 now, the change since the last hour, the last 12 hours */
static void page_pm1h(uint16_t *fb, const air_data_t *d) {
    char s[32]; snprintf(s, sizeof s, "AS OF %s", d->pm25_time[0] ? d->pm25_time : "--");
    header_badge(fb, "PM2.5 CENTRAL", "1H", true, s);
    int v = d->pm25_1h[AIR_C];
    const char *band; uint32_t col; pm25_band(v, &band, &col);
    char n[8]; snprintf(n, sizeof n, "%d", v);
    ui_text(fb, 8, 56, 7, col, n);
    int rx = 8 + ui_text_w(n, 7) + 12;
    ui_text(fb, rx, 58, 2, UI_MUTED, "UG/M3");
    int prev = d->pm25_ok ? d->pm25_hist[10] : -1;
    if (prev >= 0) {
        int dv = v - prev;
        if (dv) { uint32_t tc = dv > 0 ? C_VUNH : C_GOOD; tri(fb, rx, 85, dv > 0, tc); snprintf(s, sizeof s, "%d", dv > 0 ? dv : -dv); ui_text(fb, rx + 15, 85, 2, tc, s); }
        else ui_text(fb, rx, 85, 2, UI_MUTED, "STEADY");
    }
    ui_text(fb, 8, 114, 2, col, band);
    ui_text_r(fb, UI_W - 8, 114, 2, UI_MUTED, "LAST 12H");
    int top = 136, bh = 50, base = top + bh, mx = 100;
    if (d->pm25_ok) {
        for (int i = 0; i < 12; i++) if (d->pm25_hist[i] > mx) mx = d->pm25_hist[i] * 11 / 10;
        for (int i = 0; i < 12; i++) {
            int hv = d->pm25_hist[i]; if (hv < 0) continue;
            int h = hv * bh / mx; if (h < 2) h = 2;
            ui_fill(fb, 12 + i * 18, base - h, 14, h, i == 11 ? pm_col(hv) : dim(pm_col(hv), 45));
        }
        int fh = d->pm25_first_hour;
        snprintf(s, sizeof s, "%d%s", fh % 12 ? fh % 12 : 12, fh < 12 ? "AM" : "PM");
        ui_text(fb, 10, base + 6, 2, UI_MUTED, s);
        ui_text_r(fb, UI_W - 10, base + 6, 2, UI_TEXT, "NOW");
        int ty = base - 55 * bh / mx;                              /* NORMAL / ELEVATED */
        for (int xx = 8; xx < UI_W - 8; xx += 6) ui_fill(fb, xx, ty, 3, 1, 0x9a9a9a);
    } else ui_text_c(fb, top + 18, 2, UI_DIM, "NO HISTORY YET");
    ui_fill(fb, 8, base + 1, UI_W - 16, 1, 0x3a3a3a);
    static const char *lab[4] = { "N", "S", "E", "W" };
    static const int idx[4] = { AIR_N, AIR_S, AIR_E, AIR_W };
    for (int i = 0; i < 4; i++) {
        int cx = 8 + i * 56 + 28, rv = d->pm25_1h[idx[i]]; snprintf(n, sizeof n, "%d", rv);
        int w = ui_text_w(lab[i], 2) + 6 + ui_text_w(n, 2), gx = cx - w / 2;
        ui_text(fb, gx, 214, 2, UI_MUTED, lab[i]); ui_text(fb, gx + ui_text_w(lab[i], 2) + 6, 214, 2, pm_col(rv), n);
        if (i) ui_fill(fb, 8 + i * 56, 213, 1, 16, 0x2a2a2a);
    }
}

/* page 2: 24-hr PSI, central big, the other four regions as tiles */
static void page_psi(uint16_t *fb, const air_data_t *d) {
    char sub[32]; snprintf(sub, sizeof sub, "AVG TO %s", d->psi_time[0] ? d->psi_time : "--");
    header_badge(fb, "PSI CENTRAL", "24H", false, sub);
    const char *band, *advice; uint32_t col;
    int c = d->psi24[AIR_C];
    psi_band(c, &band, &advice, &col);
    char n[16]; snprintf(n, sizeof n, "%d", c);
    ui_text_c(fb, 54, 9, col, n);
    ui_text_c(fb, 124, 3, col, band);
    ui_text_c(fb, 150, 2, UI_MUTED, advice);
    static const char *lab[4] = { "N", "S", "E", "W" };
    static const int idx[4] = { AIR_N, AIR_S, AIR_E, AIR_W };
    for (int i = 0; i < 4; i++) {
        int x = 8 + i * 58, y = 174, w = 50, h = 50, v = d->psi24[idx[i]];
        uint32_t cc = psi_col(v);
        ui_rfill(fb, x, y, w, h, 7, 0x10161d);
        ui_fill(fb, x + 6, y + h - 5, w - 12, 3, cc);
        ui_text_cx(fb, x + w / 2, y + 5, 2, UI_MUTED, lab[i]);
        snprintf(n, sizeof n, "%d", v);
        ui_text_cx(fb, x + w / 2, y + 21, 3 - (v >= 100 && ui_text_w(n, 3) > w - 4), cc, n);
    }
}

/* page 3: the other things that can hurt you today, near home (three roomy rows) */
static void alert_row(uint16_t *fb, int y, const char *label, const char *status, uint32_t col, const char *big) {
    ui_dot(fb, 15, y + 22, 6, col);
    ui_text(fb, 30, y + 6, 2, UI_MUTED, label);
    ui_text_fit(fb, 30, y + 26, 2, col, status, big ? 140 : UI_W - 38);
    if (big) ui_text_r(fb, UI_W - 8, y + 16, 3, col, big);
    ui_fill(fb, 8, y + 55, UI_W - 16, 1, UI_LINE);
}
static void page_alerts(uint16_t *fb, const air_data_t *d) {
    ui_header(fb, "ALERTS", USER_AREA_LABEL);
    ui_fill(fb, 8, 48, UI_W - 16, 1, UI_LINE);
    const char *name; uint32_t col; char big[12];
    if (d->uv >= 0) { uv_band(d->uv, &name, &col); snprintf(big, sizeof big, "%d", d->uv); alert_row(fb, 54, "UV INDEX", name, col, big); }
    else alert_row(fb, 54, "UV INDEX", "NO READING", UI_DIM, NULL);
    { char r[32]; strncpy(r, d->rain[0] ? d->rain : "NO FORECAST", sizeof r - 1); r[sizeof r - 1] = 0;
      char *paren = strchr(r, '('); if (paren) { *paren = 0; }           /* "Partly Cloudy (Day)" -> "Partly Cloudy" */
      alert_row(fb, 112, "RAIN  NEXT 2 HRS", r, rain_col(d->rain), NULL); }
    if (d->heat[0]) { snprintf(big, sizeof big, "%.0fC", d->wbgt); alert_row(fb, 170, "HEAT STRESS", d->heat, heat_col(d->heat), big); }
    else alert_row(fb, 170, "HEAT STRESS", "NO READING", UI_DIM, NULL);
}

void air_render(uint16_t *fb, const air_data_t *d, int page) {
    ui_clear(fb);
    if (!d->ok) {
        ui_header(fb, "AIR", NULL);
        ui_text_c(fb, 90, 3, UI_MUTED, "LOADING");
        ui_text_c(fb, 130, 2, UI_DIM, "DATA.GOV.SG");
        return;
    }
    if (page == 0) page_pm1h(fb, d); else if (page == 1) page_psi(fb, d); else page_alerts(fb, d);
    ui_page_dots(fb, AIR_PAGES, page, C_ACCENT);
}

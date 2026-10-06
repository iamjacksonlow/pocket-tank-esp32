/* air_ui.h - the AIR pages (1-hr PM2.5, 24-hr PSI, health alerts), drawn on 240x240.
 * Data from data.gov.sg (api-open.data.gov.sg/v2/real-time/api/...). */
#ifndef AIR_UI_H
#define AIR_UI_H
#include <stdint.h>
#include <stdbool.h>

enum { AIR_N, AIR_S, AIR_E, AIR_W, AIR_C, AIR_REGIONS };

typedef struct {
    bool ok;                      /* at least the PSI arrived */
    int  psi24[AIR_REGIONS];      /* 24-hr PSI */
    int  pm25_1h[AIR_REGIONS];    /* 1-hr PM2.5 (ug/m3) */
    char psi_time[8];             /* "18:00" */
    int  uv;                      /* UV index, -1 = n/a */
    char uv_time[8];
    char rain[32];                /* 2-hr forecast for USER_FORECAST_AREA, e.g. "Thundery Showers" */
    char rain_until[16];          /* "9.30 PM" */
    char heat[12];                /* "Low" / "Moderate" / "High", "" = n/a */
    float wbgt;
    /* 1-hr PM2.5 for Central over the last 12 hours (oldest first, -1 = no reading) */
    bool pm25_ok;
    int  pm25_hist[12];
    int  pm25_first_hour;         /* clock hour of pm25_hist[0], 0-23 */
    char pm25_time[8];            /* "20:00" */
} air_data_t;

#define AIR_PAGES 3               /* 1H PM2.5 (start-up), 24H PSI, alerts */
void air_render(uint16_t *fb240, const air_data_t *d, int page);
#endif

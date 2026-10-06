#include "batlog.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "progression.h"     /* clock_port_now_unix: the RTC's wall clock */
#include <string.h>
#include <stddef.h>
#include "esp_attr.h"
#include "nvs.h"

/* RTC slow memory, NO-INIT section (2026-09-15): the ring lives through
   deep sleep AND every other reset - a reflash, a software reset, a
   watchdog - and is discarded only when the magic + checksum say the RAM
   is garbage (a true power-on, or a PMIC power-off). It was RTC_DATA_ATTR
   before, which ESP-IDF zeroes on any reset that is not a deep-sleep wake:
   the first full night of deep sleep on battery (2026-09-14/15) was wiped
   by the morning's app flash before anyone read it. */
#define N 96
#define BATLOG_MAGIC 0xB47106A1u
typedef struct { int64_t us; int16_t pct, mv; uint8_t bright, asleep; char why[8]; } sample_t;
typedef struct { uint32_t magic; sample_t ring[N]; int n, head; uint32_t crc; } log_t;
RTC_NOINIT_ATTR static log_t s_log;
#define s_ring s_log.ring
#define s_n    s_log.n
#define s_head s_log.head

static uint32_t crc_of(const log_t *l) {     /* FNV-1a over everything but the crc field */
    const uint8_t *b = (const uint8_t *)l; uint32_t h = 2166136261u;
    for (size_t i = 0; i < offsetof(log_t, crc); i++) { h ^= b[i]; h *= 16777619u; }
    return h;
}
static bool valid(void) {
    return s_log.magic == BATLOG_MAGIC && s_log.n >= 0 && s_log.n <= N &&
           s_log.head >= 0 && s_log.head < N && s_log.crc == crc_of(&s_log);
}
static void seal(void) { s_log.magic = BATLOG_MAGIC; s_log.crc = crc_of(&s_log); }

void batlog_clear(void) { s_n = s_head = 0; seal(); }

/* The bedtime row also goes to NVS (2026-09-16): the RTC ring is lost to a
   power-off, and a cell that dies in the night IS a power-off - the morning
   of 09-16 found a wiped ring and no idea what SoC the tank went to bed at.
   An empty ring at boot is seeded from it, once ("bed"), so the printout
   still derives the night's mA between bedtime and the boot row. */
static void bed_store(const sample_t *s) {
    nvs_handle_t h; if (nvs_open("tank", NVS_READWRITE, &h) != ESP_OK) return;
    if (nvs_set_blob(h, "bed", s, sizeof *s) == ESP_OK) nvs_commit(h);
    nvs_close(h);
}
static bool bed_take(sample_t *s) {
    nvs_handle_t h; if (nvs_open("tank", NVS_READWRITE, &h) != ESP_OK) return false;
    size_t len = sizeof *s;
    bool ok = nvs_get_blob(h, "bed", s, &len) == ESP_OK && len == sizeof *s;
    if (ok) { nvs_erase_key(h, "bed"); nvs_commit(h); }
    nvs_close(h);
    return ok;
}
int batlog_init(void) {
    if (!valid()) {
        batlog_clear();
        sample_t bed;
        if (bed_take(&bed)) {
            strncpy(bed.why, "bed", sizeof bed.why - 1); bed.why[sizeof bed.why - 1] = 0; bed.asleep = 1;
            s_ring[0] = bed; s_n = 1; s_head = 1; seal();
            ESP_LOGI("batlog", "ring lost to a power-off; bedtime row restored from NVS (%d%%, %d mV)", bed.pct, bed.mv);
        }
        return 0;
    }
    return s_n;
}
void batlog_add(int pct, int mv, int bright, bool asleep, const char *why) {
    if (!valid()) batlog_clear();
    sample_t *s = &s_ring[s_head];
    /* wall clock in us (the PCF85063 sets it at every boot), so the stamp
       survives deep sleep - esp_timer restarts from zero at each wake;
       fall back to it only when the RTC is unset */
    int64_t unix = clock_port_now_unix();
    s->us = unix > 0 ? unix * 1000000LL : esp_timer_get_time();
    s->pct = (int16_t)pct; s->mv = (int16_t)mv;
    s->bright = (uint8_t)bright; s->asleep = asleep;
    strncpy(s->why, why ? why : "", sizeof s->why - 1); s->why[sizeof s->why - 1] = 0;
    s_head = (s_head + 1) % N; if (s_n < N) s_n++;
    seal();
    if (asleep && why && (!strcmp(why, "sleep") || !strcmp(why, "off"))) bed_store(s);   /* survives the cell dying */
}
void batlog_print(void) {
    if (!s_n) { ESP_LOGI("batlog", "no samples yet"); return; }
    int first = (s_head - s_n + N) % N;
    const sample_t *prev = NULL; double awake_mah = 0, awake_h = 0, sleep_mah = 0, sleep_h = 0;
    int64_t t_first = s_ring[first].us;
    ESP_LOGI("batlog", "%d samples (h:mm since the first | SoC | VBAT | bright | state | mA since previous)", s_n);
    for (int k = 0; k < s_n; k++) {
        const sample_t *s = &s_ring[(first + k) % N];
        double h = (s->us - t_first) / 3.6e9;
        char cur[24] = "";
        if (prev && s->pct >= 0 && prev->pct >= 0 && s->us > prev->us) {
            double dh = (s->us - prev->us) / 3.6e9, mah = (prev->pct - s->pct) * BATLOG_CELL_MAH / 100.0;
            snprintf(cur, sizeof cur, "%6.1f mA", mah / dh);
            if (prev->asleep) { sleep_mah += mah; sleep_h += dh; } else { awake_mah += mah; awake_h += dh; }
        }
        ESP_LOGI("batlog", "  %3d:%02d | %3d%% | %4d mV | %3d | %-6s | %-6s %s", (int)h, (int)((h - (int)h) * 60),
                 s->pct, s->mv, s->bright, s->asleep ? "asleep" : "awake", s->why, cur);
        prev = s;
    }
    if (awake_h > 0) ESP_LOGI("batlog", "  awake:  %.1f h, %.0f mAh -> %.0f mA average", awake_h, awake_mah, awake_mah / awake_h);
    if (sleep_h > 0) ESP_LOGI("batlog", "  asleep: %.1f h, %.1f mAh -> %.2f mA average", sleep_h, sleep_mah, sleep_mah / sleep_h);
    ESP_LOGI("batlog", "  (SoC is 1%% = 2 mAh; short awake windows are coarse, a night is not)");
}

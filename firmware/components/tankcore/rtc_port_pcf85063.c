/* rtc_port_pcf85063.c — PCF85063 RTC (I2C 0x51) -> system wall clock.
 * At boot: if the RTC holds a plausible time, set the system clock from it;
 * if it is unset (year < 2024, e.g. first power-up), seed it from the firmware
 * build time so the ravenous-boot rule has a clock from day one. The
 * progression save stamps clock_port_now_unix(); nothing else needs RTC. */
#include "rtc_port.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include <string.h>
#include <sys/time.h>
#include <time.h>

#define PCF85063_ADDR 0x51
#define REG_SECONDS   0x04     /* sec, min, hour, day, weekday, month, year (BCD) */
static const char *TAG = "rtc";
static i2c_master_dev_handle_t s_dev;

static int bcd2bin(uint8_t b) { return (b >> 4) * 10 + (b & 0x0f); }
static uint8_t bin2bcd(int v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

static bool rtc_read(struct tm *out) {
    uint8_t reg = REG_SECONDS, b[7];
    if (i2c_master_transmit_receive(s_dev, &reg, 1, b, 7, 100) != ESP_OK) return false;
    memset(out, 0, sizeof *out);
    out->tm_sec = bcd2bin(b[0] & 0x7f); out->tm_min = bcd2bin(b[1] & 0x7f);
    out->tm_hour = bcd2bin(b[2] & 0x3f); out->tm_mday = bcd2bin(b[3] & 0x3f);
    out->tm_wday = b[4] & 0x07; out->tm_mon = bcd2bin(b[5] & 0x1f) - 1;
    out->tm_year = bcd2bin(b[6]) + 100;                 /* 20xx */
    return !(b[0] & 0x80);                              /* OS flag set = clock invalid */
}
static bool rtc_write(const struct tm *t) {
    uint8_t w[8] = { REG_SECONDS, bin2bcd(t->tm_sec), bin2bcd(t->tm_min), bin2bcd(t->tm_hour),
                     bin2bcd(t->tm_mday), (uint8_t)t->tm_wday, bin2bcd(t->tm_mon + 1), bin2bcd(t->tm_year - 100) };
    return i2c_master_transmit(s_dev, w, sizeof w, 100) == ESP_OK;
}

bool rtc_port_init(i2c_master_bus_handle_t bus) {
    i2c_device_config_t cfg = { .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = PCF85063_ADDR, .scl_speed_hz = 400000 };
    if (!bus || i2c_master_bus_add_device(bus, &cfg, &s_dev) != ESP_OK) { ESP_LOGW(TAG, "no RTC"); return false; }
    struct tm t;
    if (rtc_read(&t) && t.tm_year + 1900 >= 2024) {
        struct timeval tv = { .tv_sec = mktime(&t) };
        settimeofday(&tv, NULL);
        ESP_LOGI(TAG, "system clock set from RTC: %04d-%02d-%02d %02d:%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min);
    } else {
        /* seed from build time: "Aug 21 2026" "10:15:00" */
        static const char mon[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
        char ms[4] = {0}; int d, y, hh, mm, ss;
        sscanf(__DATE__, "%3s %d %d", ms, &d, &y); sscanf(__TIME__, "%d:%d:%d", &hh, &mm, &ss);
        memset(&t, 0, sizeof t);
        t.tm_mon = (int)((strstr(mon, ms) - mon) / 3); t.tm_mday = d; t.tm_year = y - 1900;
        t.tm_hour = hh; t.tm_min = mm; t.tm_sec = ss;
        time_t secs = mktime(&t); struct timeval tv = { .tv_sec = secs }; settimeofday(&tv, NULL);
        rtc_write(&t);
        ESP_LOGW(TAG, "RTC was unset; seeded from build time %s %s", __DATE__, __TIME__);
    }
    return true;
}

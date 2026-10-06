/* battery_port_adc.c - Spotpear 1.54" muma: no PMIC. Battery level from
 * ADC1 channel 0 (GPIO1) through the board divider, charge detect on GPIO41
 * (high = charging), and GPIO3 as the power-hold / charge-LED line the
 * XiaoZhi firmware drives high while running and low at shutdown.
 * Level curve: the XiaoZhi board's own table (power_manager.h). */
#include "battery_port.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_adc/adc_oneshot.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"

#define PIN_CHG  41
#define PIN_HOLD 3

static const char *TAG = "battery";
static adc_oneshot_unit_handle_t s_adc;
static int s_avg = -1;
static float s_frac;
static int64_t s_last;

static const struct { int adc; int pct; } LV[] = {
    {1980, 0}, {2081, 20}, {2163, 40}, {2250, 60}, {2340, 80}, {2480, 100} };

static void sample(void) {
    int64_t now = esp_timer_get_time();
    if (s_avg >= 0 && now - s_last < 1000000) return;
    s_last = now;
    int v = 0;
    if (adc_oneshot_read(s_adc, ADC_CHANNEL_0, &v) != ESP_OK) return;
    s_avg = s_avg < 0 ? v : (s_avg * 7 + v) / 8;
    int a = s_avg;
    float pct;
    if (a <= LV[0].adc) pct = 0;
    else if (a >= LV[5].adc) pct = 100;
    else {
        pct = 0;
        for (int i = 0; i < 5; i++)
            if (a >= LV[i].adc && a < LV[i + 1].adc) {
                pct = LV[i].pct + (float)(a - LV[i].adc) / (LV[i + 1].adc - LV[i].adc) * (LV[i + 1].pct - LV[i].pct);
                break;
            }
    }
    s_frac = pct / 100.0f;
}

bool battery_port_init(i2c_master_bus_handle_t bus) {
    (void)bus;
    rtc_gpio_hold_dis(PIN_HOLD);
    rtc_gpio_init(PIN_HOLD);
    rtc_gpio_set_direction(PIN_HOLD, RTC_GPIO_MODE_OUTPUT_ONLY);
    rtc_gpio_set_level(PIN_HOLD, 1);
    gpio_config_t io = { .pin_bit_mask = 1ULL << PIN_CHG, .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE };
    gpio_config(&io);
    adc_oneshot_unit_init_cfg_t uc = { .unit_id = ADC_UNIT_1, .ulp_mode = ADC_ULP_MODE_DISABLE };
    if (adc_oneshot_new_unit(&uc, &s_adc) != ESP_OK) { ESP_LOGW(TAG, "ADC init failed"); return false; }
    adc_oneshot_chan_cfg_t cc = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_12 };
    adc_oneshot_config_channel(s_adc, ADC_CHANNEL_0, &cc);
    sample();
    ESP_LOGI(TAG, "ADC gauge: raw %d -> %d%%, %s", s_avg, (int)(s_frac * 100), gpio_get_level(PIN_CHG) ? "charging" : "on battery");
    return true;
}

bool battery_port_read(float *frac, bool *charging) {
    if (!s_adc) return false;
    sample();
    *frac = s_frac;
    *charging = gpio_get_level(PIN_CHG) == 1;
    return true;
}

int battery_port_state(void) {
    if (!gpio_get_level(PIN_CHG)) return BAT_ON_BATTERY;
    return s_frac >= 0.99f ? BAT_FULL : BAT_CHARGING;
}

/* the XiaoZhi shutdown: drop the hold line (and keep it dropped in deep
 * sleep). Returns false so the caller still enters deep sleep, BOOT wakes. */
bool battery_port_poweroff(void) {
    rtc_gpio_set_level(PIN_HOLD, 0);
    rtc_gpio_hold_en(PIN_HOLD);
    return false;
}
void battery_port_key_init(void) {}
int  battery_port_key_poll(void) { return 0; }
void battery_port_key_trace(int seconds) { (void)seconds; }
void battery_port_dump(void) { ESP_LOGI(TAG, "ADC avg %d -> %d%%", s_avg, (int)(s_frac * 100)); }
int  battery_port_vbat_mv(void) { return s_avg < 0 ? 0 : s_avg * 2 * 3300 / 4095; }   /* rough: 1:2 divider assumed */
bool battery_port_set_rail(const char *name, bool on) { (void)name; (void)on; return true; }
void battery_port_trim_rails(void) {}

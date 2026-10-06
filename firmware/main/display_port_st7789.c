/* display_port_st7789.c - Spotpear ESP32-S3 1.54" "muma" board:
 * ST7789 240x240 over SPI3, PWM backlight on GPIO42 (inverted), the shared
 * I2C bus (ES8311 codec) on SDA15/SCL14. Pins from the XiaoZhi board file
 * main/boards/spotpear/sp-esp32-s3-1.54-muma/config.h.
 *
 * The tank renders 448x368 landscape. Here it is scaled (2x2 box sample) to
 * 240x197 and centred, with black bars top and bottom. Bus mode pushes a
 * native 240x240 frame through display_port_flush_native. */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "driver/spi_master.h"
#include "driver/ledc.h"
#include "driver/i2c_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "display_port.h"
#include "tank.h"

#define LCD_W 240
#define LCD_H 240
#define PIN_SCLK 4
#define PIN_MOSI 2
#define PIN_CS   5
#define PIN_DC   47
#define PIN_RST  38
#define PIN_BL   42
#define PIN_SDA  15
#define PIN_SCL  14

#define OUT_H    197                       /* 368 * 240 / 448 */
#define OUT_Y0   ((LCD_H - OUT_H) / 2)     /* 21 */
#define CHUNK    24                        /* lines per DMA transfer */

static const char *TAG = "st7789";
static esp_lcd_panel_handle_t s_panel;
static esp_lcd_panel_io_handle_t s_io;
static i2c_master_bus_handle_t s_i2c;
static SemaphoreHandle_t s_free;           /* counts free line buffers */
static uint16_t *s_buf[2];
static int s_bi;
static uint8_t s_bright = 255;
static bool s_on;
static uint16_t s_xmap[LCD_W];             /* output x -> source x (448 wide) */
static uint16_t s_ymap[OUT_H];

i2c_master_bus_handle_t board_i2c_bus(void) { return s_i2c; }

static bool on_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *e, void *ctx) {
    BaseType_t hp = pdFALSE; xSemaphoreGiveFromISR(s_free, &hp); return hp == pdTRUE;
}

static void bl_apply(void) {
    uint32_t duty = s_on ? s_bright : 0;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 255 - duty);   /* inverted */
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

/* the two DMA line buffers come from internal RAM, which the model and the
 * render caches eat at boot: app_main reserves them before anything else */
void display_port_reserve(void) {
    for (int i = 0; i < 2; i++)
        if (!s_buf[i]) s_buf[i] = heap_caps_malloc(LCD_W * CHUNK * 2, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
}

bool display_port_init(void) {
    i2c_master_bus_config_t ic = { .i2c_port = I2C_NUM_0, .sda_io_num = PIN_SDA, .scl_io_num = PIN_SCL,
                                   .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7,
                                   .flags.enable_internal_pullup = 1 };
    if (i2c_new_master_bus(&ic, &s_i2c) != ESP_OK) { ESP_LOGW(TAG, "I2C bus failed"); s_i2c = NULL; }

    ledc_timer_config_t lt = { .speed_mode = LEDC_LOW_SPEED_MODE, .duty_resolution = LEDC_TIMER_8_BIT,
                               .timer_num = LEDC_TIMER_0, .freq_hz = 20000, .clk_cfg = LEDC_AUTO_CLK };
    ledc_timer_config(&lt);
    ledc_channel_config_t lc = { .gpio_num = PIN_BL, .speed_mode = LEDC_LOW_SPEED_MODE, .channel = LEDC_CHANNEL_0,
                                 .timer_sel = LEDC_TIMER_0, .duty = 255, .hpoint = 0 };
    ledc_channel_config(&lc);

    spi_bus_config_t bc = { .mosi_io_num = PIN_MOSI, .miso_io_num = -1, .sclk_io_num = PIN_SCLK,
                            .quadwp_io_num = -1, .quadhd_io_num = -1, .max_transfer_sz = LCD_W * CHUNK * 2 };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &bc, SPI_DMA_CH_AUTO));
    esp_lcd_panel_io_spi_config_t io = { .cs_gpio_num = PIN_CS, .dc_gpio_num = PIN_DC, .spi_mode = 0,
                                         .pclk_hz = 60 * 1000 * 1000, .trans_queue_depth = 4,
                                         .on_color_trans_done = on_done, .lcd_cmd_bits = 8, .lcd_param_bits = 8 };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io, &s_io));
    esp_lcd_panel_dev_config_t pc = { .reset_gpio_num = PIN_RST, .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB, .bits_per_pixel = 16 };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(s_io, &pc, &s_panel));
    esp_lcd_panel_reset(s_panel);
    esp_lcd_panel_init(s_panel);
    esp_lcd_panel_invert_color(s_panel, true);
    uint8_t bb = 0x38; esp_lcd_panel_io_tx_param(s_io, 0xBB, &bb, 1);

    s_free = xSemaphoreCreateCounting(2, 2);
    display_port_reserve();
    if (!s_buf[0] || !s_buf[1]) {
        ESP_LOGE(TAG, "no DMA line buffers (internal free %u) - display off", (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA));
        s_panel = NULL; return false;
    }
    for (int x = 0; x < LCD_W; x++) { int sx = x * TANK_W / LCD_W; s_xmap[x] = sx > TANK_W - 2 ? TANK_W - 2 : sx; }
    for (int y = 0; y < OUT_H; y++) { int sy = y * TANK_H / OUT_H; s_ymap[y] = sy > TANK_H - 2 ? TANK_H - 2 : sy; }

    /* clear to black, then light up */
    for (int y = 0; y < LCD_H; y += CHUNK) {
        xSemaphoreTake(s_free, portMAX_DELAY);
        uint16_t *b = s_buf[s_bi]; s_bi ^= 1;
        memset(b, 0, LCD_W * CHUNK * 2);
        esp_lcd_panel_draw_bitmap(s_panel, 0, y, LCD_W, y + CHUNK > LCD_H ? LCD_H : y + CHUNK, b);
    }
    esp_lcd_panel_disp_on_off(s_panel, true);
    s_on = true; bl_apply();
    ESP_LOGI(TAG, "ST7789 240x240 up; tank scaled to 240x%d", OUT_H);
    return true;
}

static inline uint16_t avg4(uint16_t a, uint16_t b, uint16_t c, uint16_t d) {
    uint32_t r = ((a >> 11) + (b >> 11) + (c >> 11) + (d >> 11)) >> 2;
    uint32_t g = (((a >> 5) & 63) + ((b >> 5) & 63) + ((c >> 5) & 63) + ((d >> 5) & 63)) >> 2;
    uint32_t bl = ((a & 31) + (b & 31) + (c & 31) + (d & 31)) >> 2;
    uint16_t v = (uint16_t)((r << 11) | (g << 5) | bl);
    return (uint16_t)((v << 8) | (v >> 8));               /* panel wants big-endian */
}

void display_port_flush(const uint16_t *fb) {
    if (!s_panel || !fb) return;
    for (int y0 = 0; y0 < LCD_H; y0 += CHUNK) {
        int n = y0 + CHUNK > LCD_H ? LCD_H - y0 : CHUNK;
        xSemaphoreTake(s_free, portMAX_DELAY);
        uint16_t *b = s_buf[s_bi]; s_bi ^= 1;
        for (int j = 0; j < n; j++) {
            int y = y0 + j - OUT_Y0;
            uint16_t *o = b + j * LCD_W;
            if (y < 0 || y >= OUT_H) { memset(o, 0, LCD_W * 2); continue; }
            const uint16_t *r0 = fb + s_ymap[y] * TANK_W, *r1 = r0 + TANK_W;
            for (int x = 0; x < LCD_W; x++) { int sx = s_xmap[x]; o[x] = avg4(r0[sx], r0[sx + 1], r1[sx], r1[sx + 1]); }
        }
        esp_lcd_panel_draw_bitmap(s_panel, 0, y0, LCD_W, y0 + n, b);
    }
}

void display_port_flush_native(const uint16_t *fb) {
    if (!s_panel || !fb) return;
    for (int y0 = 0; y0 < LCD_H; y0 += CHUNK) {
        int n = y0 + CHUNK > LCD_H ? LCD_H - y0 : CHUNK;
        xSemaphoreTake(s_free, portMAX_DELAY);
        uint16_t *b = s_buf[s_bi]; s_bi ^= 1;
        const uint16_t *src = fb + y0 * LCD_W;
        for (int i = 0; i < n * LCD_W; i++) { uint16_t v = src[i]; b[i] = (uint16_t)((v << 8) | (v >> 8)); }
        esp_lcd_panel_draw_bitmap(s_panel, 0, y0, LCD_W, y0 + n, b);
    }
}

void display_port_sleep(void) {
    if (!s_panel) return;
    s_on = false; bl_apply();
    esp_lcd_panel_disp_on_off(s_panel, false);
}
void display_port_wake(void) {
    if (!s_panel) return;
    esp_lcd_panel_disp_on_off(s_panel, true);
    s_on = true; bl_apply();
}
void display_port_set_inverted(bool inverted) { (void)inverted; }
void display_port_set_brightness(uint8_t level) { s_bright = level < 8 ? 8 : level; bl_apply(); }
uint8_t display_port_brightness(void) { return s_bright; }

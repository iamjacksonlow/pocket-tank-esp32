/* voice.c - offline voice commands (esp-sr v2: AFE + WakeNet9 "computer" +
 * MultiNet7 English), mic through the ES8311 (audio_port_read).
 *
 * Two tasks on core 1 (the fish brain's core, above its priority: speech is
 * real time, a fish decision can wait a beat):
 *   feed  : mic -> afe->feed
 *   detect: afe->fetch -> WakeNet; once woken, MultiNet until the window
 *           closes. Every command re-opens the window (VOICE_WINDOW_MS).
 * Commands go into a queue; the tank task takes them (voice_take). */
#include <string.h>
#include <ctype.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_afe_sr_models.h"
#include "esp_mn_models.h"
#include "esp_mn_iface.h"
#include "esp_mn_speech_commands.h"
#include "model_path.h"
#include "voice.h"
#include "audio_port.h"
#include "audio.h"
#include "sounds.h"
#include "render.h"

static const char *TAG = "voice";
#define VOICE_WINDOW_MS 8000

static const esp_afe_sr_iface_t *s_afe;
static esp_afe_sr_data_t *s_afe_data;
static esp_mn_iface_t *s_mn;
static model_iface_data_t *s_mn_data;
static QueueHandle_t s_q;
static volatile bool s_listening;
static volatile int64_t s_window_end_us, s_heard_us;
static volatile int s_heard;

static const struct { int id; const char *phrase; } PHRASES[] = {
    { VC_FEED, "feed the fish" }, { VC_FEED, "feed them" }, { VC_FEED, "dinner time" }, { VC_FEED, "feeding time" },
    { VC_LIGHT_ON, "lights on" }, { VC_LIGHT_ON, "turn on the light" }, { VC_LIGHT_ON, "turn the lights on" },
    { VC_LIGHT_OFF, "lights off" }, { VC_LIGHT_OFF, "turn off the light" }, { VC_LIGHT_OFF, "turn the lights off" },
    { VC_TAP, "tap the glass" }, { VC_TAP, "knock knock" },
    { VC_SLEEP, "go to sleep" }, { VC_SLEEP, "good night" },
    { VC_BUS, "show buses" }, { VC_BUS, "bus times" }, { VC_BUS, "bus mode" }, { VC_BUS, "when is my bus" },
    { VC_FISH, "show fish" }, { VC_FISH, "fish tank" }, { VC_FISH, "back to fish" }, { VC_FISH, "show the tank" },
    { VC_NEXT, "next" }, { VC_NEXT, "next stop" }, { VC_NEXT, "scroll" },
    { VC_PREV, "previous" }, { VC_PREV, "go back" }, { VC_PREV, "previous stop" },
    { VC_STOP, "stop" }, { VC_STOP, "stay" }, { VC_STOP, "hold on" },
    { VC_SUMMARY, "leave soon" }, { VC_SUMMARY, "summary" },
    { VC_CANCEL, "thank you" }, { VC_CANCEL, "never mind" }, { VC_CANCEL, "cancel" },
};
static const char *LABEL[VC_COUNT] = {
    "", "FEEDING", "LIGHTS ON", "LIGHTS OFF", "TAP TAP", "GOOD NIGHT",
    "BUS TIMES", "FISH TANK", "NEXT", "PREVIOUS", "STAYING HERE", "LEAVE SOON", "OK", "YES?",
};

static void feed_task(void *arg) {
    int chunk = s_afe->get_feed_chunksize(s_afe_data);
    int ch = s_afe->get_feed_channel_num(s_afe_data);
    int16_t *buf = heap_caps_malloc(chunk * ch * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!buf) buf = heap_caps_malloc(chunk * ch * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "feed: %d samples x %d ch", chunk, ch);
    for (;;) {
        int got = 0;
        while (got < chunk) {
            int n = audio_port_read(buf + got, chunk - got);
            if (n <= 0) { vTaskDelay(pdMS_TO_TICKS(100)); audio_port_prewarm(); continue; }   /* codec down (asleep): wait */
            got += n;
        }
        /* mic check: level every 3 s in the log (is the codec sending sound at all?) */
        { static int64_t last; static int64_t sumsq; static int peak, nsamp;
          for (int i = 0; i < chunk; i++) { int v = buf[i]; sumsq += (int64_t)v * v; if (v < 0) v = -v; if (v > peak) peak = v; }
          nsamp += chunk;
          int64_t now = esp_timer_get_time();
          if (now - last > 3000000) {
              int rms = 0; { int64_t m = nsamp ? sumsq / nsamp : 0; while ((int64_t)(rms + 1) * (rms + 1) <= m) rms += rms < 64 ? 1 : rms / 8; }
              ESP_LOGI(TAG, "mic level: rms %d peak %d (of 32767)%s", rms, peak, peak == 0 ? " - SILENT: no data from the codec" : "");
              last = now; sumsq = 0; peak = 0; nsamp = 0;
          } }
        s_afe->feed(s_afe_data, buf);
    }
}

static void open_window(void) {
    s_window_end_us = esp_timer_get_time() + VOICE_WINDOW_MS * 1000LL;
}

/* WakeNet "computer" runs inside the AFE (cheap). On a wake: chime, the
 * window opens and MultiNet matches commands until it closes. */
static void detect_task(void *arg) {
    for (;;) {
        afe_fetch_result_t *res = s_afe->fetch(s_afe_data);
        if (!res || res->ret_value == ESP_FAIL) continue;
        int64_t now = esp_timer_get_time();
        if (!s_listening) {
            if (res->wakeup_state == WAKENET_DETECTED) {
                s_listening = true; open_window();
                s_heard = VC_WAKE; s_heard_us = now;
                s_afe->disable_wakenet(s_afe_data);
                s_mn->clean(s_mn_data);
                audio_port_play(SND_CONFIRM, AUDIO_PITCH_ONE);          /* the chime */
                ESP_LOGI(TAG, "\"computer\" - listening");
            }
            continue;
        }
        esp_mn_state_t st = s_mn->detect(s_mn_data, res->data);
        if (st == ESP_MN_STATE_DETECTED) {
            esp_mn_results_t *r = s_mn->get_results(s_mn_data);
            int id = r->num > 0 ? r->command_id[0] : 0;
            ESP_LOGI(TAG, "heard \"%s\" (id %d, p=%.2f)", r->string, id, r->num ? r->prob[0] : 0);
            if (id > VC_NONE && id < VC_COUNT && id != VC_WAKE) {
                s_heard = id; s_heard_us = now;
                xQueueSend(s_q, &id, 0);
                audio_port_play(SND_CARD_OPEN, AUDIO_PITCH_ONE);
                if (id == VC_CANCEL || id == VC_SLEEP) s_window_end_us = now;
                else open_window();
            }
            s_mn->clean(s_mn_data);
        } else if (st == ESP_MN_STATE_TIMEOUT) {
            s_mn->clean(s_mn_data);
        }
        if (now > s_window_end_us) {
            s_listening = false;
            s_afe->enable_wakenet(s_afe_data);
            ESP_LOGI(TAG, "window closed - say \"computer\" again");
        }
    }
}

bool voice_init(void) {
    if (!audio_port_has_mic()) { ESP_LOGW(TAG, "no mic: voice off"); return false; }
    srmodel_list_t *models = esp_srmodel_init("model");
    if (!models || models->num == 0) { ESP_LOGW(TAG, "no speech models in the \"model\" partition: voice off"); return false; }
    for (int i = 0; i < models->num; i++) ESP_LOGI(TAG, "model: %s", models->model_name[i]);

    afe_config_t *cfg = afe_config_init("M", models, AFE_TYPE_SR, AFE_MODE_LOW_COST);
    if (!cfg) { ESP_LOGE(TAG, "AFE config failed"); return false; }
    cfg->aec_init = false;
    cfg->wakenet_init = true;
    cfg->se_init = false;
    cfg->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;
    cfg->afe_perferred_core = 1;
    cfg->afe_perferred_priority = 6;
    s_afe = esp_afe_handle_from_config(cfg);
    s_afe_data = s_afe ? s_afe->create_from_config(cfg) : NULL;
    afe_config_free(cfg);
    if (!s_afe_data) { ESP_LOGE(TAG, "AFE create failed"); return false; }

    char *mn_name = esp_srmodel_filter(models, ESP_MN_PREFIX, ESP_MN_ENGLISH);
    s_mn = mn_name ? esp_mn_handle_from_name(mn_name) : NULL;
    s_mn_data = s_mn ? s_mn->create(mn_name, 5000) : NULL;
    if (!s_mn_data) { ESP_LOGE(TAG, "MultiNet (English) failed"); return false; }
    esp_mn_commands_alloc(s_mn, s_mn_data);
    for (size_t i = 0; i < sizeof PHRASES / sizeof PHRASES[0]; i++) esp_mn_commands_add(PHRASES[i].id, PHRASES[i].phrase);
    esp_mn_error_t *err = esp_mn_commands_update();
    if (err && err->num) for (int i = 0; i < err->num; i++) ESP_LOGW(TAG, "phrase rejected: %s", err->phrases[i]->string);
    ESP_LOGI(TAG, "%s with %d phrases; fetch %d / mn %d samples", mn_name, (int)(sizeof PHRASES / sizeof PHRASES[0]),
             s_afe->get_fetch_chunksize(s_afe_data), s_mn->get_samp_chunksize(s_mn_data));

    s_q = xQueueCreate(8, sizeof(int));
    xTaskCreatePinnedToCore(feed_task, "vfeed", 4096, NULL, 7, NULL, 1);
    xTaskCreatePinnedToCore(detect_task, "vdetect", 8192, NULL, 5, NULL, 0);   /* core 0: core 1 has the AFE + the fish brain */
    ESP_LOGI(TAG, "ready: say \"computer\" | internal free %u KB, psram %u KB",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024, (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024);
    return true;
}

int voice_take(void) {
    int id = VC_NONE;
    if (s_q) xQueueReceive(s_q, &id, 0);
    return id;
}
bool voice_listening(void) { return s_listening; }
bool voice_busy(void) { return s_listening; }
const char *voice_last_label(int *age_ms) {
    if (age_ms) *age_ms = s_heard_us ? (int)((esp_timer_get_time() - s_heard_us) / 1000) : 1 << 30;
    return LABEL[s_heard];
}

/* a pill at the top centre: "LISTENING" with a pulsing dot while the window
 * is open, the heard command for 1.5 s after it lands */
void voice_overlay(uint16_t *fb, int stride, int w, int h, int scale) {
    int age; const char *heard = voice_last_label(&age);
    bool show_heard = age < 1500 && heard[0];
    if (!s_listening && !show_heard) return;
    const char *txt = show_heard ? heard : "LISTENING";
    int tw = render_text_w(txt, scale), dot = 4 * scale;
    int pw = tw + dot + 10 * scale, ph = 11 * scale;
    int x = (w - pw) / 2, y = 4 * scale;
    if (x < 0) x = 0;
    if (pw > w) pw = w;
    (void)h;
    render_rect_blend(fb, stride, x, y, pw, ph, show_heard ? 0x0f3a24 : 0x101c2c, 220);
    float t = (esp_timer_get_time() % 1000000) / 1e6f;
    uint32_t dc = show_heard ? 0x3ddc84 : (t < 0.5f ? 0xff5a5a : 0x7a2a2a);
    render_rect(fb, stride, x + 3 * scale, y + (ph - dot) / 2, dot, dot, dc);
    render_text(fb, stride, x + dot + 6 * scale, y + 2 * scale, scale, show_heard ? 0x7dffb0 : 0xe8f0ff, txt);
}

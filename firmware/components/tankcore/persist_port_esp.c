/* persist_port_esp.c — progression ports on the device: NVS blob + wall clock.
 * Wall clock: esp time (set from the PCF85063 RTC at boot in Track 4; until
 * then it is 0 on a cold boot, which simply disables the ravenous rule). */
#include "progression.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include <time.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "persist";

/* A save LONGER than this build's save_t is a newer build's (a rollback, or
 * the installer's older release): its head is our whole layout (tails only
 * ever append), so the head is loaded and the newer tail - things this build
 * never knew about - reads as its defaults when the newer build comes back.
 * A blob that exists but won't read blocks every save until a reset, so a
 * tank we could not load is never overwritten by a fresh one. */
static bool s_save_blocked;
bool persist_port_load(void *buf, size_t max, size_t *got) {
    nvs_handle_t h; if (nvs_open(SAVE_NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t len = 0; esp_err_t e = nvs_get_blob(h, SAVE_NVS_KEY, NULL, &len);   /* the stored length first */
    if (e == ESP_OK && len == 0) e = ESP_ERR_NVS_NOT_FOUND;         /* an empty blob is nothing saved */
    if (e == ESP_OK && len <= max) e = nvs_get_blob(h, SAVE_NVS_KEY, buf, &len);
    else if (e == ESP_OK) {                                /* a newer build's save: keep its head */
        uint8_t *all = malloc(len);
        if (!all) e = ESP_ERR_NO_MEM;
        else if ((e = nvs_get_blob(h, SAVE_NVS_KEY, all, &len)) == ESP_OK) {
            memcpy(buf, all, max);
            ESP_LOGW(TAG, "a newer build's save (%u bytes): loading the first %u", (unsigned)len, (unsigned)max);
            len = max;
        }
        free(all);
    }
    nvs_close(h);
    if (e == ESP_OK) *got = len;
    else if (e != ESP_ERR_NVS_NOT_FOUND) {
        s_save_blocked = true;
        ESP_LOGE(TAG, "load failed: %s (%u bytes) - saving is OFF so the stored tank stays", esp_err_to_name(e), (unsigned)len);
    }
    return e == ESP_OK;
}
bool persist_port_save(const void *buf, size_t len) {
    if (s_save_blocked) return false;
    nvs_handle_t h; if (nvs_open(SAVE_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t e = nvs_set_blob(h, SAVE_NVS_KEY, buf, len); if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h); if (e != ESP_OK) ESP_LOGW(TAG, "save failed: %s", esp_err_to_name(e));
    else ESP_LOGI(TAG, "tank saved (%u bytes)", (unsigned)len);
    return e == ESP_OK;
}
/* the keeper's reset: the whole "tank" namespace goes - "save" and the
 * director's parked "bk" alike - so nothing can bring the old tank back */
bool persist_port_erase(void) {
    nvs_handle_t h; if (nvs_open(SAVE_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t e = nvs_erase_all(h); if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    if (e != ESP_OK) ESP_LOGW(TAG, "erase failed: %s", esp_err_to_name(e));
    else { ESP_LOGI(TAG, "every saved tank erased"); s_save_blocked = false; }
    return e == ESP_OK;
}
int64_t clock_port_now_unix(void) {
    time_t now = time(NULL); return now > 1700000000 ? (int64_t)now : 0;   /* 0 until the RTC sets it */
}

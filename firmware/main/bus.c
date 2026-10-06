/* bus.c - Bus mode: live arrivals for the stops near home (listed in
 * user_config.h), from LTA DataMall when a key is set, else the keyless
 * arrivelah API (same LTA data): https://arrivelah2.busrouter.sg/?id=<stop code>
 * Wi-Fi is only on while Bus mode is up. With no saved network (or one that
 * won't join) the board opens an open access point "PocketTank-Setup" with a
 * captive page where the keeper picks the home Wi-Fi from a phone.
 *
 * Pages: 0 = LEAVE SOON (every stop, sorted by when to walk out the door),
 * then one page per stop. They cycle every PAGE_S seconds; the button's
 * double press jumps to the next one. */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <time.h>
#include <math.h>
#include "esp_heap_caps.h"
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_crt_bundle.h"
#include "esp_sntp.h"
#include "nvs.h"
#include "cJSON.h"
#include "lwip/sockets.h"
#include "bus.h"
#include "render.h"
#include "air_ui.h"
#include "ui240.h"
#if __has_include("user_config.h")
#include "user_config.h"
#else
#include "user_config.example.h"
#endif

static const char *TAG = "bus";

#define W 240
#define H 240
#define MAX_SVC   16
#define REFRESH_S 20
#define PAGE_S    120              /* pages move on by themselves every 2 minutes */
#define AIR_EVERY_S 600            /* air data: one quick Wi-Fi visit every 10 minutes */
#define AP_SSID   "PocketTank-Setup"

/* ---- the stops (from user_config.h; walk = minutes on foot from home) ---- */
typedef struct {
    char no[6];
    int  eta_s[3];          /* seconds at fetch time; -1 = none */
    char load[3];           /* 'G' seats, 'A' standing, 'R' full, 0 = n/a */
    bool dd, wab;
    char dest[6];
} svc_t;
typedef struct {
    const char *code, *name, *road;
    int walk_min;
    svc_t svc[MAX_SVC];
    int n;
    int64_t at_us;          /* when this data was fetched (0 = never) */
    bool ok;
} stop_t;
static stop_t S[] = { USER_STOPS };       /* { code, name, road, walk }: the rest starts empty */
#define N_STOPS ((int)(sizeof S / sizeof S[0]))
#define N_PAGES (N_STOPS + 1)

static const struct { const char *code, *name; } DEST[] = {
    {"02101","FLYER"}, {"10009","BT MERAH"}, {"10499","KG BAHRU"}, {"14009","HARBFRONT"},
    {"28009","JURONG E"}, {"52009","TOA PAYOH"}, {"52499","WHAMPOA"}, {"53009","BISHAN"},
    {"53239","BISHAN STN"}, {"54009","AMK"}, {"54189","MAYFLOWER"}, {"55509","YCK"},
    {"64009","HOUGANG"}, {"66009","SERANGOON"}, {"67009","SENGKANG"}, {"74009","TAMP NTH"},
    {"75009","TAMPINES"}, {"82009","EUNOS"}, {"84009","BEDOK"}, {"94009","E COAST"},
};
static const char *dest_name(const char *code) {
    for (size_t i = 0; i < sizeof DEST / sizeof DEST[0]; i++) if (!strcmp(DEST[i].code, code)) return DEST[i].name;
    return "";
}

/* ---- state ---- */
typedef enum { NET_OFF, NET_CONNECTING, NET_UP, NET_PORTAL, NET_FAILED } net_t;
static SemaphoreHandle_t s_mx;
static volatile bool s_active;
static volatile net_t s_net = NET_OFF;
static int s_page;
static int s_scroll_dir;          /* 0 = none, +1 / -1 = voice scrolling */
static bool s_pinned;             /* voice "stop": no auto-cycling */
static int64_t s_page_us;
static volatile bool s_want;      /* the fetch task wants the radio (BUS on, or AIR data due) */
/* AIR mode: data.gov.sg; the radio is up only while fetching */
static volatile bool s_air_active;
static air_data_t s_air;
static int64_t s_air_at_us, s_air_next_us;
static int s_air_page; static int64_t s_air_page_us;
static char s_ssid[33], s_pass[65];
/* LTA DataMall account key: the keeper's, overridable from the setup page.
 * With a working key the board asks LTA directly; otherwise arrivelah. */
static char s_ltakey[48] = USER_LTA_KEY;     /* "" = arrivelah; the setup page can store one in NVS */
static bool s_lta_bad;                    /* LTA refused the key this session: arrivelah only */
static bool s_have_creds, s_wifi_inited, s_sntp_on;
static TaskHandle_t s_fetch_task;
static httpd_handle_t s_httpd;
static int s_dns_sock = -1;
static TaskHandle_t s_dns_task;
static int s_retries;
static int64_t s_connect_start_us;
static char s_scan[8][33]; static int s_scan_n;

static void creds_load(void) {
    nvs_handle_t h; size_t l;
    s_have_creds = false;
    if (nvs_open("wifi", NVS_READONLY, &h) != ESP_OK) return;
    l = sizeof s_ssid; bool a = nvs_get_str(h, "ssid", s_ssid, &l) == ESP_OK;
    l = sizeof s_pass; if (nvs_get_str(h, "pass", s_pass, &l) != ESP_OK) s_pass[0] = 0;
    char k[48]; l = sizeof k; if (nvs_get_str(h, "lta", k, &l) == ESP_OK && k[0]) strlcpy(s_ltakey, k, sizeof s_ltakey);
    nvs_close(h);
    s_have_creds = a && s_ssid[0];
}
static void creds_save(const char *ssid, const char *pass) {
    nvs_handle_t h;
    if (nvs_open("wifi", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, "ssid", ssid); nvs_set_str(h, "pass", pass); nvs_commit(h); nvs_close(h);
    strlcpy(s_ssid, ssid, sizeof s_ssid); strlcpy(s_pass, pass, sizeof s_pass); s_have_creds = true;
}

/* ---- Wi-Fi ---- */
static void sta_connect(void) {
    wifi_config_t wc = { 0 };
    strlcpy((char *)wc.sta.ssid, s_ssid, sizeof wc.sta.ssid);
    strlcpy((char *)wc.sta.password, s_pass, sizeof wc.sta.password);
    wc.sta.threshold.authmode = s_pass[0] ? WIFI_AUTH_WEP : WIFI_AUTH_OPEN;
    esp_wifi_set_config(WIFI_IF_STA, &wc);
    s_net = NET_CONNECTING; s_retries = 0; s_connect_start_us = esp_timer_get_time();
    esp_wifi_connect();
    ESP_LOGI(TAG, "joining \"%s\"", s_ssid);
}

static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (!s_want || s_net == NET_PORTAL || !s_have_creds) return;
        if (s_net == NET_UP) s_net = NET_CONNECTING;
        if (++s_retries <= 8) { vTaskDelay(pdMS_TO_TICKS(500)); esp_wifi_connect(); }
        else { s_net = NET_FAILED; ESP_LOGW(TAG, "can't join \"%s\"", s_ssid); }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_net = NET_UP; s_retries = 0;
        ESP_LOGI(TAG, "online");
        if (!s_sntp_on) {
            esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
            esp_sntp_setservername(0, "pool.ntp.org");
            esp_sntp_init(); s_sntp_on = true;
        }
        if (s_fetch_task) xTaskNotifyGive(s_fetch_task);
    }
}

static void wifi_init_once(void) {
    if (s_wifi_inited) return;
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t e = esp_wifi_init(&cfg);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi init failed: %s (internal free %u KB)", esp_err_to_name(e), (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024);
        return;                                  /* s_wifi_inited stays false: radio_up shows the error */
    }
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi, NULL);
    setenv("TZ", "SGT-8", 1); tzset();
    s_wifi_inited = true;
}

/* ---- the setup portal: open AP + DNS that answers everything with us + one form ---- */
static void dns_task(void *a) {
    uint8_t buf[512];
    for (;;) {
        struct sockaddr_in from; socklen_t fl = sizeof from;
        int n = recvfrom(s_dns_sock, buf, sizeof buf - 16, 0, (struct sockaddr *)&from, &fl);
        if (n < 12) { if (s_dns_sock < 0) break; continue; }
        buf[2] = 0x81; buf[3] = 0x80;              /* response, no error */
        buf[6] = 0; buf[7] = 1;                    /* one answer */
        buf[8] = buf[9] = buf[10] = buf[11] = 0;
        int p = n;
        const uint8_t ans[] = { 0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, 30, 0, 4, 192, 168, 4, 1 };
        memcpy(buf + p, ans, sizeof ans); p += sizeof ans;
        sendto(s_dns_sock, buf, p, 0, (struct sockaddr *)&from, fl);
    }
    vTaskDelete(NULL);
}

static void url_decode(char *s) {
    char *o = s;
    for (; *s; s++) {
        if (*s == '+') *o++ = ' ';
        else if (*s == '%' && isxdigit((int)s[1]) && isxdigit((int)s[2])) { char h[3] = { s[1], s[2], 0 }; *o++ = (char)strtol(h, NULL, 16); s += 2; }
        else *o++ = *s;
    }
    *o = 0;
}

static esp_err_t page_get(httpd_req_t *r) {
    httpd_resp_set_type(r, "text/html");
    httpd_resp_sendstr_chunk(r,
        "<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
        "<title>Pocket Tank Wi-Fi</title><style>body{font:17px system-ui;margin:24px;max-width:420px}"
        "input,select,button{font:inherit;width:100%;padding:12px;margin:6px 0 16px;box-sizing:border-box;border-radius:10px;border:1px solid #bbb}"
        "button{background:#0a7;color:#fff;border:0}</style>"
        "<h2>Pocket Tank &middot; Wi-Fi</h2><p>Pick your home Wi-Fi so Bus mode can load arrival times.</p>"
        "<form method=post action=/save><label>Network</label><input name=s list=n autocomplete=off required><datalist id=n>");
    for (int i = 0; i < s_scan_n; i++) {
        char line[80]; snprintf(line, sizeof line, "<option value=\"%s\">", s_scan[i]);
        httpd_resp_sendstr_chunk(r, line);
    }
    httpd_resp_sendstr_chunk(r, "</datalist><label>Password</label><input name=p type=password>"
                                "<label>LTA DataMall key (optional)</label><input name=k autocomplete=off placeholder='leave empty to keep the current one'>"
                                "<button>Save &amp; connect</button></form>");
    return httpd_resp_sendstr_chunk(r, NULL);
}

static void portal_stop(void);
static esp_err_t page_save(httpd_req_t *r) {
    char body[320] = { 0 };
    int n = httpd_req_recv(r, body, sizeof body - 1);
    if (n <= 0) return ESP_FAIL;
    char ssid[33] = "", pass[65] = "";
    char *s = strstr(body, "s="), *p = strstr(body, "p=");
    if (s) { s += 2; char *e = strchr(s, '&'); size_t l = e ? (size_t)(e - s) : strlen(s); if (l > 96) l = 96; char t[100]; memcpy(t, s, l); t[l] = 0; url_decode(t); strlcpy(ssid, t, sizeof ssid); }
    if (p) { p += 2; char *e = strchr(p, '&'); size_t l = e ? (size_t)(e - p) : strlen(p); if (l > 192) l = 192; char t[200]; memcpy(t, p, l); t[l] = 0; url_decode(t); strlcpy(pass, t, sizeof pass); }
    char *k = strstr(body, "k=");
    if (k) { k += 2; char *e = strchr(k, '&'); size_t l = e ? (size_t)(e - k) : strlen(k); if (l > 60) l = 60; char t[64]; memcpy(t, k, l); t[l] = 0; url_decode(t);
             if (t[0]) { nvs_handle_t h; if (nvs_open("wifi", NVS_READWRITE, &h) == ESP_OK) { nvs_set_str(h, "lta", t); nvs_commit(h); nvs_close(h); }
                         strlcpy(s_ltakey, t, sizeof s_ltakey); s_lta_bad = false; } }
    if (!ssid[0]) return httpd_resp_sendstr(r, "Missing network name. Go back and try again.");
    creds_save(ssid, pass);
    httpd_resp_set_type(r, "text/html");
    httpd_resp_sendstr(r, "<meta name=viewport content='width=device-width'><body style='font:17px system-ui;margin:24px'>"
                          "<h2>Saved</h2><p>Pocket Tank is joining your Wi-Fi now. You can close this page.</p>");
    ESP_LOGI(TAG, "portal: saved \"%s\"", ssid);
    if (s_fetch_task) xTaskNotifyGive(s_fetch_task);   /* the fetch task leaves portal mode */
    return ESP_OK;
}
static esp_err_t redirect(httpd_req_t *r, httpd_err_code_t e) {
    httpd_resp_set_status(r, "302 Found");
    httpd_resp_set_hdr(r, "Location", "http://192.168.4.1/");
    return httpd_resp_send(r, NULL, 0);
}

static void portal_start(void) {
    if (s_net == NET_PORTAL) return;
    esp_wifi_set_mode(WIFI_MODE_APSTA);
    wifi_scan_config_t sc = { 0 };
    if (esp_wifi_scan_start(&sc, true) == ESP_OK) {
        uint16_t n = 16; wifi_ap_record_t rec[16];
        esp_wifi_scan_get_ap_records(&n, rec);
        s_scan_n = 0;
        for (int i = 0; i < n && s_scan_n < 8; i++) {
            if (!rec[i].ssid[0]) continue;
            bool dup = false; for (int j = 0; j < s_scan_n; j++) if (!strcmp(s_scan[j], (char *)rec[i].ssid)) dup = true;
            if (!dup) strlcpy(s_scan[s_scan_n++], (char *)rec[i].ssid, 33);
        }
    }
    wifi_config_t ap = { .ap = { .ssid = AP_SSID, .ssid_len = sizeof AP_SSID - 1, .channel = 1,
                                 .authmode = WIFI_AUTH_OPEN, .max_connection = 2 } };
    esp_wifi_set_config(WIFI_IF_AP, &ap);
    httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
    hc.lru_purge_enable = true; hc.max_open_sockets = 4;
    if (!s_httpd && httpd_start(&s_httpd, &hc) == ESP_OK) {
        httpd_uri_t g = { .uri = "/", .method = HTTP_GET, .handler = page_get };
        httpd_uri_t sv = { .uri = "/save", .method = HTTP_POST, .handler = page_save };
        httpd_register_uri_handler(s_httpd, &g);
        httpd_register_uri_handler(s_httpd, &sv);
        httpd_register_err_handler(s_httpd, HTTPD_404_NOT_FOUND, redirect);
    }
    if (s_dns_sock < 0) {
        s_dns_sock = socket(AF_INET, SOCK_DGRAM, 0);
        struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(53), .sin_addr.s_addr = htonl(INADDR_ANY) };
        bind(s_dns_sock, (struct sockaddr *)&a, sizeof a);
        xTaskCreatePinnedToCore(dns_task, "dns", 3072, NULL, 3, &s_dns_task, 0);
    }
    s_net = NET_PORTAL;
    ESP_LOGI(TAG, "setup portal up: join \"%s\"", AP_SSID);
}
static void portal_stop(void) {
    if (s_httpd) { httpd_stop(s_httpd); s_httpd = NULL; }
    if (s_dns_sock >= 0) { int s = s_dns_sock; s_dns_sock = -1; shutdown(s, 0); close(s); }
    esp_wifi_set_mode(WIFI_MODE_STA);
}

/* ---- fetching ---- */
static bool s_radio_on;
static void radio_up(void); static void radio_down(void);
static char load_code(const char *l) {
    if (!l) return 0;
    if (!strcmp(l, "SEA")) return 'G';
    if (!strcmp(l, "SDA")) return 'A';
    if (!strcmp(l, "LSD")) return 'R';
    return 0;
}

static int http_get(const char *url, const char *key, char *buf, int cap) {
    esp_http_client_config_t c = { .url = url, .crt_bundle_attach = esp_crt_bundle_attach, .timeout_ms = 8000 };
    esp_http_client_handle_t h = esp_http_client_init(&c);
    if (!h) return -1;
    if (key) { esp_http_client_set_header(h, "AccountKey", key); esp_http_client_set_header(h, "accept", "application/json"); }
    int status = -1, len = 0;
    if (esp_http_client_open(h, 0) == ESP_OK) {
        esp_http_client_fetch_headers(h);
        int r;
        while (len < cap - 1 && (r = esp_http_client_read(h, buf + len, cap - 1 - len)) > 0) len += r;
        buf[len] = 0;
        status = esp_http_client_get_status_code(h);
    }
    esp_http_client_cleanup(h);
    return len > 0 ? status : -1;
}

static void store(stop_t *st, const svc_t *tmp, int n) {
    xSemaphoreTake(s_mx, portMAX_DELAY);
    memcpy(st->svc, tmp, sizeof(svc_t) * n); st->n = n; st->at_us = esp_timer_get_time(); st->ok = true;
    xSemaphoreGive(s_mx);
}

/* "2026-10-04T12:01:23+08:00" -> seconds from now (TZ is SGT) */
static int secs_until(const char *iso) {
    struct tm tm = { 0 };
    if (!iso || sscanf(iso, "%d-%d-%dT%d:%d:%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday, &tm.tm_hour, &tm.tm_min, &tm.tm_sec) != 6) return -1;
    tm.tm_year -= 1900; tm.tm_mon -= 1; tm.tm_isdst = 0;
    int d = (int)(mktime(&tm) - time(NULL));
    return d < 0 ? 0 : d;
}

/* LTA DataMall v3: https://datamall2.mytransport.sg/ltaodataservice/v3/BusArrival */
static bool fetch_lta(stop_t *st, char *buf, int cap) {
    if (s_lta_bad || !s_ltakey[0] || time(NULL) < 1735689600) return false;   /* needs the SNTP clock */
    char url[100]; snprintf(url, sizeof url, "https://datamall2.mytransport.sg/ltaodataservice/v3/BusArrival?BusStopCode=%s", st->code);
    int status = http_get(url, s_ltakey, buf, cap);
    if (status == 401 || status == 403) { s_lta_bad = true; ESP_LOGW(TAG, "LTA refused the key (HTTP %d): using arrivelah", status); return false; }
    if (status != 200) { ESP_LOGW(TAG, "LTA stop %s: HTTP %d", st->code, status); return false; }
    cJSON *root = cJSON_Parse(buf);
    cJSON *arr = root ? cJSON_GetObjectItem(root, "Services") : NULL;
    if (!cJSON_IsArray(arr)) { cJSON_Delete(root); return false; }
    svc_t tmp[MAX_SVC]; int n = 0; cJSON *it;
    cJSON_ArrayForEach(it, arr) {
        if (n >= MAX_SVC) break;
        svc_t *s = &tmp[n]; memset(s, 0, sizeof *s);
        cJSON *no = cJSON_GetObjectItem(it, "ServiceNo");
        if (!cJSON_IsString(no)) continue;
        strlcpy(s->no, no->valuestring, sizeof s->no);
        const char *keys[3] = { "NextBus", "NextBus2", "NextBus3" };
        for (int k = 0; k < 3; k++) {
            cJSON *a = cJSON_GetObjectItem(it, keys[k]);
            cJSON *ea = a ? cJSON_GetObjectItem(a, "EstimatedArrival") : NULL;
            bool has = cJSON_IsString(ea) && ea->valuestring[0];
            s->eta_s[k] = has ? secs_until(ea->valuestring) : -1;
            if (!has) continue;
            cJSON *l = cJSON_GetObjectItem(a, "Load");
            s->load[k] = load_code(cJSON_IsString(l) ? l->valuestring : NULL);
            if (k == 0) {
                cJSON *ty = cJSON_GetObjectItem(a, "Type"), *fe = cJSON_GetObjectItem(a, "Feature"), *de = cJSON_GetObjectItem(a, "DestinationCode");
                s->dd = cJSON_IsString(ty) && !strcmp(ty->valuestring, "DD");
                s->wab = cJSON_IsString(fe) && !strcmp(fe->valuestring, "WAB");
                if (cJSON_IsString(de)) strlcpy(s->dest, de->valuestring, sizeof s->dest);
            }
        }
        n++;
    }
    cJSON_Delete(root);
    store(st, tmp, n);
    return true;
}

static bool fetch_stop(stop_t *st, char *buf, int cap) {
    if (fetch_lta(st, buf, cap)) return true;
    char url[80]; snprintf(url, sizeof url, "https://arrivelah2.busrouter.sg/?id=%s", st->code);
    if (http_get(url, NULL, buf, cap) != 200) return false;

    cJSON *root = cJSON_Parse(buf);
    if (!root) return false;
    cJSON *arr = cJSON_GetObjectItem(root, "services");
    svc_t tmp[MAX_SVC]; int n = 0;
    cJSON *it;
    cJSON_ArrayForEach(it, arr) {
        if (n >= MAX_SVC) break;
        svc_t *s = &tmp[n]; memset(s, 0, sizeof *s);
        cJSON *no = cJSON_GetObjectItem(it, "no");
        if (!cJSON_IsString(no)) continue;
        strlcpy(s->no, no->valuestring, sizeof s->no);
        const char *keys[3] = { "next", "subsequent", "next3" };
        for (int k = 0; k < 3; k++) {
            cJSON *a = cJSON_GetObjectItem(it, keys[k]);
            cJSON *d = a ? cJSON_GetObjectItem(a, "duration_ms") : NULL;
            s->eta_s[k] = cJSON_IsNumber(d) ? (int)(d->valuedouble / 1000) : -1;
            if (cJSON_IsNumber(d)) {
                cJSON *l = cJSON_GetObjectItem(a, "load");
                s->load[k] = load_code(cJSON_IsString(l) ? l->valuestring : NULL);
                if (k == 0) {
                    cJSON *ty = cJSON_GetObjectItem(a, "type"), *fe = cJSON_GetObjectItem(a, "feature"), *de = cJSON_GetObjectItem(a, "destination_code");
                    s->dd = cJSON_IsString(ty) && !strcmp(ty->valuestring, "DD");
                    s->wab = cJSON_IsString(fe) && !strcmp(fe->valuestring, "WAB");
                    if (cJSON_IsString(de)) strlcpy(s->dest, de->valuestring, sizeof s->dest);
                }
            }
        }
        n++;
    }
    cJSON_Delete(root);
    store(st, tmp, n);
    return true;
}

/* ---- AIR: data.gov.sg real-time APIs (no key) ---- */
static cJSON *get_json(const char *url, char *buf, int cap) {
    int st = http_get(url, NULL, buf, cap);
    if (st != 200) { ESP_LOGW(TAG, "air %s: HTTP %d", url, st); return NULL; }
    cJSON *r = cJSON_Parse(buf);
    if (!r) ESP_LOGW(TAG, "air %s: bad JSON (%d bytes)", url, (int)strlen(buf));
    return r;
}
static cJSON *path(cJSON *o, const char *a, const char *b, const char *c) {
    if (o && a) o = cJSON_GetObjectItem(o, a);
    if (o && b) o = cJSON_GetObjectItem(o, b);
    if (o && c) o = cJSON_GetObjectItem(o, c);
    return o;
}
static void regions(cJSON *readings, int out[AIR_REGIONS]) {
    static const char *k[AIR_REGIONS] = { "north", "south", "east", "west", "central" };
    for (int i = 0; i < AIR_REGIONS; i++) { cJSON *v = cJSON_GetObjectItem(readings, k[i]); if (cJSON_IsNumber(v)) out[i] = v->valueint; }
}
static void hhmm(const char *iso, char *out) {   /* "2026-10-05T18:00:00+08:00" -> "18:00" */
    const char *t = iso ? strchr(iso, 'T') : NULL;
    if (t && strlen(t) >= 6) { memcpy(out, t + 1, 5); out[5] = 0; }
}
/* 1-hr PM2.5 for the whole day (plus yesterday before 11 am), so the 1H page can
 * show the last 12 hours. slot = (0 yesterday | 24 today) + hour; latest wins. */
static int pm25_add_day(int slots[48], int base, const char *date, air_data_t *a, int *latest, char *buf, int cap) {
    char url[96]; snprintf(url, sizeof url, "https://api-open.data.gov.sg/v2/real-time/api/pm25?date=%s", date);
    cJSON *r = get_json(url, buf, cap), *it;
    if (!r) return 0;
    int n = 0;
    cJSON_ArrayForEach(it, path(r, "data", "items", NULL)) {
        cJSON *ts = cJSON_GetObjectItem(it, "timestamp"), *rd = path(it, "readings", "pm25_one_hourly", NULL);
        const char *t = cJSON_IsString(ts) ? strchr(ts->valuestring, 'T') : NULL;
        if (!t || !rd) continue;
        int hr = atoi(t + 1); if (hr < 0 || hr > 23) continue;
        cJSON *cv = cJSON_GetObjectItem(rd, "central");
        if (cJSON_IsNumber(cv)) slots[base + hr] = cv->valueint;
        if (base + hr > *latest) { *latest = base + hr; regions(rd, a->pm25_1h); hhmm(ts->valuestring, a->pm25_time); }
        n++;
    }
    cJSON_Delete(r);
    return n;
}
static bool fetch_pm25_day(air_data_t *a, char *buf, int cap) {
    time_t now = time(NULL);
    if (now < 1735689600) return false;                       /* no clock yet: can't name the day */
    int slots[48]; for (int i = 0; i < 48; i++) slots[i] = -1;
    int latest = -1; char d[12]; struct tm tm;
    localtime_r(&now, &tm); strftime(d, sizeof d, "%Y-%m-%d", &tm);
    if (!pm25_add_day(slots, 24, d, a, &latest, buf, cap)) return false;
    if (latest - 11 < 24) {                                   /* the window reaches back into yesterday */
        time_t y = now - 86400; localtime_r(&y, &tm); strftime(d, sizeof d, "%Y-%m-%d", &tm);
        int keep = latest; pm25_add_day(slots, 0, d, a, &latest, buf, cap); latest = keep;
    }
    if (latest < 24) return false;
    for (int i = 0; i < 12; i++) a->pm25_hist[i] = slots[latest - 11 + i];
    a->pm25_first_hour = (latest - 11) % 24;
    a->pm25_ok = true;
    return true;
}
static bool fetch_air(char *buf, int cap) {
    air_data_t a; xSemaphoreTake(s_mx, portMAX_DELAY); a = s_air; xSemaphoreGive(s_mx);
    bool got_psi = false;
    cJSON *r = get_json("https://api-open.data.gov.sg/v2/real-time/api/psi", buf, cap);
    if (r) {
        cJSON *it = cJSON_GetArrayItem(path(r, "data", "items", NULL), 0);
        cJSON *rd = path(it, "readings", "psi_twenty_four_hourly", NULL);
        if (rd) { regions(rd, a.psi24); got_psi = true; cJSON *ts = cJSON_GetObjectItem(it, "timestamp"); if (cJSON_IsString(ts)) hhmm(ts->valuestring, a.psi_time); }
        cJSON_Delete(r);
    }
    if (!fetch_pm25_day(&a, buf, cap) && (r = get_json("https://api-open.data.gov.sg/v2/real-time/api/pm25", buf, cap))) {
        cJSON *it = cJSON_GetArrayItem(path(r, "data", "items", NULL), 0);
        cJSON *rd = path(it, "readings", "pm25_one_hourly", NULL);
        if (rd) regions(rd, a.pm25_1h);
        a.pm25_ok = false;                                   /* no history this time: hide the stale chart */
        cJSON *ts = cJSON_GetObjectItem(it, "timestamp"); if (cJSON_IsString(ts)) hhmm(ts->valuestring, a.pm25_time);
        cJSON_Delete(r);
    }
    if ((r = get_json("https://api-open.data.gov.sg/v2/real-time/api/uv", buf, cap))) {
        cJSON *idx = path(cJSON_GetArrayItem(path(r, "data", "records", NULL), 0), "index", NULL, NULL), *e;
        const char *best = NULL;
        cJSON_ArrayForEach(e, idx) {                               /* the latest hour */
            cJSON *h = cJSON_GetObjectItem(e, "hour"), *v = cJSON_GetObjectItem(e, "value");
            if (cJSON_IsString(h) && cJSON_IsNumber(v) && (!best || strcmp(h->valuestring, best) > 0)) { best = h->valuestring; a.uv = v->valueint; hhmm(best, a.uv_time); }
        }
        cJSON_Delete(r);
    }
    if ((r = get_json("https://api-open.data.gov.sg/v2/real-time/api/two-hr-forecast", buf, cap))) {
        cJSON *it = cJSON_GetArrayItem(path(r, "data", "items", NULL), 0), *f;
        cJSON_ArrayForEach(f, cJSON_GetObjectItem(it, "forecasts")) {
            cJSON *ar = cJSON_GetObjectItem(f, "area"), *fc = cJSON_GetObjectItem(f, "forecast");
            if (cJSON_IsString(ar) && !strcmp(ar->valuestring, USER_FORECAST_AREA) && cJSON_IsString(fc)) strlcpy(a.rain, fc->valuestring, sizeof a.rain);
        }
        cJSON *vp = path(it, "valid_period", "text", NULL);
        if (cJSON_IsString(vp)) { const char *to = strstr(vp->valuestring, " to "); strlcpy(a.rain_until, to ? to + 4 : "", sizeof a.rain_until); }
        cJSON_Delete(r);
    }
    if ((r = get_json("https://api-open.data.gov.sg/v2/real-time/api/weather?api=wbgt", buf, cap))) {
        cJSON *rs = path(cJSON_GetArrayItem(path(r, "data", "records", NULL), 0), "item", "readings", NULL), *e;
        cJSON_ArrayForEach(e, rs) {
            cJSON *id = path(e, "station", "id", NULL);
            if (cJSON_IsString(id) && !strcmp(id->valuestring, USER_WBGT_STATION)) {
                cJSON *w = cJSON_GetObjectItem(e, "wbgt"), *hs = cJSON_GetObjectItem(e, "heatStress");
                if (cJSON_IsString(w)) a.wbgt = strtof(w->valuestring, NULL); else if (cJSON_IsNumber(w)) a.wbgt = (float)w->valuedouble;
                if (cJSON_IsString(hs)) strlcpy(a.heat, hs->valuestring, sizeof a.heat);
            }
        }
        cJSON_Delete(r);
    }
    if (got_psi) a.ok = true;
    xSemaphoreTake(s_mx, portMAX_DELAY); s_air = a; if (got_psi) s_air_at_us = esp_timer_get_time(); xSemaphoreGive(s_mx);
    ESP_LOGI(TAG, "air: PSI central %d (N %d S %d E %d W %d) at %s | PM2.5 %d at %s, 12h %s from %d:00 [%d %d %d %d %d %d %d %d %d %d %d %d] | UV %d | rain \"%s\" | heat %s %.1f",
             a.psi24[AIR_C], a.psi24[AIR_N], a.psi24[AIR_S], a.psi24[AIR_E], a.psi24[AIR_W], a.psi_time,
             a.pm25_1h[AIR_C], a.pm25_time, a.pm25_ok ? "ok" : "none", a.pm25_first_hour,
             a.pm25_hist[0], a.pm25_hist[1], a.pm25_hist[2], a.pm25_hist[3], a.pm25_hist[4], a.pm25_hist[5],
             a.pm25_hist[6], a.pm25_hist[7], a.pm25_hist[8], a.pm25_hist[9], a.pm25_hist[10], a.pm25_hist[11],
             a.uv, a.rain, a.heat, a.wbgt);
    return got_psi;
}
static bool air_due(void) { return s_air_active && esp_timer_get_time() >= s_air_next_us; }

static void fetch_task(void *a) {
    const int cap = 40 * 1024;
    char *buf = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
    for (;;) {
        s_want = s_active || air_due();
        if (!s_want) {
            radio_down();
            TickType_t wait = portMAX_DELAY;
            if (s_air_active) { int64_t ms = (s_air_next_us - esp_timer_get_time()) / 1000; wait = pdMS_TO_TICKS(ms < 100 ? 100 : ms); }
            ulTaskNotifyTake(pdTRUE, wait);
            continue;
        }
        if (!s_radio_on) radio_up();
        if (!s_wifi_inited) { ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5000)); continue; }
        if (s_net == NET_PORTAL) {
            if (s_have_creds) { portal_stop(); sta_connect(); }
            else { ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000)); continue; }
        }
        if (s_net == NET_FAILED || (s_net == NET_CONNECTING && esp_timer_get_time() - s_connect_start_us > 30LL * 1000000)) {
            ESP_LOGW(TAG, "no connection after 30 s: opening the setup portal too");
            esp_wifi_disconnect(); s_have_creds = false;   /* this session: wait for the form */
            portal_start();
            continue;
        }
        if (s_net != NET_UP) { ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(500)); continue; }
        if (air_due()) {
            if (time(NULL) < 1735689600) vTaskDelay(pdMS_TO_TICKS(1500));    /* let SNTP set the clock first (header time) */
            bool ok = fetch_air(buf, cap);
            s_air_next_us = esp_timer_get_time() + (ok ? AIR_EVERY_S : 60) * 1000000LL;
        }
        if (!s_active) continue;                                            /* AIR only: the loop top turns the radio off */
        int good = 0;
        for (int i = 0; i < N_STOPS && s_active && s_net == NET_UP; i++) {
            if (fetch_stop(&S[i], buf, cap)) good++;
            else ESP_LOGW(TAG, "stop %s: fetch failed", S[i].code);
        }
        ESP_LOGI(TAG, "refreshed %d/%d stops via %s | internal free %u KB", good, N_STOPS,
                 (!s_lta_bad && time(NULL) >= 1735689600) ? "LTA DataMall" : "arrivelah",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024);
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(REFRESH_S * 1000));
    }
}

/* ---- public ---- */
void bus_init(void) {
    s_mx = xSemaphoreCreateMutex();
    creds_load();
    xTaskCreatePinnedToCore(fetch_task, "busfetch", 10240, NULL, 3, &s_fetch_task, 0);
}
bool bus_active(void) { return s_active; }

/* enter/leave only flip the wish; the fetch task does the radio work, so a
 * button press never stalls the tank's frame loop */
void bus_enter(void) {
    if (s_active) return;
    if (!s_radio_on) { s_net = NET_CONNECTING; s_connect_start_us = esp_timer_get_time(); }
    s_active = true; s_page = 0; s_page_us = esp_timer_get_time(); s_scroll_dir = 0; s_pinned = false;
    xTaskNotifyGive(s_fetch_task);
    ESP_LOGI(TAG, "bus mode on");
}
void bus_leave(void) {
    if (!s_active) return;
    s_active = false;
    xTaskNotifyGive(s_fetch_task);
    ESP_LOGI(TAG, "bus mode off");
}
void bus_refresh(void) { s_page = 0; s_page_us = esp_timer_get_time(); s_scroll_dir = 0; s_pinned = false; if (s_fetch_task) xTaskNotifyGive(s_fetch_task); }

/* AIR mode */
void air_enter(void) {
    if (s_air_active) return;
    s_air_active = true; s_air_page = 0; s_air_page_us = esp_timer_get_time();
    if (s_air_at_us == 0 || esp_timer_get_time() - s_air_at_us > AIR_EVERY_S * 1000000LL) s_air_next_us = 0;   /* stale: fetch now */
    if (!s_radio_on && s_air_next_us == 0) { s_net = NET_CONNECTING; s_connect_start_us = esp_timer_get_time(); }
    xTaskNotifyGive(s_fetch_task);
    ESP_LOGI(TAG, "air mode on");
}
void air_leave(void) { if (!s_air_active) return; s_air_active = false; xTaskNotifyGive(s_fetch_task); ESP_LOGI(TAG, "air mode off"); }
bool air_active(void) { return s_air_active; }
void air_next_page(void) { s_air_page = (s_air_page + 1) % AIR_PAGES; s_air_page_us = esp_timer_get_time(); }
void air_refresh(void) { s_air_page = 0; s_air_page_us = esp_timer_get_time(); s_air_next_us = 0; if (s_fetch_task) xTaskNotifyGive(s_fetch_task); }
static void radio_up(void) {
    wifi_init_once();
    if (!s_wifi_inited) { strlcpy(s_ssid, "(WI-FI CHIP)", sizeof s_ssid); s_net = NET_FAILED; s_radio_on = false; return; }
    creds_load();
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_start();
    s_radio_on = true;
    if (s_have_creds) sta_connect(); else portal_start();
}
static void radio_down(void) {
    if (!s_radio_on) return;
    if (s_net == NET_PORTAL) portal_stop();
    esp_wifi_disconnect();
    esp_wifi_stop();
    s_radio_on = false; s_net = NET_OFF;
    ESP_LOGI(TAG, "Wi-Fi off");
}
/* paging: by default the pages cycle every PAGE_S. Voice "next"/"previous"
 * scrolls that way every SCROLL_S; "stop" pins the current page. */
#define SCROLL_S 3
static void step(int d) { s_page = (s_page + d + N_PAGES) % N_PAGES; s_page_us = esp_timer_get_time(); }
void bus_next_page(void) { step(+1); }
void bus_scroll(int dir) { s_pinned = false; s_scroll_dir = dir; step(dir); }
void bus_hold(void) { s_scroll_dir = 0; s_pinned = true; s_page_us = esp_timer_get_time(); }
void bus_goto(int page) { s_scroll_dir = 0; s_pinned = true; s_page = page % N_PAGES; s_page_us = esp_timer_get_time(); }

/* ---- drawing (240x240 RGB565) ---- */
#define C_BG     0x000000
#define C_TEXT   0xf2f2f2
#define C_MUTED  0x8a8a8a
#define C_DIM    0x4a4a4a
#define C_GO     0x3ddc84
#define C_SOON   0xffb020
#define C_RED    0xff5a5a
#define C_PILL   0x1f2a36
#define C_PILLTX 0x9cc8ff
#define C_LINE   0x1e1e1e

static uint16_t c565(uint32_t c) { return (uint16_t)(((c >> 8) & 0xf800) | ((c >> 5) & 0x07e0) | ((c >> 3) & 0x1f)); }
static void fill(uint16_t *fb, int x, int y, int w, int h, uint32_t c) {
    uint16_t v = c565(c);
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > W) w = W - x;
    if (y + h > H) h = H - y;
    for (int j = 0; j < h; j++) { uint16_t *p = fb + (y + j) * W + x; for (int i = 0; i < w; i++) p[i] = v; }
}
static void rfill(uint16_t *fb, int x, int y, int w, int h, int r, uint32_t c) {   /* rounded rect */
    for (int j = 0; j < h; j++) {
        int in = 0;
        if (j < r) in = r - (int)sqrtf((float)(r * r - (r - j) * (r - j)));
        else if (j >= h - r) { int d = j - (h - r - 1); in = r - (int)sqrtf((float)(r * r - d * d)); }
        fill(fb, x + in, y + j, w - 2 * in, 1, c);
    }
}
static void dot(uint16_t *fb, int cx, int cy, int r, uint32_t c) {
    for (int j = -r; j <= r; j++) for (int i = -r; i <= r; i++) if (i * i + j * j <= r * r) fill(fb, cx + i, cy + j, 1, 1, c);
}
static void text(uint16_t *fb, int x, int y, int sc, uint32_t c, const char *s) {
    if (x < 0 || y < 0 || y + 7 * sc > H) return;
    char t[40]; int i = 0;
    for (; s[i] && i < 39; i++) t[i] = (char)toupper((unsigned char)s[i]);
    t[i] = 0;
    while (i > 0 && x + render_text_w(t, sc) > W) t[--i] = 0;   /* clip on the right */
    render_text(fb, W, x, y, sc, c, t);
}
static void text_fit(uint16_t *fb, int x, int y, int sc, uint32_t c, const char *s, int maxw) {
    char t[40]; strlcpy(t, s, sizeof t);
    for (char *p = t; *p; p++) *p = (char)toupper((unsigned char)*p);
    int l = strlen(t);
    while (l > 0 && render_text_w(t, sc) > maxw) t[--l] = 0;
    while (l > 0 && t[l - 1] == ' ') t[--l] = 0;
    text(fb, x, y, sc, c, t);
}
static void text_r(uint16_t *fb, int xr, int y, int sc, uint32_t c, const char *s) { text(fb, xr - render_text_w(s, sc), y, sc, c, s); }
static void text_c(uint16_t *fb, int y, int sc, uint32_t c, const char *s) { text(fb, (W - render_text_w(s, sc)) / 2, y, sc, c, s); }

static uint32_t load_col(char l) { return l == 'G' ? C_GO : l == 'A' ? C_SOON : l == 'R' ? C_RED : C_DIM; }

static void pill(uint16_t *fb, int x, int y, const char *no, bool hot) {
    int tw = render_text_w(no, 2);
    int w = tw + 12 < 46 ? 46 : tw + 12;
    rfill(fb, x, y, w, 26, 6, hot ? 0x14361f : C_PILL);
    text(fb, x + (w - tw) / 2, y + 6, 2, hot ? 0x7dffb0 : C_PILLTX, no);
}

static void header(uint16_t *fb, const char *title, const char *sub) {
    text_fit(fb, 8, 8, 2, C_TEXT, title, 160);
    time_t now = time(NULL); struct tm tm; localtime_r(&now, &tm);
    if (tm.tm_year + 1900 >= 2025) { char c[8]; snprintf(c, sizeof c, "%d:%02d", tm.tm_hour, tm.tm_min); text_r(fb, W - 8, 8, 2, C_MUTED, c); }
    if (sub) text_fit(fb, 8, 27, 2, C_MUTED, sub, W - 16);
}

static void page_dots(uint16_t *fb) {
    int gap = 12, x0 = (W - (N_PAGES - 1) * gap) / 2;
    for (int i = 0; i < N_PAGES; i++) dot(fb, x0 + i * gap, H - 6, i == s_page ? 3 : 2, i == s_page ? C_TEXT : C_DIM);
}

static int eta_now(const stop_t *st, int s) { return s < 0 ? -1 : s - (int)((esp_timer_get_time() - st->at_us) / 1000000); }
static void fmt_min(char *o, size_t n, int sec) {
    if (sec < 0) snprintf(o, n, "-");
    else if (sec < 60) snprintf(o, n, "ARR");
    else snprintf(o, n, "%d", (sec / 60) % 1000);
}

static void draw_message(uint16_t *fb, const char *a, const char *b, const char *c, uint32_t col) {
    text_c(fb, 78, 3, col, a);
    if (b) text_c(fb, 116, 2, C_TEXT, b);
    if (c) text_c(fb, 140, 2, C_MUTED, c);
}

static void draw_stop(uint16_t *fb, const stop_t *st) {
    char sub[48]; snprintf(sub, sizeof sub, "%s  %d MIN WALK", st->code, st->walk_min);
    header(fb, st->name, sub);
    fill(fb, 8, 44, W - 16, 1, C_LINE);
    if (!st->ok) { draw_message(fb, "LOADING", NULL, NULL, C_MUTED); return; }
    /* sort by next arrival */
    int idx[MAX_SVC], n = st->n;
    for (int i = 0; i < n; i++) idx[i] = i;
    for (int i = 1; i < n; i++) for (int j = i; j > 0; j--) {
        int a = st->svc[idx[j - 1]].eta_s[0], b = st->svc[idx[j]].eta_s[0];
        if ((a < 0 && b >= 0) || (a >= 0 && b >= 0 && b < a)) { int t = idx[j]; idx[j] = idx[j - 1]; idx[j - 1] = t; } else break;
    }
    if (!n) { draw_message(fb, "NO BUSES", "NONE RUNNING NOW", NULL, C_MUTED); return; }
    int y = 48, rows = 0;
    for (int k = 0; k < n && rows < 5; k++, rows++) {
        const svc_t *s = &st->svc[idx[k]];
        int e0 = eta_now(st, s->eta_s[0]), e1 = eta_now(st, s->eta_s[1]);
        int leave = e0 < 0 ? -1 : e0 - st->walk_min * 60;
        bool catchable = e0 >= 0 && leave >= 0;
        pill(fb, 8, y + 3, s->no, catchable && leave < 180);
        const char *dn = dest_name(s->dest);
        text_fit(fb, 62, y + 2, 2, C_TEXT, dn[0] ? dn : "-", 112);
        dot(fb, 66, y + 25, 4, load_col(s->load[0]));
        char then[16];
        if (e1 >= 0) { char m[8]; fmt_min(m, sizeof m, e1); snprintf(then, sizeof then, "THEN %s", m); }
        else snprintf(then, sizeof then, "%s", s->dd ? "DOUBLE" : "");
        text(fb, 76, y + 19, 2, C_MUTED, then);
        char m[8]; fmt_min(m, sizeof m, e0);
        uint32_t col = e0 < 0 ? C_DIM : !catchable ? C_MUTED : leave < 180 ? C_GO : C_TEXT;
        text_r(fb, W - 8, y + 7, 3, col, m);
        fill(fb, 8, y + 35, W - 16, 1, C_LINE);
        y += 36;
    }
    (void)0;
}

static void draw_leave(uint16_t *fb) {
    header(fb, "LEAVE SOON", "MINUTES TO GO");
    fill(fb, 8, 44, W - 16, 1, C_LINE);
    typedef struct { int leave, eta; int stop; const svc_t *s; } cand_t;
    cand_t c[N_STOPS * MAX_SVC]; int n = 0; bool any = false;
    for (int i = 0; i < N_STOPS; i++) {
        if (!S[i].ok) continue;
        any = true;
        for (int k = 0; k < S[i].n; k++) {
            const svc_t *s = &S[i].svc[k];
            for (int a = 0; a < 2; a++) {            /* if the next one can't be caught, the one after may */
                int e = eta_now(&S[i], s->eta_s[a]);
                if (e < 0) continue;
                int l = e - S[i].walk_min * 60;
                if (l >= 0) { c[n++] = (cand_t){ l, e, i, s }; break; }
            }
        }
    }
    if (!any) { draw_message(fb, "LOADING", NULL, NULL, C_MUTED); return; }
    /* one row per service number (soonest stop wins), sorted by leave time */
    for (int i = 1; i < n; i++) for (int j = i; j > 0 && c[j].leave < c[j - 1].leave; j--) { cand_t t = c[j]; c[j] = c[j - 1]; c[j - 1] = t; }
    int y = 48, rows = 0;
    const char *seen[8]; int ns = 0;
    for (int k = 0; k < n && rows < 5; k++) {
        bool dup = false; for (int q = 0; q < ns; q++) if (!strcmp(seen[q], c[k].s->no)) dup = true;
        if (dup) continue;
        seen[ns++] = c[k].s->no; rows++;
        bool hot = c[k].leave < 180;
        pill(fb, 8, y + 3, c[k].s->no, hot);
        text_fit(fb, 62, y + 2, 2, C_TEXT, S[c[k].stop].name, 112);
        char sub[24]; char m[8]; fmt_min(m, sizeof m, c[k].eta);
        snprintf(sub, sizeof sub, c[k].eta < 60 ? "BUS ARR" : "BUS %s MIN", m);
        text(fb, 62, y + 19, 2, C_MUTED, sub);
        char lv[8]; if (c[k].leave < 60) snprintf(lv, sizeof lv, "NOW"); else snprintf(lv, sizeof lv, "%d", (c[k].leave / 60) % 1000);
        text_r(fb, W - 8, y + 7, 3, hot ? C_GO : C_TEXT, lv);
        fill(fb, 8, y + 35, W - 16, 1, C_LINE);
        y += 36;
    }
    if (!rows) draw_message(fb, "NO BUSES", "NONE YOU CAN CATCH", NULL, C_MUTED);
}

/* the portal / connecting / failed screens, shared by BUS and AIR; true = drawn */
static bool net_screen(uint16_t *fb, const char *title) {
    int64_t now = esp_timer_get_time();
    switch (s_net) {
    case NET_PORTAL:
        header(fb, "WI-FI SETUP", NULL);
        text_c(fb, 52, 2, C_MUTED, "ON YOUR PHONE, JOIN");
        rfill(fb, 14, 74, W - 28, 34, 8, C_PILL);
        text_c(fb, 84, 2, C_PILLTX, AP_SSID);
        text_c(fb, 122, 2, C_MUTED, "THEN PICK YOUR");
        text_c(fb, 142, 2, C_MUTED, "HOME WI-FI");
        text_c(fb, 176, 2, C_DIM, "OR OPEN 192.168.4.1");
        text_c(fb, 210, 2, C_DIM, "3 PRESSES: NEXT MODE");
        return true;
    case NET_CONNECTING:
    case NET_OFF:
        header(fb, title, NULL);
        draw_message(fb, "CONNECTING", s_ssid[0] ? s_ssid : NULL, "TO WI-FI", C_MUTED);
        { int k = (int)(now / 300000) % 3; for (int i = 0; i < 3; i++) dot(fb, W / 2 - 14 + i * 14, 170, 3, i == k ? C_TEXT : C_DIM); }
        return true;
    case NET_FAILED:
        header(fb, title, NULL);
        draw_message(fb, "NO WI-FI", "CAN'T JOIN", s_ssid, C_RED);
        return true;
    case NET_UP: break;
    }
    return false;
}

void air_render_screen(uint16_t *fb) {
    int64_t now = esp_timer_get_time();
    if (now - s_air_page_us > PAGE_S * 1000000LL) air_next_page();
    xSemaphoreTake(s_mx, portMAX_DELAY);
    air_data_t a = s_air; int64_t at = s_air_at_us;
    xSemaphoreGive(s_mx);
    if (!a.ok) {                                   /* nothing yet: show what the network is doing */
        for (int i = 0; i < W * H; i++) fb[i] = 0;
        if (net_screen(fb, "AIR")) return;
    }
    air_render(fb, &a, s_air_page);
    if (a.ok && now - at > 2 * 3600LL * 1000000) { fill(fb, 0, H - 40, W, 24, 0x2a1d00); text_c(fb, H - 35, 2, C_SOON, "OLD DATA, RETRYING"); }
}

void bus_render(uint16_t *fb, float clock) {
    (void)clock;
    for (int i = 0; i < W * H; i++) fb[i] = 0;
    int64_t now = esp_timer_get_time();
    if (s_net == NET_UP) {
        if (s_scroll_dir && now - s_page_us > SCROLL_S * 1000000LL) step(s_scroll_dir);
        else if (!s_scroll_dir && !s_pinned && now - s_page_us > PAGE_S * 1000000LL) step(+1);
    }
    if (net_screen(fb, "BUS")) return;
    xSemaphoreTake(s_mx, portMAX_DELAY);
    if (s_page == 0) draw_leave(fb); else draw_stop(fb, &S[s_page - 1]);
    /* stale-data warning */
    const stop_t *ref = s_page ? &S[s_page - 1] : &S[0];
    xSemaphoreGive(s_mx);
    if (ref->ok && now - ref->at_us > 90LL * 1000000) { fill(fb, 0, H - 40, W, 24, 0x2a1d00); text_c(fb, H - 35, 2, C_SOON, "OLD DATA, RETRYING"); }
    page_dots(fb);
}

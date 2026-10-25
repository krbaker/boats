// WeatherLink v2 API client: see weatherlink.h.

#include "weatherlink.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"

#include "https_lock.h"

static const char *TAG = "weatherlink";

#define BASE_URL "https://api.weatherlink.com/v2/"
#define RESP_MAX (32 * 1024) // a current-conditions response is about 5 KB

char *weatherlink_get(const char *path, const char *key, const char *secret, char *err, size_t errlen)
{
    char url[160];
    snprintf(url, sizeof(url), BASE_URL "%s?api-key=%s", path, key);
    esp_http_client_config_t cfg = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
        .user_agent = "ais-display/1.0",
    };
    https_lock();
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        https_unlock();
        snprintf(err, errlen, "out of memory");
        return NULL;
    }
    esp_http_client_set_header(c, "X-Api-Secret", secret);
    char *buf = NULL;
    if (esp_http_client_open(c, 0) != ESP_OK) {
        snprintf(err, errlen, "can't reach WeatherLink");
        goto out;
    }
    esp_http_client_fetch_headers(c);
    int status = esp_http_client_get_status_code(c);
    buf = heap_caps_malloc(RESP_MAX, MALLOC_CAP_SPIRAM);
    if (!buf) {
        snprintf(err, errlen, "out of memory");
        goto out;
    }
    int total = 0;
    for (;;) {
        int n = esp_http_client_read(c, buf + total, RESP_MAX - 1 - total);
        if (n <= 0) break;
        total += n;
        if (total >= RESP_MAX - 1) break;
    }
    buf[total] = '\0';
    if (status != 200) {
        char msg[64];
        if (status == 401) snprintf(err, errlen, "API key or secret rejected");
        else if (wl_parse_error(buf, msg, sizeof(msg))) snprintf(err, errlen, "%s", msg);
        else snprintf(err, errlen, "WeatherLink answered HTTP %d", status);
        free(buf);
        buf = NULL;
    }
out:
    esp_http_client_cleanup(c);
    https_unlock();
    return buf;
}

int weatherlink_list(const char *key, const char *secret, wl_station_info_t *out, int max, char *err, size_t errlen)
{
    char *body = weatherlink_get("stations", key, secret, err, errlen);
    if (!body) return -1;
    wl_station_t st[WL_MAX_STATIONS];
    int n = wl_parse_stations(body, st, max < WL_MAX_STATIONS ? max : WL_MAX_STATIONS);
    free(body);
    if (n < 0) {
        snprintf(err, errlen, "unexpected answer from WeatherLink");
        return -1;
    }
    for (int i = 0; i < n; i++) {
        out[i].station = st[i];
        char path[32], e[64];
        snprintf(path, sizeof(path), "current/%lu", (unsigned long)st[i].id);
        body = weatherlink_get(path, key, secret, e, sizeof(e));
        out[i].ntx = body ? wl_parse_transmitters(body, out[i].tx, WL_MAX_TX) : -1;
        if (!body) ESP_LOGW(TAG, "station %lu: %s", (unsigned long)st[i].id, e);
        free(body);
    }
    return n;
}

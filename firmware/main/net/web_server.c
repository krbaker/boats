#include "web_server.h"

#include <ctype.h>
#include <math.h>
#include <string.h>
#include <sys/param.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "cp_update.h"
#include "fw_update.h"
#include "map_store.h"
#include "nmea_client.h"
#include "rx_stats.h"
#include "sdr_probe.h"
#include "settings.h"
#include "tide.h"
#include "vessel_db.h"
#include "wifi_mgr.h"

static const char *TAG = "web";

extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[] asm("_binary_index_html_end");

static esp_err_t send_json(httpd_req_t *req, cJSON *root)
{
    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t r = httpd_resp_sendstr(req, s);
    free(s);
    return r;
}

static esp_err_t send_error(httpd_req_t *req, const char *status, const char *msg)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "error", msg);
    httpd_resp_set_status(req, status);
    return send_json(req, o);
}

// Read a small JSON request body.
static cJSON *read_json(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > 2048) return NULL;
    char buf[2049];
    int got = 0;
    while (got < req->content_len) {
        int n = httpd_req_recv(req, buf + got, req->content_len - got);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (n <= 0) return NULL;
        got += n;
    }
    buf[got] = '\0';
    return cJSON_Parse(buf);
}

static esp_err_t index_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, index_html_start, index_html_end - index_html_start - 1);
}

static esp_err_t config_get(httpd_req_t *req)
{
    settings_t cfg;
    settings_get(&cfg);
    char ssid[33], pass[65];
    settings_get_wifi(ssid, pass);

    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "ais_host", cfg.ais_host);
    cJSON_AddNumberToObject(o, "ais_port", cfg.ais_port);
    cJSON_AddStringToObject(o, "tide_station", cfg.tide_station);
    cJSON_AddStringToObject(o, "tz_name", cfg.tz_name);
    cJSON_AddStringToObject(o, "tz_posix", cfg.tz_posix);
    cJSON_AddNumberToObject(o, "center_lat", cfg.center_lat);
    cJSON_AddNumberToObject(o, "center_lon", cfg.center_lon);
    cJSON_AddNumberToObject(o, "brightness", cfg.brightness);
    cJSON_AddStringToObject(o, "wifi_ssid", ssid);
    cJSON_AddBoolToObject(o, "net_enabled", cfg.net_enabled);
    cJSON_AddBoolToObject(o, "sdr_enabled", cfg.sdr_enabled);
    cJSON_AddNumberToObject(o, "sdr_gain", cfg.sdr_gain);  // tenths of a dB, -1 tuner AGC, -2 best fixed
    cJSON_AddNumberToObject(o, "sdr_ppm", cfg.sdr_ppm_tenths / 10.0);
    cJSON_AddBoolToObject(o, "sdr_bias_tee", cfg.sdr_bias_tee);
    cJSON *fa = cJSON_AddArrayToObject(o, "retransmit");
    for (int i = 0; i < FWD_DEST_MAX; i++) {
        cJSON *d = cJSON_CreateObject();
        cJSON_AddBoolToObject(d, "enabled", cfg.fwd[i].enabled);
        cJSON_AddStringToObject(d, "proto", cfg.fwd[i].proto == FWD_TCP ? "tcp" : "udp");
        cJSON_AddStringToObject(d, "host", cfg.fwd[i].host);
        cJSON_AddNumberToObject(d, "port", cfg.fwd[i].port);
        cJSON_AddItemToArray(fa, d);
    }
    return send_json(req, o);
}

static bool get_str_field(cJSON *root, const char *key, char *dst, size_t cap, const char **err)
{
    cJSON *v = cJSON_GetObjectItem(root, key);
    if (!v) return true;
    if (!cJSON_IsString(v) || strlen(v->valuestring) >= cap) {
        *err = key;
        return false;
    }
    strcpy(dst, v->valuestring);
    return true;
}

// A host name or IPv4 address: letters, digits, dots and hyphens.
static bool valid_host(const char *h)
{
    for (; *h; h++) {
        if (!isalnum((unsigned char)*h) && *h != '.' && *h != '-') return false;
    }
    return true;
}

// One retransmit destination from the config page. On a bad value, writes
// why into msg and returns false.
static bool parse_fwd(cJSON *o, fwd_dest_t *d, int n, char *msg, size_t cap)
{
    if (!cJSON_IsObject(o)) {
        snprintf(msg, cap, "invalid retransmit destination %d", n);
        return false;
    }
    cJSON *v;
    if ((v = cJSON_GetObjectItem(o, "enabled"))) {
        if (!cJSON_IsBool(v)) goto bad;
        d->enabled = cJSON_IsTrue(v);
    }
    if ((v = cJSON_GetObjectItem(o, "proto"))) {
        if (cJSON_IsString(v) && strcmp(v->valuestring, "udp") == 0) d->proto = FWD_UDP;
        else if (cJSON_IsString(v) && strcmp(v->valuestring, "tcp") == 0) d->proto = FWD_TCP;
        else goto bad;
    }
    if ((v = cJSON_GetObjectItem(o, "host"))) {
        if (!cJSON_IsString(v) || strlen(v->valuestring) >= sizeof(d->host) || !valid_host(v->valuestring)) {
            snprintf(msg, cap, "destination %d: enter an IP address or host name", n);
            return false;
        }
        strcpy(d->host, v->valuestring);
    }
    if ((v = cJSON_GetObjectItem(o, "port"))) {
        if (!cJSON_IsNumber(v) || v->valueint < 0 || v->valueint > 65535) {
            snprintf(msg, cap, "destination %d: the port must be 1-65535", n);
            return false;
        }
        d->port = v->valueint;
    }
    if (d->enabled && (!d->host[0] || !d->port)) {
        snprintf(msg, cap, "destination %d: enter an address and port before turning it on", n);
        return false;
    }
    return true;
bad:
    snprintf(msg, cap, "invalid retransmit destination %d", n);
    return false;
}

static esp_err_t config_post(httpd_req_t *req)
{
    cJSON *root = read_json(req);
    if (!root) return send_error(req, "400 Bad Request", "invalid JSON");

    settings_t cfg;
    settings_get(&cfg);
    const char *bad = NULL;
    get_str_field(root, "ais_host", cfg.ais_host, sizeof(cfg.ais_host), &bad);
    get_str_field(root, "tide_station", cfg.tide_station, sizeof(cfg.tide_station), &bad);
    get_str_field(root, "tz_name", cfg.tz_name, sizeof(cfg.tz_name), &bad);
    get_str_field(root, "tz_posix", cfg.tz_posix, sizeof(cfg.tz_posix), &bad);

    cJSON *v;
    if ((v = cJSON_GetObjectItem(root, "ais_port"))) {
        if (!cJSON_IsNumber(v) || v->valueint < 0 || v->valueint > 65535) bad = "ais_port";
        else cfg.ais_port = v->valueint;
    }
    if ((v = cJSON_GetObjectItem(root, "center_lat"))) {
        if (!cJSON_IsNumber(v) || fabs(v->valuedouble) > 90) bad = "center_lat";
        else cfg.center_lat = v->valuedouble;
    }
    if ((v = cJSON_GetObjectItem(root, "center_lon"))) {
        if (!cJSON_IsNumber(v) || fabs(v->valuedouble) > 180) bad = "center_lon";
        else cfg.center_lon = v->valuedouble;
    }
    if ((v = cJSON_GetObjectItem(root, "brightness"))) {
        if (!cJSON_IsNumber(v) || v->valueint < 5 || v->valueint > 100) bad = "brightness";
        else cfg.brightness = v->valueint;
    }
    if ((v = cJSON_GetObjectItem(root, "net_enabled"))) {
        if (!cJSON_IsBool(v)) bad = "net_enabled";
        else cfg.net_enabled = cJSON_IsTrue(v);
    }
    if ((v = cJSON_GetObjectItem(root, "sdr_enabled"))) {
        if (!cJSON_IsBool(v)) bad = "sdr_enabled";
        else cfg.sdr_enabled = cJSON_IsTrue(v);
    }
    if ((v = cJSON_GetObjectItem(root, "sdr_bias_tee"))) {
        if (!cJSON_IsBool(v)) bad = "sdr_bias_tee";
        else cfg.sdr_bias_tee = cJSON_IsTrue(v);
    }
    if ((v = cJSON_GetObjectItem(root, "sdr_gain"))) {
        if (!cJSON_IsNumber(v) || v->valueint < SDR_GAIN_BEST || v->valueint > 500) bad = "sdr_gain";
        else cfg.sdr_gain = v->valueint;
    }
    if ((v = cJSON_GetObjectItem(root, "sdr_ppm"))) {
        if (!cJSON_IsNumber(v) || fabs(v->valuedouble) > 200) bad = "sdr_ppm";
        else cfg.sdr_ppm_tenths = (int16_t)lround(v->valuedouble * 10);
    }
    for (const char *c = cfg.tide_station; *c; c++) {
        if (*c < '0' || *c > '9') bad = "tide_station";
    }
    char msg[80] = "";
    if ((v = cJSON_GetObjectItem(root, "retransmit"))) {
        if (!cJSON_IsArray(v) || cJSON_GetArraySize(v) > FWD_DEST_MAX) {
            bad = "retransmit";
        } else {
            for (int i = 0; i < cJSON_GetArraySize(v) && !msg[0]; i++) {
                parse_fwd(cJSON_GetArrayItem(v, i), &cfg.fwd[i], i + 1, msg, sizeof(msg));
            }
        }
    }
    cJSON_Delete(root);

    if (bad) snprintf(msg, sizeof(msg), "invalid value for %s", bad);
    if (msg[0]) return send_error(req, "400 Bad Request", msg);
    settings_set(&cfg);
    return config_get(req);
}

static esp_err_t wifi_post(httpd_req_t *req)
{
    cJSON *root = read_json(req);
    cJSON *ssid = cJSON_GetObjectItem(root, "ssid"), *pass = cJSON_GetObjectItem(root, "password");
    if (!cJSON_IsString(ssid) || !ssid->valuestring[0] || strlen(ssid->valuestring) > 32 ||
        (pass && (!cJSON_IsString(pass) || strlen(pass->valuestring) > 64 ||
                  (pass->valuestring[0] && strlen(pass->valuestring) < 8)))) {
        cJSON_Delete(root);
        return send_error(req, "400 Bad Request", "SSID must be 1-32 characters and password empty or 8-64 characters");
    }
    wifi_mgr_set_credentials(ssid->valuestring, pass ? pass->valuestring : "");
    cJSON_Delete(root);
    httpd_resp_set_status(req, "202 Accepted");
    return httpd_resp_sendstr(req, "{}");
}

static const char *test_str(wifi_test_result_t t)
{
    switch (t) {
    case WIFI_TEST_RUNNING: return "testing";
    case WIFI_TEST_OK: return "ok";
    case WIFI_TEST_FAILED_REVERTED: return "failed_reverted";
    case WIFI_TEST_FAILED_AP: return "failed_ap";
    default: return "none";
    }
}

static esp_err_t status_get(httpd_req_t *req)
{
    cJSON *o = cJSON_CreateObject();
    int64_t now_us = esp_timer_get_time();

    wifi_mgr_status_t w;
    wifi_mgr_get_status(&w);
    cJSON *wj = cJSON_AddObjectToObject(o, "wifi");
    cJSON_AddStringToObject(wj, "state", wifi_mgr_state_str(w.state));
    cJSON_AddStringToObject(wj, "ssid", w.sta_ssid);
    cJSON_AddStringToObject(wj, "ip", w.sta_ip);
    cJSON_AddNumberToObject(wj, "rssi", w.rssi);
    cJSON_AddBoolToObject(wj, "ap_active", w.ap_active);
    cJSON_AddStringToObject(wj, "ap_ssid", w.ap_ssid);
    cJSON_AddStringToObject(wj, "test", test_str(w.test));
    cJSON_AddStringToObject(wj, "test_ssid", w.test_ssid);
    char cpv[24];
    cp_update_version(cpv, sizeof(cpv));
    cJSON_AddStringToObject(wj, "coprocessor", cpv);

    nmea_status_t n;
    nmea_client_get_status(&n);
    cJSON *aj = cJSON_AddObjectToObject(o, "ais");
    cJSON_AddStringToObject(aj, "state", nmea_state_str(n.state));
    cJSON_AddStringToObject(aj, "error", n.last_error);
    cJSON_AddNumberToObject(aj, "lines", n.stats.lines);
    cJSON_AddNumberToObject(aj, "reports", n.stats.reports);
    cJSON_AddNumberToObject(aj, "ignored", n.stats.ignored);
    cJSON_AddNumberToObject(aj, "bad_checksum", n.stats.bad_checksum);
    cJSON_AddNumberToObject(aj, "unsupported", n.stats.unsupported);
    cJSON_AddNumberToObject(aj, "last_rx_s", n.last_rx_us ? (now_us - n.last_rx_us) / 1000000 : -1);
    cJSON_AddNumberToObject(o, "vessels", vessel_db_count());

    vc_stats_t vc;
    cJSON *cachej = cJSON_AddObjectToObject(o, "vessel_cache");
    if (!vessel_db_cache_stats(&vc)) {
        cJSON_AddStringToObject(cachej, "state", "loading");
    } else {
        cJSON_AddStringToObject(cachej, "state", vc.ram_only ? "no card" : vc.last_error[0] ? "error" : "ok");
        cJSON_AddNumberToObject(cachej, "vessels", vc.count);
        cJSON_AddNumberToObject(cachej, "capacity", vc.capacity);
        cJSON_AddNumberToObject(cachej, "file_bytes", vc.file_records * VC_RECORD_SIZE);
        cJSON_AddNumberToObject(cachej, "writes", vc.records_written);
        cJSON_AddNumberToObject(cachej, "compactions", vc.compactions);
        cJSON_AddStringToObject(cachej, "error", vc.last_error);
    }

    tide_status_t t;
    tide_get_status(&t);
    static const char *tide_states[] = {"no station", "waiting", "ok", "error"};
    cJSON *tj = cJSON_AddObjectToObject(o, "tide");
    cJSON_AddStringToObject(tj, "state", tide_states[t.state]);
    cJSON_AddStringToObject(tj, "station", t.station);
    cJSON_AddStringToObject(tj, "name", t.station_name);
    cJSON_AddStringToObject(tj, "error", t.error);
    tide_event_t next;
    if (tide_next(time(NULL), &next)) {
        cJSON *nj = cJSON_AddObjectToObject(tj, "next");
        cJSON_AddNumberToObject(nj, "time", (double)next.t);
        cJSON_AddBoolToObject(nj, "high", next.high);
        cJSON_AddNumberToObject(nj, "height_ft", next.height_ft);
    }

    map_manifest_t m;
    cJSON *mj = cJSON_AddObjectToObject(o, "map");
    if (map_store_get_manifest(&m)) {
        cJSON_AddNumberToObject(mj, "center_lat", m.center_lat);
        cJSON_AddNumberToObject(mj, "center_lon", m.center_lon);
        cJSON *levels = cJSON_AddArrayToObject(mj, "radii_nm");
        for (int i = 0; i < m.nlevels; i++) cJSON_AddItemToArray(levels, cJSON_CreateNumber(m.levels[i].radius_nm));
    }
    uint64_t total, free_bytes;
    map_store_usage(&total, &free_bytes);
    cJSON_AddBoolToObject(mj, "card", map_store_card_ok());
    cJSON_AddNumberToObject(mj, "storage_free", (double)free_bytes);
    cJSON_AddNumberToObject(mj, "storage_total", (double)total);

    sdr_status_t r;
    sdr_get_status(&r);
    cJSON *rj = cJSON_AddObjectToObject(o, "sdr");
    cJSON_AddStringToObject(rj, "state", sdr_state_str(r.state));
    cJSON_AddStringToObject(rj, "tuner", r.tuner);
    cJSON_AddNumberToObject(rj, "center_hz", r.center_hz);
    cJSON_AddNumberToObject(rj, "rate", r.rate);
    cJSON *cj = cJSON_AddObjectToObject(rj, "capture");
    cJSON_AddNumberToObject(cj, "seconds", r.cap_seconds);
    cJSON_AddNumberToObject(cj, "bytes", (double)r.cap_bytes);
    cJSON_AddNumberToObject(cj, "total", (double)r.cap_total);
    cJSON_AddNumberToObject(cj, "dropped", (double)r.cap_dropped);
    cJSON_AddNumberToObject(cj, "file_bytes", (double)r.cap_file_bytes);
    cJSON_AddStringToObject(cj, "error", r.cap_error);
    cJSON_AddNumberToObject(cj, "log_lines", r.cap_log_lines);
    cJSON_AddNumberToObject(rj, "cpu_pct", r.cpu_pct);
    cJSON_AddNumberToObject(rj, "dropped_bytes", (double)r.dropped_bytes);
    cJSON_AddNumberToObject(rj, "bad_crc", r.demod.bad_crc);
    cJSON_AddNumberToObject(rj, "noise_dbfs", (r.demod.noise_dbfs[0] + r.demod.noise_dbfs[1]) / 2);
    cJSON_AddNumberToObject(rj, "ppm", r.demod.ppm);
    cJSON *gj = cJSON_AddObjectToObject(rj, "gain");
    cJSON_AddNumberToObject(gj, "now", r.gain);
    cJSON_AddBoolToObject(gj, "best", r.gain_best);
    cJSON_AddNumberToObject(gj, "steps_down", r.gain_steps_down);
    cJSON_AddNumberToObject(gj, "sweep_step", r.sweep_step);
    cJSON_AddNumberToObject(gj, "sweep_steps", r.sweep_steps);

    // Packets per source and channel, and which source each vessel came from.
    rx_stats_t rx;
    rx_stats_get(&rx);
    cJSON *xj = cJSON_AddObjectToObject(o, "receivers");
    static const char *const src_names[RX_NSRC] = {"network", "sdr"};
    for (int i = 0; i < RX_NSRC; i++) {
        cJSON *sj2 = cJSON_AddObjectToObject(xj, src_names[i]);
        cJSON_AddNumberToObject(sj2, "a", rx.total[i][0]);
        cJSON_AddNumberToObject(sj2, "b", rx.total[i][1]);
        cJSON_AddNumberToObject(sj2, "a_min", rx.last_min[i][0]);
        cJSON_AddNumberToObject(sj2, "b_min", rx.last_min[i][1]);
    }
    int net_only, sdr_only, both;
    vessel_db_source_counts(&net_only, &sdr_only, &both);
    cJSON *vj = cJSON_AddObjectToObject(xj, "vessels");
    cJSON_AddNumberToObject(vj, "network_only", net_only);
    cJSON_AddNumberToObject(vj, "sdr_only", sdr_only);
    cJSON_AddNumberToObject(vj, "both", both);

    fw_info_t fw;
    fw_update_info(&fw);
    cJSON *fj = cJSON_AddObjectToObject(o, "firmware");
    cJSON_AddStringToObject(fj, "version", fw.version);
    cJSON_AddStringToObject(fj, "built", fw.build_date);
    cJSON_AddStringToObject(fj, "slot", fw.running);
    cJSON_AddBoolToObject(fj, "on_trial", fw.pending_verify);
    cJSON_AddBoolToObject(fj, "rolled_back", fw.rolled_back);
    cJSON_AddBoolToObject(fj, "can_update", fw.can_update);

    cJSON *sj = cJSON_AddObjectToObject(o, "system");
    cJSON_AddNumberToObject(sj, "uptime_s", now_us / 1000000);
    cJSON_AddNumberToObject(sj, "time", clock_is_set() ? (double)time(NULL) : 0);
    cJSON_AddNumberToObject(sj, "heap_internal", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(sj, "heap_psram", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    return send_json(req, o);
}

static esp_err_t vessels_get(httpd_req_t *req)
{
    vessel_t *list = heap_caps_malloc(VESSEL_MAX * sizeof(vessel_t), MALLOC_CAP_SPIRAM);
    if (!list) return send_error(req, "500 Internal Server Error", "out of memory");
    int n = vessel_db_snapshot(list, VESSEL_MAX);
    int64_t now = esp_timer_get_time();
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < n; i++) {
        vessel_t *v = &list[i];
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "mmsi", v->mmsi);
        cJSON_AddStringToObject(o, "name", v->name);
        cJSON_AddStringToObject(o, "class", ais_class_str(v->cls));
        if (v->fields & AIS_F_POS) {
            cJSON_AddNumberToObject(o, "lat", v->lat);
            cJSON_AddNumberToObject(o, "lon", v->lon);
        }
        if (v->fields & AIS_F_SOG) cJSON_AddNumberToObject(o, "sog", v->sog_kn);
        if (v->fields & AIS_F_COG) cJSON_AddNumberToObject(o, "cog", v->cog_deg);
        if (v->fields & AIS_F_HEADING) cJSON_AddNumberToObject(o, "heading", v->heading);
        cJSON_AddStringToObject(o, "type", ais_ship_type_str(v->ship_type));
        cJSON_AddNumberToObject(o, "age_s", (now - v->last_heard_us) / 1000000);
        cJSON_AddNumberToObject(o, "msgs_network", v->msgs[RX_NET]);
        cJSON_AddNumberToObject(o, "msgs_sdr", v->msgs[RX_SDR]);
        cJSON_AddItemToArray(arr, o);
    }
    free(list);
    return send_json(req, arr);
}

#define UPLOAD_CHUNK (16 * 1024)

static esp_err_t map_post(httpd_req_t *req)
{
    if (!map_store_card_ok()) {
        return send_error(req, "400 Bad Request", "no microSD card in the display (insert one and restart it)");
    }
    uint64_t total, free_bytes;
    map_store_usage(&total, &free_bytes);
    if (req->content_len <= 0) return send_error(req, "400 Bad Request", "upload map_set.zip");
    if (req->content_len + (1 << 20) > free_bytes) {
        return send_error(req, "400 Bad Request", "not enough free space on the microSD card");
    }
    char msg[200] = "";
    map_upload_t *u = map_upload_begin(msg, sizeof(msg));
    if (!u) return send_error(req, "500 Internal Server Error", msg);

    char *buf = malloc(UPLOAD_CHUNK);
    int remaining = req->content_len;
    esp_err_t err = ESP_OK;
    while (remaining > 0) {
        int n = httpd_req_recv(req, buf, MIN(remaining, UPLOAD_CHUNK));
        if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (n <= 0) {
            err = ESP_FAIL;
            break;
        }
        remaining -= n;
        if (map_upload_feed(u, (uint8_t *)buf, n) != ESP_OK) {
            // Keep draining so the browser sees our error rather than a reset.
            while (remaining > 0) {
                n = httpd_req_recv(req, buf, MIN(remaining, UPLOAD_CHUNK));
                if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
                if (n <= 0) break;
                remaining -= n;
            }
            break;
        }
    }
    free(buf);
    if (err != ESP_OK) {
        map_upload_abort(u);
        return ESP_FAIL; // connection is gone
    }

    if (map_upload_finish(u, msg, sizeof(msg)) != ESP_OK) {
        ESP_LOGW(TAG, "map upload rejected: %s", msg);
        return send_error(req, "400 Bad Request", msg);
    }
    return config_get(req);
}

// Display firmware update: the body is firmware/build/ais_display.bin.
static esp_err_t firmware_post(httpd_req_t *req)
{
    char msg[128] = "";
    if (req->content_len <= 0) return send_error(req, "400 Bad Request", "upload firmware/build/ais_display.bin");
    fw_update_t *u = fw_update_begin(req->content_len, msg, sizeof(msg));
    if (!u) return send_error(req, "400 Bad Request", msg);
    char *buf = malloc(4096);
    if (!buf) {
        fw_update_finish(u, msg, sizeof(msg));
        return send_error(req, "500 Internal Server Error", "out of memory");
    }
    int remaining = req->content_len;
    bool ok = true;
    while (remaining > 0) {
        int n = httpd_req_recv(req, buf, MIN(remaining, 4096));
        if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (n <= 0) break;
        remaining -= n;
        if (ok && !fw_update_feed(u, (uint8_t *)buf, n)) ok = false;  // keep draining so the browser sees the error
    }
    free(buf);
    if (remaining > 0) {
        fw_update_finish(u, msg, sizeof(msg));  // abandons the half-written slot
        return ESP_FAIL;                         // connection lost
    }
    if (!fw_update_finish(u, msg, sizeof(msg))) return send_error(req, "400 Bad Request", msg);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

// Wi-Fi co-processor firmware update: the body is an ESP32-C6 .bin image.
static esp_err_t coprocessor_post(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > 4 * 1024 * 1024) {
        return send_error(req, "400 Bad Request", "upload the ESP32-C6 firmware .bin (up to 4 MB)");
    }
    cp_update_t *u = cp_update_begin();
    char *buf = malloc(4096);
    if (!u || !buf) {
        free(buf);
        free(u);
        return send_error(req, "500 Internal Server Error", "out of memory");
    }
    int remaining = req->content_len;
    bool ok = true;
    while (remaining > 0) {
        int n = httpd_req_recv(req, buf, MIN(remaining, 4096));
        if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (n <= 0) {
            ok = false;
            break;
        }
        remaining -= n;
        if (ok && !cp_update_feed(u, (uint8_t *)buf, n)) ok = false; // keep draining so the browser sees the error
    }
    free(buf);
    char msg[128] = "";
    if (remaining > 0) {
        cp_update_finish(u, msg, sizeof(msg));
        return ESP_FAIL; // connection lost
    }
    if (!cp_update_finish(u, msg, sizeof(msg))) return send_error(req, "400 Bad Request", msg);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true,\"message\":\"Wi-Fi chip updated; the display is restarting\"}");
}

static void restart_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(500));  // let the HTTP response go out
    vessel_db_save();
    esp_restart();
}

static esp_err_t restart_post(httpd_req_t *req)
{
    ESP_LOGI(TAG, "restart requested from the web page");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    xTaskCreate(restart_task, "web_restart", 2048, NULL, 5, NULL);
    return ESP_OK;
}

static esp_err_t vessels_forget_post(httpd_req_t *req)
{
    vessel_db_forget_all();
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t sdr_capture_post(httpd_req_t *req)
{
    cJSON *root = read_json(req);
    cJSON *v = cJSON_GetObjectItem(root, "seconds");
    int seconds = cJSON_IsNumber(v) ? v->valueint : 0;
    cJSON_Delete(root);
    char err[96];
    if (!sdr_capture_start(seconds, err, sizeof(err))) return send_error(req, "400 Bad Request", err);
    httpd_resp_set_status(req, "202 Accepted");
    return httpd_resp_sendstr(req, "{}");
}

// Download the last I/Q capture (raw unsigned 8-bit I/Q, as rtl_sdr writes).
static esp_err_t sdr_capture_get(httpd_req_t *req)
{
    sdr_status_t r;
    sdr_get_status(&r);
    if (r.state == SDR_CAPTURING) return send_error(req, "409 Conflict", "a capture is still being recorded");
    FILE *f = fopen(SDR_CAPTURE_PATH, "rb");
    if (!f) return send_error(req, "404 Not Found", "no capture recorded yet");

    char disp[96];
    snprintf(disp, sizeof(disp), "attachment; filename=\"ais_%lu_%lusps.cu8\"", (unsigned long)r.center_hz,
             (unsigned long)r.rate);
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Content-Disposition", disp);
    char *buf = heap_caps_malloc(UPLOAD_CHUNK, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (!buf) {
        fclose(f);
        return send_error(req, "500 Internal Server Error", "out of memory");
    }
    esp_err_t err = ESP_OK;
    size_t n;
    while ((n = fread(buf, 1, UPLOAD_CHUNK, f)) > 0) {
        if ((err = httpd_resp_send_chunk(req, buf, n)) != ESP_OK) break;
    }
    fclose(f);
    free(buf);
    if (err != ESP_OK) return err; // browser went away
    return httpd_resp_send_chunk(req, NULL, 0);
}

// Download the sentences logged during the last capture.
static esp_err_t sdr_capture_nmea_get(httpd_req_t *req)
{
    sdr_status_t r;
    sdr_get_status(&r);
    if (r.state == SDR_CAPTURING) return send_error(req, "409 Conflict", "a capture is still being recorded");
    FILE *f = fopen(SDR_CAPTURE_NMEA_PATH, "r");
    if (!f) return send_error(req, "404 Not Found", "no NMEA log recorded yet");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"ais_capture.nmea\"");
    char *buf = malloc(4096);
    esp_err_t err = buf ? ESP_OK : ESP_ERR_NO_MEM;
    size_t n;
    while (buf && (n = fread(buf, 1, 4096, f)) > 0) {
        if ((err = httpd_resp_send_chunk(req, buf, n)) != ESP_OK) break;
    }
    fclose(f);
    free(buf);
    if (err != ESP_OK) return err;
    return httpd_resp_send_chunk(req, NULL, 0);
}

// In AP mode, send every unknown URL to the config page (captive portal).
static esp_err_t not_found(httpd_req_t *req, httpd_err_code_t code)
{
    wifi_mgr_status_t w;
    wifi_mgr_get_status(&w);
    if (w.ap_active && strncmp(req->uri, "/api/", 5) != 0) {
        char loc[40];
        snprintf(loc, sizeof(loc), "http://%s/", w.ap_ip);
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", loc);
        return httpd_resp_send(req, NULL, 0);
    }
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
    return ESP_FAIL;
}

void web_server_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 8192;
    cfg.max_uri_handlers = 16;
    cfg.lru_purge_enable = true;
    cfg.recv_wait_timeout = 30;
    cfg.send_wait_timeout = 30;

    httpd_handle_t server;
    if (httpd_start(&server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "failed to start HTTP server");
        return;
    }
    const httpd_uri_t uris[] = {
        {.uri = "/", .method = HTTP_GET, .handler = index_get},
        {.uri = "/api/config", .method = HTTP_GET, .handler = config_get},
        {.uri = "/api/config", .method = HTTP_POST, .handler = config_post},
        {.uri = "/api/wifi", .method = HTTP_POST, .handler = wifi_post},
        {.uri = "/api/status", .method = HTTP_GET, .handler = status_get},
        {.uri = "/api/vessels", .method = HTTP_GET, .handler = vessels_get},
        {.uri = "/api/vessels/forget", .method = HTTP_POST, .handler = vessels_forget_post},
        {.uri = "/api/map", .method = HTTP_POST, .handler = map_post},
        {.uri = "/api/coprocessor", .method = HTTP_POST, .handler = coprocessor_post},
        {.uri = "/api/firmware", .method = HTTP_POST, .handler = firmware_post},
        {.uri = "/api/restart", .method = HTTP_POST, .handler = restart_post},
        {.uri = "/api/sdr/capture", .method = HTTP_POST, .handler = sdr_capture_post},
        {.uri = "/api/sdr/capture", .method = HTTP_GET, .handler = sdr_capture_get},
        {.uri = "/api/sdr/capture.nmea", .method = HTTP_GET, .handler = sdr_capture_nmea_get},
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(server, &uris[i]);
    }
    httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, not_found);
    ESP_LOGI(TAG, "HTTP server started");
}

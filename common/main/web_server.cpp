/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include "web_server.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cJSON.h>
#include <esp_app_desc.h>
#include <esp_heap_caps.h>
#include <esp_http_server.h>
#include <esp_idf_version.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <nvs.h>

#include "sdkconfig.h"

#include "app_mode.h"
#include "ble_addr.h"
#include "ble_scanner.h"
#include "config_store.h"
#include "http_guard.h"
#include "json_util.h"
#include "mac_watch.h"
#include "matter_device.h"
#include "presence_battery.h"
#include "presence_config.h"
#if CONFIG_PRESENCE_ROLE_SCANNER
#include "presence_link.h"
#endif
#include "presence_manager.h"
#include "scan_cache.h"
#include "wifi_config.h"

static const char *TAG = "web";

// cJSON recurses once per nesting level of a request body, on the stack of this server task (8 KB): the default
// limit of 1000 lets a body of a few hundred "[" overflow it and reset the device. The API nests 3 levels deep.
#if CONFIG_CJSON_NESTING_LIMIT > 32
#error "Set CONFIG_CJSON_NESTING_LIMIT to 32 or less (sdkconfig.defaults: 16), then run idf.py reconfigure"
#endif

extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[] asm("_binary_index_html_end");

// The page sends about 260 bytes per slot (48-byte label, 32-byte model), up to about 360 with every character of
// them escaped: 20 slots fit in 8 KB. Each JSON value costs cJSON about 50 bytes of heap while parsing: the number of
// values is checked before (too_many_values()), so a body of thousands of tiny values is refused unparsed.
static constexpr size_t kMaxConfigBody = 8192;
static constexpr size_t kMaxConfigValues = 40 * PRESENCE_SLOT_COUNT + 32; // the page sends about 21 per slot
static constexpr size_t kMaxSmallBody = 256;
static constexpr size_t kMaxErrors = 64;
#if CONFIG_PRESENCE_ALLOW_DUPLICATE_BEACON
static constexpr bool kAllowDuplicates = true;
#else
static constexpr bool kAllowDuplicates = false;
#endif

static web_server_info_t s_info;
static char s_host[WIFI_CONFIG_IP_SIZE]; // the only Host served (SoftAP address)
// The server runs one handler at a time (one task): these buffers are shared by the handlers.
static char *s_body;               // kMaxConfigBody + 1
static scan_entry_t *s_beacons;    // CONFIG_PRESENCE_SCAN_CACHE_SIZE
static presence_config_t *s_cfg;   // 2 KB with 20 slots: not on the 8 KB stack of the server task

// ---------------------------------------------------------------------------------------------------------------------
// Response helpers

namespace {

// JSON written in chunks of up to 512 bytes (httpd_resp_send_chunk): the response size is not limited by a buffer.
class JsonOut
{
public:
    explicit JsonOut(httpd_req_t *req) : mReq(req) {}

    void raw(const char *s) { append(s, strlen(s)); }

    // Numbers and fixed identifiers only; text from outside goes through str().
    void fmt(const char *format, ...) __attribute__((format(printf, 2, 3)))
    {
        char tmp[256];
        va_list ap;
        va_start(ap, format);
        const int n = vsnprintf(tmp, sizeof(tmp), format, ap);
        va_end(ap);
        if (n < 0 || static_cast<size_t>(n) >= sizeof(tmp)) {
            ESP_LOGE(TAG, "JSON fragment too long");
            mErr = ESP_ERR_INVALID_SIZE; // never send a truncated, broken document
            return;
        }
        append(tmp, static_cast<size_t>(n));
    }

    // Quoted, escaped string (labels: 48 bytes, names: 29 bytes, messages: a few hundred bytes)
    void str(const char *s)
    {
        char esc[768];
        json_escape(s, esc, sizeof(esc));
        append("\"", 1);
        raw(esc);
        append("\"", 1);
    }

    esp_err_t finish()
    {
        flush();
        if (mErr == ESP_OK) {
            mErr = httpd_resp_send_chunk(mReq, nullptr, 0);
        }
        return mErr;
    }

private:
    void append(const char *s, size_t n)
    {
        while (n > 0 && mErr == ESP_OK) {
            const size_t room = sizeof(mBuf) - mLen;
            const size_t take = n < room ? n : room;
            memcpy(&mBuf[mLen], s, take);
            mLen += take;
            s += take;
            n -= take;
            if (mLen == sizeof(mBuf)) {
                flush();
            }
        }
    }

    void flush()
    {
        if (mLen > 0 && mErr == ESP_OK) {
            mErr = httpd_resp_send_chunk(mReq, mBuf, mLen);
        }
        mLen = 0;
    }

    httpd_req_t *mReq;
    char mBuf[512];
    size_t mLen = 0;
    esp_err_t mErr = ESP_OK;
};

} // namespace

static void common_headers(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_hdr(req, "Referrer-Policy", "no-referrer");
}

static void json_headers(httpd_req_t *req, const char *status)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    common_headers(req);
}

// {"ok":false,"message":"..."} with an HTTP error status
static esp_err_t send_message(httpd_req_t *req, const char *status, bool ok, const char *message)
{
    json_headers(req, status);
    JsonOut out(req);
    out.fmt("{\"ok\":%s,\"message\":", ok ? "true" : "false");
    out.str(message);
    out.raw("}");
    return out.finish();
}

// Periodic polls of an open page (?auto=1) do not keep the device in CONFIG mode; everything else does.
static void note_activity(httpd_req_t *req)
{
    char query[32];
    char value[4];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "auto", value, sizeof(value)) == ESP_OK && strcmp(value, "1") == 0) {
        return;
    }
    app_mode_note_activity();
}

// Every handler starts here (http_guard.h): the Host must be the SoftAP address (DNS rebinding) and a POST must be
// application/json (no request from a page of another site). A refused request gets its answer, then the
// connection is closed (the handler returns ESP_FAIL) so an unread body is never parsed as the next request.
static bool request_allowed(httpd_req_t *req)
{
    char value[64];
    if (httpd_req_get_hdr_value_str(req, "Host", value, sizeof(value)) != ESP_OK ||
        !http_host_allowed(value, s_host)) {
        char message[96];
        snprintf(message, sizeof(message), "Hãy mở trang bằng địa chỉ http://%s", s_host);
        send_message(req, "403 Forbidden", false, message);
        return false;
    }
    if (req->method == HTTP_POST) {
        const bool json = httpd_req_get_hdr_value_str(req, "Content-Type", value, sizeof(value)) == ESP_OK &&
                          http_content_type_is_json(value);
        if (!json) {
            send_message(req, "415 Unsupported Media Type", false,
                         "Chỉ nhận dữ liệu JSON (Content-Type: application/json)");
            return false;
        }
    }
    return true;
}

// Reads the whole body into buf (NUL-terminated). Larger than max: 413 and the connection is closed (the
// handler returns ESP_FAIL so the unread bytes are never parsed as a request).
static esp_err_t read_body(httpd_req_t *req, char *buf, size_t max, size_t *len)
{
    if (req->content_len > max) {
        send_message(req, "413 Payload Too Large", false, "Yêu cầu quá lớn");
        return ESP_FAIL;
    }
    size_t got = 0;
    int timeouts = 0;
    while (got < req->content_len) {
        const int r = httpd_req_recv(req, &buf[got], req->content_len - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 3) {
            continue;
        }
        if (r <= 0) {
            return ESP_FAIL;
        }
        got += static_cast<size_t>(r);
    }
    buf[got] = '\0';
    *len = got;
    return ESP_OK;
}

// ---------------------------------------------------------------------------------------------------------------------
// Page

static esp_err_t index_get(httpd_req_t *req)
{
    if (!request_allowed(req)) {
        return ESP_FAIL;
    }
    app_mode_note_activity();
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    common_headers(req);
    httpd_resp_set_hdr(req, "Content-Security-Policy",
                       "default-src 'self'; script-src 'self' 'unsafe-inline'; style-src 'self' 'unsafe-inline'; "
                       "img-src 'self' data:; connect-src 'self'; frame-ancestors 'none'");
    return httpd_resp_send(req, reinterpret_cast<const char *>(index_html_start),
                           static_cast<ssize_t>(index_html_end - index_html_start));
}

static esp_err_t favicon_get(httpd_req_t *req)
{
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, nullptr, 0);
}

// ---------------------------------------------------------------------------------------------------------------------
// Status

static esp_err_t status_get(httpd_req_t *req)
{
    if (!request_allowed(req)) {
        return ESP_FAIL;
    }
    note_activity(req);
    ble_scanner_stats_t scan;
    ble_scanner_get_stats(&scan);
    scan_cache_stats_t cache;
    scan_cache_get_stats(&cache);
    char paused[96];
    ble_scanner_pause_text(scan.pause_mask, paused, sizeof(paused));

    json_headers(req, "200 OK");
    JsonOut out(req);
    out.raw("{\"mode\":\"config\",\"network\":\"" MATTER_DEVICE_NETWORK_ID "\",\"firmware\":");
    out.str(esp_app_get_description()->version);
    out.raw(",\"idf\":");
    out.str(esp_get_idf_version());
    out.fmt(",\"uptime_s\":%lld,\"idle_left_s\":%u,\"idle_timeout_s\":%u", esp_timer_get_time() / 1000000,
            static_cast<unsigned>(app_mode_idle_remaining_s()),
            static_cast<unsigned>(CONFIG_PRESENCE_CONFIG_IDLE_TIMEOUT_MIN * 60));
    out.fmt(",\"heap\":{\"free\":%u,\"min\":%u,\"largest\":%u}", static_cast<unsigned>(esp_get_free_heap_size()),
            static_cast<unsigned>(esp_get_minimum_free_heap_size()),
            static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
    out.raw(",\"ap\":{\"ssid\":");
    out.str(s_info.ssid);
    out.fmt(",\"stations\":%d,\"channel\":%d}", wifi_config_station_count(), CONFIG_PRESENCE_AP_CHANNEL);
    out.fmt(",\"scan\":{\"state\":\"%s\",\"paused_by\":", ble_scanner_state_name(scan.state));
    out.str(paused);
    out.fmt(",\"active\":%s,\"interval_ms\":%u,\"window_ms\":%u", scan.active ? "true" : "false", scan.interval_ms,
            scan.window_ms);
    out.fmt(",\"adv_reports\":%u,\"ibeacon_reports\":%u,\"starts\":%u,\"start_failures\":%u",
            static_cast<unsigned>(scan.adv_reports), static_cast<unsigned>(scan.ibeacon_reports),
            static_cast<unsigned>(scan.starts), static_cast<unsigned>(scan.start_failures));
    out.fmt(",\"unexpected_stops\":%u,\"watchdog_restarts\":%u,\"last_error\":%d",
            static_cast<unsigned>(scan.unexpected_stops), static_cast<unsigned>(scan.watchdog_restarts),
            scan.last_error);
    out.fmt(",\"list\":%u,\"capacity\":%u,\"queue_drops\":%u,\"evictions\":%u}", static_cast<unsigned>(cache.count),
            static_cast<unsigned>(cache.capacity), static_cast<unsigned>(cache.queue_drops),
            static_cast<unsigned>(cache.evictions));
    out.raw(",\"storage\":{\"status\":");
    out.str(config_store_status_name(config_store_status()));
    out.raw(",\"detail\":");
    out.str(config_store_status_detail());
    out.raw(",\"nvs_main\":");
    out.str(esp_err_to_name(s_info.nvs_main_err));
    out.raw("}");
#if CONFIG_PRESENCE_ROLE_SCANNER
    // Two-chip version: what the Matter node (ESP32-H2) last reported over the UART link
    presence_link_stats_t link;
    presence_link_get_stats(&link);
    const plink_status_t &h2 = link.peer_status;
    const long long ago = link.last_rx_ms > 0 ? (esp_timer_get_time() / 1000 - link.last_rx_ms) / 1000 : -1;
    out.fmt(",\"h2\":{\"seen\":%s,\"ago_s\":%lld,\"commissioned\":%s,\"network_up\":%s,\"window_open\":%s,"
            "\"fabrics\":%u",
            link.have_peer_status ? "true" : "false", ago, (h2.flags & PLINK_STATUS_COMMISSIONED) ? "true" : "false",
            (h2.flags & PLINK_STATUS_NETWORK_UP) ? "true" : "false",
            (h2.flags & PLINK_STATUS_WINDOW_OPEN) ? "true" : "false", static_cast<unsigned>(h2.fabrics));
    out.fmt(",\"heap_kb\":%u,\"heap_min_kb\":%u", h2.heap_free_kb, h2.heap_min_kb);
    // Endpoint of each slot's bridged device (0: none), the slots whose device waits (for memory: kept for
    // commissioning before it, too little after) and those among them that could not be created
    out.raw(",\"bridged\":[");
    for (int i = 0; i < PLINK_SLOT_COUNT; i++) {
        out.fmt("%s%u", i ? "," : "", static_cast<unsigned>(link.have_peer_status ? h2.endpoint[i] : 0));
    }
    out.raw("],\"waiting\":[");
    for (int i = 0; i < PLINK_SLOT_COUNT; i++) {
        out.fmt("%s%s", i ? "," : "", link.have_peer_status && (h2.waiting_mask & (1u << i)) ? "true" : "false");
    }
    out.raw("],\"errors\":[");
    for (int i = 0; i < PLINK_SLOT_COUNT; i++) {
        out.fmt("%s%s", i ? "," : "", link.have_peer_status && (h2.error_mask & (1u << i)) ? "true" : "false");
    }
    // The code only while the H2 can be commissioned (not paired yet, or pairing mode): otherwise it opens nothing,
    // and the SoftAP password is shared
    out.raw("],\"pairing\":");
    const bool pairable = !(h2.flags & PLINK_STATUS_COMMISSIONED) || (h2.flags & PLINK_STATUS_WINDOW_OPEN);
    if (link.have_pairing && pairable) {
        out.raw("{\"manual\":");
        out.str(link.pairing.manual_code);
        out.raw(",\"qr\":");
        out.str(link.pairing.qr_code);
        out.raw("}");
    } else {
        out.raw("null");
    }
    out.raw("}");
#endif
    out.raw("}");
    return out.finish();
}

// ---------------------------------------------------------------------------------------------------------------------
// Scan list

static esp_err_t scan_get(httpd_req_t *req)
{
    if (!request_allowed(req)) {
        return ESP_FAIL;
    }
    note_activity(req);
    int64_t now = 0;
    const size_t n = scan_cache_snapshot(s_beacons, CONFIG_PRESENCE_SCAN_CACHE_SIZE, &now);

    json_headers(req, "200 OK");
    JsonOut out(req);
    out.raw("{\"beacons\":[");
    for (size_t i = 0; i < n; i++) {
        const scan_entry_t &e = s_beacons[i];
        char addr[BLE_ADDR_STR_SIZE];
        char uuid[IBEACON_UUID_STR_SIZE];
        ble_addr_format(e.addr, addr);
        ibeacon_uuid_format(e.ibeacon.uuid, uuid);
        // The address bits say nothing reliable about a change (many beacons flag a fixed address as random with
        // "private" bits): a slot's beacon is watched in the NORMAL mode instead (mac_watch.h)
        const ble_addr_kind_t kind = ble_addr_kind(e.addr_type, e.addr);
        out.fmt("%s{\"mac\":\"%s\",\"addr_type\":\"%s\",\"kind\":\"%s\",\"rssi\":%d,", i ? "," : "", addr,
                beacon_addr_type_name(beacon_addr_type_from_ota(e.addr_type)), ble_addr_kind_name(kind), e.rssi);
        out.fmt("\"uuid\":\"%s\",\"major\":%u,\"minor\":%u,\"tx_power\":%d,\"name\":", uuid, e.ibeacon.major,
                e.ibeacon.minor, e.ibeacon.tx_power);
        out.str(e.name);
        // Battery of its last Eddystone-TLM frame: one reading, the NORMAL mode smooths them (presence_battery.h)
        const uint8_t pct = presence_battery_percent(presence_battery_kconfig(), e.battery_mv);
        out.fmt(",\"battery_mv\":%u,\"battery_pct\":%d,\"battery_age_ms\":%lld", e.battery_mv,
                pct == PRESENCE_BATTERY_UNKNOWN ? -1 : pct, e.battery_mv ? now - e.battery_ms : -1LL);
        out.fmt(",\"age_ms\":%lld,\"count\":%u}", now - e.last_seen_ms, static_cast<unsigned>(e.count));
    }
    out.raw("]}");
    return out.finish();
}

static esp_err_t scan_post(httpd_req_t *req)
{
    if (!request_allowed(req)) {
        return ESP_FAIL;
    }
    app_mode_note_activity();
    size_t len = 0;
    if (read_body(req, s_body, kMaxSmallBody, &len) != ESP_OK) {
        return ESP_FAIL;
    }
    cJSON *root = cJSON_ParseWithLength(s_body, len);
    const cJSON *action = cJSON_GetObjectItemCaseSensitive(root, "action");
    esp_err_t ret;
    if (!cJSON_IsString(action)) {
        ret = send_message(req, "400 Bad Request", false, "Thiếu action");
    } else if (strcmp(action->valuestring, "start") == 0) {
        ble_scanner_resume(SCAN_PAUSE_USER);
        ret = send_message(req, "200 OK", true, "Đã bật quét");
    } else if (strcmp(action->valuestring, "stop") == 0) {
        ble_scanner_pause(SCAN_PAUSE_USER);
        ret = send_message(req, "200 OK", true, "Đã dừng quét");
    } else if (strcmp(action->valuestring, "clear") == 0) {
        scan_cache_clear();
        ret = send_message(req, "200 OK", true, "Đã xóa danh sách");
    } else {
        ret = send_message(req, "400 Bad Request", false, "action không hợp lệ");
    }
    cJSON_Delete(root);
    return ret;
}

// ---------------------------------------------------------------------------------------------------------------------
// Configuration

// MAC watch records of the NORMAL mode (mac_watch.h), read from flash; all none when there are none yet
static mac_watch_t s_watch[PRESENCE_SLOT_COUNT]; // server task only: off its stack

// Saved configuration: a new MAC in a slot starts a new watch, and the iBeacon identity of its beacon is taken from
// the list now. So a beacon that changes its MAC before the NORMAL mode ever hears it is still caught.
static void seed_watch(const presence_config_t &cfg)
{
    const esp_err_t err = config_store_load_watch(s_watch, PRESENCE_SLOT_COUNT);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "MAC watch records not read (%s)", esp_err_to_name(err));
    }
    int64_t now = 0;
    const size_t n = scan_cache_snapshot(s_beacons, CONFIG_PRESENCE_SCAN_CACHE_SIZE, &now);
    bool changed = false;
    for (int i = 0; i < PRESENCE_SLOT_COUNT; i++) {
        const presence_slot_config_t &s = cfg.slots[i];
        if (!presence_slot_is_active(&s)) {
            continue; // its record is kept: the same MAC enabled again keeps its verdict
        }
        changed = mac_watch_assign(&s_watch[i], s.addr) || changed;
        for (size_t j = 0; j < n; j++) {
            if (memcmp(s_beacons[j].addr, s.addr, BLE_ADDR_LEN) == 0 &&
                beacon_addr_type_matches(s.addr_type, s_beacons[j].addr_type)) {
                changed = mac_watch_seed(&s_watch[i], &s_beacons[j].ibeacon) || changed;
                break;
            }
        }
    }
    if (changed && config_store_save_watch(s_watch, PRESENCE_SLOT_COUNT) != ESP_OK) {
        ESP_LOGW(TAG, "MAC watch records not saved: the NORMAL mode starts them");
    }
}

static void write_config(JsonOut &out, const presence_config_t &cfg)
{
    mac_watch_t *watch = s_watch;
    config_store_load_watch(watch, PRESENCE_SLOT_COUNT); // all none when there are none (yet)
    out.fmt("{\"scan\":{\"interval_ms\":%u,\"window_ms\":%u},\"slots\":[", cfg.scan_interval_ms, cfg.scan_window_ms);
    for (int i = 0; i < PRESENCE_SLOT_COUNT; i++) {
        const presence_slot_config_t &s = cfg.slots[i];
        char addr[BLE_ADDR_STR_SIZE] = "";
        if (s.has_addr) {
            ble_addr_format(s.addr, addr);
        }
        out.fmt("%s{\"enabled\":%s,\"label\":", i ? "," : "", s.enabled ? "true" : "false");
        out.str(s.label);
        out.fmt(",\"mac\":\"%s\",\"addr_type\":\"%s\",\"timeout_s\":%u,\"rssi_filter\":%s,", addr,
                beacon_addr_type_name(s.addr_type), s.timeout_s, s.rssi_filter ? "true" : "false");
        out.fmt("\"rssi_threshold\":%d,\"rssi_hysteresis\":%u,\"model\":", s.rssi_threshold, s.rssi_hysteresis);
        out.str(s.model);
        // The MAC watch of the beacon assigned when the NORMAL mode last ran (its mac may differ from the slot's)
        const mac_watch_t &w = watch[i];
        char wmac[BLE_ADDR_STR_SIZE] = "";
        char wnew[BLE_ADDR_STR_SIZE] = "";
        if (w.state != MAC_WATCH_NONE) {
            ble_addr_format(w.mac, wmac);
        }
        if (w.state == MAC_WATCH_CHANGED) {
            ble_addr_format(w.new_mac, wnew);
        }
        out.fmt(",\"watch\":{\"mac\":\"%s\",\"state\":\"%s\",\"watched_s\":%u,\"total_s\":%u,\"new_mac\":\"%s\"}}",
                wmac, mac_watch_state_name(w.state), static_cast<unsigned>(w.watched_ms / 1000),
                static_cast<unsigned>(MAC_WATCH_TIME_MS / 1000), wnew);
    }
    out.raw("]");
}

static esp_err_t config_get(httpd_req_t *req)
{
    if (!request_allowed(req)) {
        return ESP_FAIL;
    }
    note_activity(req);
    config_store_get(s_cfg);
    json_headers(req, "200 OK");
    JsonOut out(req);
    write_config(out, *s_cfg);
    out.fmt(",\"limits\":{\"label_max_bytes\":%d,\"model_max_bytes\":%d,\"timeout_min\":%d,\"timeout_max\":%d,"
            "\"rssi_min\":%d,\"rssi_max\":%d,\"hysteresis_max\":%d,",
            PRESENCE_LABEL_MAX_BYTES, PRESENCE_MODEL_MAX_BYTES, PRESENCE_TIMEOUT_MIN_S, PRESENCE_TIMEOUT_MAX_S,
            PRESENCE_RSSI_THRESHOLD_MIN, PRESENCE_RSSI_THRESHOLD_MAX, PRESENCE_RSSI_HYSTERESIS_MAX);
    out.fmt("\"interval_min\":%d,\"interval_max\":%d,\"window_min\":%d}", PRESENCE_SCAN_INTERVAL_MIN_MS,
            PRESENCE_SCAN_INTERVAL_MAX_MS, PRESENCE_SCAN_WINDOW_MIN_MS);
    out.fmt(",\"defaults\":{\"timeout_s\":%d,\"rssi_threshold\":%d,\"rssi_hysteresis\":%d,\"interval_ms\":%d,"
            "\"window_ms\":%d}",
            CONFIG_PRESENCE_DEFAULT_TIMEOUT_SEC, CONFIG_PRESENCE_DEFAULT_RSSI_THRESHOLD,
            CONFIG_PRESENCE_DEFAULT_RSSI_HYSTERESIS, CONFIG_PRESENCE_SCAN_INTERVAL_MS, CONFIG_PRESENCE_SCAN_WINDOW_MS);
    out.fmt(",\"allow_duplicates\":%s,\"network\":\"%s\"", kAllowDuplicates ? "true" : "false",
            MATTER_DEVICE_NETWORK_ID);
    out.raw(",\"storage\":{\"status\":");
    out.str(config_store_status_name(config_store_status()));
    out.raw(",\"detail\":");
    out.str(config_store_status_detail());
    out.raw("}}");
    return out.finish();
}

namespace {

struct Errors {
    presence_config_error_t list[kMaxErrors];
    size_t count = 0;

    void add(int slot, presence_field_t field, presence_error_code_t code)
    {
        if (count < kMaxErrors) {
            list[count] = { static_cast<int8_t>(slot), static_cast<uint8_t>(field), static_cast<uint8_t>(code), -1 };
        }
        count++;
    }
};

} // namespace

static bool json_int(const cJSON *obj, const char *name, long min, long max, long *out)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, name);
    if (!cJSON_IsNumber(item)) {
        return false;
    }
    const double v = item->valuedouble;
    if (!(v >= static_cast<double>(min) && v <= static_cast<double>(max)) || v != floor(v)) {
        return false;
    }
    *out = static_cast<long>(v);
    return true;
}

static bool json_bool(const cJSON *obj, const char *name, bool *out)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, name);
    if (!cJSON_IsBool(item)) {
        return false;
    }
    *out = cJSON_IsTrue(item);
    return true;
}

static const char *json_string(const cJSON *obj, const char *name)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, name);
    return cJSON_IsString(item) ? item->valuestring : nullptr;
}

// Label without surrounding blanks; empty: the default "Presence N"
static void parse_label(int slot, const char *text, presence_slot_config_t *s, Errors *errors)
{
    while (*text == ' ' || *text == '\t') {
        text++;
    }
    size_t len = strlen(text);
    while (len > 0 && (text[len - 1] == ' ' || text[len - 1] == '\t')) {
        len--;
    }
    if (len == 0) {
        presence_config_default_label(slot, s->label, sizeof(s->label));
    } else if (len > PRESENCE_LABEL_MAX_BYTES) {
        errors->add(slot, PRESENCE_FIELD_LABEL, PRESENCE_ERR_LABEL_TOO_LONG); // refused, never cut silently
    } else {
        memcpy(s->label, text, len);
        s->label[len] = '\0';
    }
}

// Model without surrounding blanks; empty: unknown (the bridge shows the beacon's advertised name)
static void parse_model(int slot, const char *text, presence_slot_config_t *s, Errors *errors)
{
    while (*text == ' ' || *text == '\t') {
        text++;
    }
    size_t len = strlen(text);
    while (len > 0 && (text[len - 1] == ' ' || text[len - 1] == '\t')) {
        len--;
    }
    if (len > PRESENCE_MODEL_MAX_BYTES) {
        errors->add(slot, PRESENCE_FIELD_MODEL, PRESENCE_ERR_MODEL_TOO_LONG); // refused, never cut silently
        return;
    }
    memcpy(s->model, text, len);
    s->model[len] = '\0';
}

// Fills cfg from the request; type errors go to errors, value checks are left to presence_config_validate().
static void parse_config(const cJSON *root, presence_config_t *cfg, Errors *errors)
{
    presence_config_set_defaults(cfg);
    long v = 0;
    const cJSON *scan = cJSON_GetObjectItemCaseSensitive(root, "scan");
    if (cJSON_IsObject(scan) && json_int(scan, "interval_ms", 0, 65535, &v)) {
        cfg->scan_interval_ms = static_cast<uint16_t>(v);
    } else {
        errors->add(-1, PRESENCE_FIELD_SCAN_INTERVAL, PRESENCE_ERR_TYPE);
    }
    if (cJSON_IsObject(scan) && json_int(scan, "window_ms", 0, 65535, &v)) {
        cfg->scan_window_ms = static_cast<uint16_t>(v);
    } else {
        errors->add(-1, PRESENCE_FIELD_SCAN_WINDOW, PRESENCE_ERR_TYPE);
    }

    const cJSON *slots = cJSON_GetObjectItemCaseSensitive(root, "slots");
    if (!cJSON_IsArray(slots) || cJSON_GetArraySize(slots) != PRESENCE_SLOT_COUNT) {
        errors->add(-1, PRESENCE_FIELD_NONE, PRESENCE_ERR_TYPE);
        return;
    }
    for (int i = 0; i < PRESENCE_SLOT_COUNT; i++) {
        const cJSON *o = cJSON_GetArrayItem(slots, i);
        presence_slot_config_t &s = cfg->slots[i];
        if (!cJSON_IsObject(o)) {
            errors->add(i, PRESENCE_FIELD_NONE, PRESENCE_ERR_TYPE);
            continue;
        }
        if (!json_bool(o, "enabled", &s.enabled)) {
            errors->add(i, PRESENCE_FIELD_ENABLED, PRESENCE_ERR_TYPE);
        }
        const char *label = json_string(o, "label");
        if (label != nullptr) {
            parse_label(i, label, &s, errors);
        } else {
            errors->add(i, PRESENCE_FIELD_LABEL, PRESENCE_ERR_TYPE);
        }
        const char *mac = json_string(o, "mac");
        if (mac == nullptr) {
            errors->add(i, PRESENCE_FIELD_ADDR, PRESENCE_ERR_TYPE);
        } else if (mac[0] == '\0') {
            s.has_addr = false;
        } else if (ble_addr_parse(mac, s.addr)) {
            s.has_addr = true;
        } else {
            errors->add(i, PRESENCE_FIELD_ADDR, PRESENCE_ERR_ADDR_INVALID);
        }
        if (!beacon_addr_type_parse(json_string(o, "addr_type"), &s.addr_type)) {
            errors->add(i, PRESENCE_FIELD_ADDR_TYPE, PRESENCE_ERR_ADDR_TYPE);
        }
        if (json_int(o, "timeout_s", 0, 65535, &v)) {
            s.timeout_s = static_cast<uint16_t>(v);
        } else {
            errors->add(i, PRESENCE_FIELD_TIMEOUT, PRESENCE_ERR_TIMEOUT_RANGE);
        }
        if (!json_bool(o, "rssi_filter", &s.rssi_filter)) {
            errors->add(i, PRESENCE_FIELD_RSSI_FILTER, PRESENCE_ERR_TYPE);
        }
        if (json_int(o, "rssi_threshold", -128, 127, &v)) {
            s.rssi_threshold = static_cast<int8_t>(v);
        } else {
            errors->add(i, PRESENCE_FIELD_RSSI_THRESHOLD, PRESENCE_ERR_RSSI_RANGE);
        }
        if (json_int(o, "rssi_hysteresis", 0, 255, &v)) {
            s.rssi_hysteresis = static_cast<uint8_t>(v);
        } else {
            errors->add(i, PRESENCE_FIELD_RSSI_HYSTERESIS, PRESENCE_ERR_HYSTERESIS_RANGE);
        }
        const char *model = json_string(o, "model");
        if (model != nullptr) {
            parse_model(i, model, &s, errors);
        } else {
            errors->add(i, PRESENCE_FIELD_MODEL, PRESENCE_ERR_TYPE);
        }
    }
}

// Structural characters outside strings: about one per JSON value. Refuses a body cJSON would turn into thousands
// of allocations before it is even parsed.
static bool too_many_values(const char *body, size_t len)
{
    size_t n = 0;
    bool in_string = false;
    bool escaped = false;
    for (size_t i = 0; i < len; i++) {
        const char c = body[i];
        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                in_string = false;
            }
        } else if (c == '"') {
            in_string = true;
        } else if ((c == ',' || c == ':' || c == '[' || c == '{') && ++n > kMaxConfigValues) {
            return true;
        }
    }
    return false;
}

static esp_err_t send_errors(httpd_req_t *req, const Errors &errors)
{
    json_headers(req, "422 Unprocessable Entity");
    JsonOut out(req);
    out.raw("{\"ok\":false,\"message\":");
    out.str("Cấu hình chưa hợp lệ, chưa lưu. Sửa các ô được đánh dấu.");
    out.raw(",\"errors\":[");
    const size_t n = errors.count < kMaxErrors ? errors.count : kMaxErrors;
    for (size_t i = 0; i < n; i++) {
        const presence_config_error_t &e = errors.list[i];
        out.fmt("%s{\"slot\":%d,\"field\":\"%s\",\"other_slot\":%d,\"message\":", i ? "," : "", e.slot,
                presence_field_name(e.field), e.other_slot);
        char message[256];
        if (e.code == PRESENCE_ERR_ADDR_DUPLICATE && e.other_slot >= 0) {
            snprintf(message, sizeof(message), "%s (Presence %d)", presence_error_text(e.code), e.other_slot + 1);
        } else {
            snprintf(message, sizeof(message), "%s", presence_error_text(e.code));
        }
        out.str(message);
        out.raw("}");
    }
    out.raw("]}");
    return out.finish();
}

static esp_err_t config_post(httpd_req_t *req)
{
    if (!request_allowed(req)) {
        return ESP_FAIL;
    }
    app_mode_note_activity();
    size_t len = 0;
    if (read_body(req, s_body, kMaxConfigBody, &len) != ESP_OK) {
        return ESP_FAIL;
    }
    if (too_many_values(s_body, len)) {
        return send_message(req, "400 Bad Request", false, "Dữ liệu gửi lên quá nhiều trường");
    }
    cJSON *root = cJSON_ParseWithLength(s_body, len);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return send_message(req, "400 Bad Request", false, "Dữ liệu gửi lên không phải JSON hợp lệ");
    }
    presence_config_t &cfg = *s_cfg;
    static Errors errors; // server task only: off its stack
    errors = Errors();
    parse_config(root, &cfg, &errors);
    cJSON_Delete(root);
    if (errors.count == 0) {
        static presence_config_error_t found[kMaxErrors];
        const size_t n = presence_config_validate(&cfg, kAllowDuplicates, found, kMaxErrors);
        for (size_t i = 0; i < n && i < kMaxErrors; i++) {
            errors.list[i] = found[i];
        }
        errors.count = n;
    }
    if (errors.count > 0) {
        return send_errors(req, errors);
    }

    if (config_store_status() == CONFIG_STORE_UNAVAILABLE) {
        char message[320];
        snprintf(message, sizeof(message),
                 "Phân vùng cấu hình beacon không dùng được (%s). Hãy định dạng phân vùng cấu hình beacon "
                 "rồi lưu lại. Dữ liệu ghép Matter/" MATTER_DEVICE_NETWORK_NAME " không bị ảnh hưởng.",
                 config_store_status_detail());
        return send_message(req, "500 Internal Server Error", false, message);
    }
    const esp_err_t err = config_store_save(&cfg);
    if (err != ESP_OK) {
        char message[256];
        snprintf(message, sizeof(message), "Lưu thất bại (%s). Cấu hình cũ vẫn được giữ nguyên.",
                 esp_err_to_name(err));
        return send_message(req, "500 Internal Server Error", false, message);
    }
    seed_watch(cfg);
#if CONFIG_PRESENCE_ROLE_SCANNER
    // The bridge of the ESP32-H2 adds and removes its devices now; the presence states follow after the restart
    presence_link_set_slots(&cfg, true);
    return send_message(req, "200 OK", true,
                        "Đã lưu và đọc lại kiểm tra thành công. Thiết bị con trên iBeacon Bridge (ESP32-H2) được cập "
                        "nhật ngay. Bấm \"Khởi động lại về chế độ Matter\" để bắt đầu quét với cấu hình mới.");
#else
    return send_message(req, "200 OK", true,
                        "Đã lưu và đọc lại kiểm tra thành công. Bấm \"Khởi động lại về chế độ Matter\" để áp dụng.");
#endif
}

static esp_err_t restart_post(httpd_req_t *req)
{
    if (!request_allowed(req)) {
        return ESP_FAIL;
    }
    size_t len = 0;
    if (read_body(req, s_body, kMaxSmallBody, &len) != ESP_OK) {
        return ESP_FAIL;
    }
    const esp_err_t err = send_message(req, "200 OK", true, "Đang khởi động lại về chế độ Matter...");
    // The response is handed to the socket first; the restart waits a second for it to leave.
    app_mode_request_restart(APP_MODE_NORMAL, 1000, "web page");
    return err;
}

static esp_err_t keepalive_post(httpd_req_t *req)
{
    if (!request_allowed(req)) {
        return ESP_FAIL;
    }
    app_mode_note_activity();
    size_t len = 0;
    if (read_body(req, s_body, kMaxSmallBody, &len) != ESP_OK) {
        return ESP_FAIL;
    }
    json_headers(req, "200 OK");
    JsonOut out(req);
    out.fmt("{\"ok\":true,\"idle_left_s\":%u}", static_cast<unsigned>(app_mode_idle_remaining_s()));
    return out.finish();
}

static esp_err_t format_post(httpd_req_t *req)
{
    if (!request_allowed(req)) {
        return ESP_FAIL;
    }
    app_mode_note_activity();
    size_t len = 0;
    if (read_body(req, s_body, kMaxSmallBody, &len) != ESP_OK) {
        return ESP_FAIL;
    }
    cJSON *root = cJSON_ParseWithLength(s_body, len);
    const char *confirm = json_string(root, "confirm");
    const bool confirmed = confirm != nullptr && strcmp(confirm, "FORMAT") == 0;
    cJSON_Delete(root);
    if (!confirmed) {
        return send_message(req, "400 Bad Request", false, "Thiếu xác nhận");
    }
    const config_store_status_t status = config_store_status();
    if (status != CONFIG_STORE_UNAVAILABLE && status != CONFIG_STORE_INVALID) {
        return send_message(req, "409 Conflict", false, "Phân vùng cấu hình đang bình thường, không cần định dạng");
    }
    const esp_err_t err = config_store_format();
    if (err != ESP_OK) {
        char message[120];
        snprintf(message, sizeof(message), "Định dạng thất bại (%s)", esp_err_to_name(err));
        return send_message(req, "500 Internal Server Error", false, message);
    }
    return send_message(req, "200 OK", true, "Đã định dạng phân vùng cấu hình beacon. Hãy cấu hình lại và lưu.");
}

#if CONFIG_PRESENCE_ROLE_SCANNER
// A command for the ESP32-H2 is only sent while it answers over UART and runs Matter: else 409 with the reason
static bool h2_reachable(httpd_req_t *req)
{
    presence_link_stats_t link;
    presence_link_get_stats(&link);
    const int64_t ago_ms = esp_timer_get_time() / 1000 - link.last_rx_ms;
    if (!link.have_peer_status || ago_ms > 10000) {
        send_message(req, "409 Conflict", false,
                     "Không có tín hiệu từ ESP32-H2 qua UART: lệnh không được gửi. Kiểm tra dây nối và nguồn.");
        return false;
    }
    if (!(link.peer_status.flags & PLINK_STATUS_MATTER_STARTED)) {
        send_message(req, "409 Conflict", false,
                     "Matter chưa chạy trên ESP32-H2 (xem log USB của H2): lệnh không được gửi.");
        return false;
    }
    return true;
}

// Two-chip version: removes the Matter pairing of the ESP32-H2 (fabrics, Thread network, bridged devices), so it
// can be commissioned again, without any console command. Explicit confirmation in the body, as /api/format.
static esp_err_t h2_reset_post(httpd_req_t *req)
{
    if (!request_allowed(req)) {
        return ESP_FAIL;
    }
    app_mode_note_activity();
    size_t len = 0;
    if (read_body(req, s_body, kMaxSmallBody, &len) != ESP_OK) {
        return ESP_FAIL;
    }
    cJSON *root = cJSON_ParseWithLength(s_body, len);
    const char *confirm = json_string(root, "confirm");
    const bool confirmed = confirm != nullptr && strcmp(confirm, "RESET-MATTER") == 0;
    cJSON_Delete(root);
    if (!confirmed) {
        return send_message(req, "400 Bad Request", false, "Thiếu xác nhận");
    }
    if (!h2_reachable(req)) {
        return ESP_OK;
    }
    presence_link_request_command(PLINK_CMD_FACTORY_RESET);
    ESP_LOGW(TAG, "Matter factory reset of the ESP32-H2 asked on the page");
    return send_message(req, "200 OK", true,
                        "Đã gửi lệnh tới ESP32-H2: H2 xóa ghép Matter và mạng Thread rồi khởi động lại (đèn H2 nháy đỏ "
                        "3 lần), chờ ghép mới (mã ghép không đổi). Thiết bị con được tạo lại theo cấu hình đã lưu.");
}

// Two-chip version: pairing mode of the ESP32-H2, as a double click on its BOOT button
static esp_err_t h2_pair_post(httpd_req_t *req)
{
    if (!request_allowed(req)) {
        return ESP_FAIL;
    }
    app_mode_note_activity();
    size_t len = 0;
    if (read_body(req, s_body, kMaxSmallBody, &len) != ESP_OK) {
        return ESP_FAIL;
    }
    if (!h2_reachable(req)) {
        return ESP_OK;
    }
    presence_link_request_command(PLINK_CMD_OPEN_WINDOW);
    ESP_LOGI(TAG, "Pairing mode of the ESP32-H2 asked on the page");
    return send_message(req, "200 OK", true,
                        "Đã gửi lệnh: ESP32-H2 mở chế độ ghép (đèn nháy xanh dương). Thêm phụ kiện trong ứng dụng "
                        "Nhà bằng mã ghép hiện ở trên.");
}
#endif

// ---------------------------------------------------------------------------------------------------------------------

esp_err_t web_server_start(const web_server_info_t *info)
{
    s_info = *info;
    wifi_config_ip(s_host);
    if (s_host[0] == '\0') {
        ESP_LOGE(TAG, "SoftAP address unknown: web server not started");
        return ESP_ERR_INVALID_STATE;
    }
    s_body = static_cast<char *>(malloc(kMaxConfigBody + 1));
    s_beacons = static_cast<scan_entry_t *>(calloc(CONFIG_PRESENCE_SCAN_CACHE_SIZE, sizeof(scan_entry_t)));
    s_cfg = static_cast<presence_config_t *>(calloc(1, sizeof(presence_config_t)));
    if (s_body == nullptr || s_beacons == nullptr || s_cfg == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192;
    config.max_uri_handlers = 14;
    config.max_open_sockets = 4;
    config.lru_purge_enable = true;
    config.recv_wait_timeout = 10;
    config.send_wait_timeout = 10;

    httpd_handle_t server = nullptr;
    esp_err_t err = httpd_start(&server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start() failed: %s", esp_err_to_name(err));
        return err;
    }
    const httpd_uri_t uris[] = {
        { .uri = "/", .method = HTTP_GET, .handler = index_get, .user_ctx = nullptr },
        { .uri = "/favicon.ico", .method = HTTP_GET, .handler = favicon_get, .user_ctx = nullptr },
        { .uri = "/api/status", .method = HTTP_GET, .handler = status_get, .user_ctx = nullptr },
        { .uri = "/api/scan", .method = HTTP_GET, .handler = scan_get, .user_ctx = nullptr },
        { .uri = "/api/scan", .method = HTTP_POST, .handler = scan_post, .user_ctx = nullptr },
        { .uri = "/api/config", .method = HTTP_GET, .handler = config_get, .user_ctx = nullptr },
        { .uri = "/api/config", .method = HTTP_POST, .handler = config_post, .user_ctx = nullptr },
        { .uri = "/api/restart", .method = HTTP_POST, .handler = restart_post, .user_ctx = nullptr },
        { .uri = "/api/keepalive", .method = HTTP_POST, .handler = keepalive_post, .user_ctx = nullptr },
        { .uri = "/api/format", .method = HTTP_POST, .handler = format_post, .user_ctx = nullptr },
#if CONFIG_PRESENCE_ROLE_SCANNER
        { .uri = "/api/h2/reset", .method = HTTP_POST, .handler = h2_reset_post, .user_ctx = nullptr },
        { .uri = "/api/h2/pair", .method = HTTP_POST, .handler = h2_pair_post, .user_ctx = nullptr },
#endif
    };
    for (const httpd_uri_t &uri : uris) {
        err = httpd_register_uri_handler(server, &uri);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Cannot register %s: %s", uri.uri, esp_err_to_name(err));
            return err;
        }
    }
    ESP_LOGI(TAG, "Configuration page at http://%s (%u bytes)", s_host,
             static_cast<unsigned>(index_html_end - index_html_start));
    return ESP_OK;
}

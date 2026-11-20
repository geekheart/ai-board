#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "board.h"
#include "board_events.h"
#include "board_lua.h"
#include "console.h"
#include "control_lease.h"
#include "matrix.h"
#include "script_store.h"
#include "web.h"
#include "web_store.h"
#include "workbench.h"
#include "driver/gpio.h"
#include "esp_app_desc.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "psa/crypto.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <stdatomic.h>

#define AP_IP "192.168.4.1"


static nvs_handle_t storage;
static char access_key[25];
static char ap_name[24];
static atomic_bool matrix_armed;
static atomic_bool reboot_pending;
static SemaphoreHandle_t matrix_lock;
static SemaphoreHandle_t settings_lock;
static SemaphoreHandle_t scan_lock;
static esp_netif_t *station;
static esp_timer_handle_t reboot_timer;
static esp_timer_handle_t wifi_retry_timer;

static void reboot(void *context)
{
    esp_restart();
}

void board_schedule_reboot(void)
{
    atomic_store(&reboot_pending, true);
    esp_err_t result = esp_timer_start_once(reboot_timer, 600000);
    if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) {
        atomic_store(&reboot_pending, false);
        printf("Reboot timer failed: %s\n", esp_err_to_name(result));
    }
}

bool board_reboot_is_pending(void)
{
    return atomic_load(&reboot_pending);
}

static esp_err_t bad_request(httpd_req_t *request, const char *message)
{
    return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, message);
}

static bool authorized(httpd_req_t *request)
{
    char key[sizeof(access_key)];
    return httpd_req_get_hdr_value_str(request, "X-Board-Key", key, sizeof(key)) == ESP_OK &&
           strcmp(key, access_key) == 0;
}

static esp_err_t require_key(httpd_req_t *request)
{
    return httpd_resp_send_err(request, HTTPD_401_UNAUTHORIZED, "Supply X-Board-Key");
}

static esp_err_t json(httpd_req_t *request, const char *body)
{
    httpd_resp_set_type(request, "application/json");
    return httpd_resp_sendstr(request, body);
}

static const char *lease_header(httpd_req_t *request, char token[CONTROL_LEASE_TOKEN_LENGTH + 1])
{
    const esp_err_t found = httpd_req_get_hdr_value_str(request, "X-Board-Lease", token,
                                                      CONTROL_LEASE_TOKEN_LENGTH + 1);
    if (found == ESP_ERR_NOT_FOUND) return NULL;
    return found == ESP_OK ? token : "";
}

static esp_err_t control_error(httpd_req_t *request, control_lease_result_t result)
{
    if (result == CONTROL_LEASE_BUSY) {
        httpd_resp_set_status(request, "409 Conflict");
        return httpd_resp_sendstr(request, "Board control is busy");
    }
    if (result == CONTROL_LEASE_DENIED) {
        httpd_resp_set_status(request, "423 Locked");
        return httpd_resp_sendstr(request, "An active board lease is required");
    }
    return bad_request(request, "Invalid board control request");
}

static esp_err_t send_control_status(httpd_req_t *request, const char *issued_token)
{
    const control_lease_status_t lease = control_lease_status();
    const uint32_t revision = workbench_result_revision();
    char response[160];
    if (issued_token) {
        snprintf(response, sizeof(response), "{\"active\":true,\"remaining_s\":%u,\"revision\":%" PRIu32
                 ",\"lease_token\":\"%s\"}", lease.remaining_s, revision, issued_token);
    } else {
        snprintf(response, sizeof(response), "{\"active\":%s,\"remaining_s\":%u,\"revision\":%" PRIu32 "}",
                 lease.active ? "true" : "false", lease.remaining_s, revision);
    }
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return json(request, response);
}

static esp_err_t get_control(httpd_req_t *request)
{
    return send_control_status(request, NULL);
}

static bool form_unsigned(const char *form, const char *key, unsigned min, unsigned max, unsigned *result)
{
    char value[6];
    if (httpd_query_key_value(form, key, value, sizeof(value)) != ESP_OK || !value[0]) return false;
    unsigned parsed = 0;
    for (const char *digit = value; *digit; ++digit) {
        if (*digit < '0' || *digit > '9') return false;
        parsed = parsed * 10 + (unsigned)(*digit - '0');
    }
    if (parsed < min || parsed > max) return false;
    *result = parsed;
    return true;
}

static esp_err_t post_control(httpd_req_t *request)
{
    if (!authorized(request)) return require_key(request);
    char body[80], action[8], token_buffer[CONTROL_LEASE_TOKEN_LENGTH + 1];
    if (!request->content_len || request->content_len >= sizeof(body)) {
        return bad_request(request, "Expected a board control action");
    }
    size_t used = 0;
    while (used < request->content_len) {
        const int received = httpd_req_recv(request, body + used, request->content_len - used);
        if (received <= 0) return bad_request(request, "Incomplete board control request");
        used += received;
    }
    if (memchr(body, 0, used)) return bad_request(request, "Invalid board control request");
    body[used] = 0;
    if (httpd_query_key_value(body, "action", action, sizeof(action)) != ESP_OK) {
        return bad_request(request, "Missing board control action");
    }

    const char *token = lease_header(request, token_buffer);
    unsigned ttl;
    control_lease_result_t result;
    if (strcmp(action, "acquire") == 0) {
        if (token || !form_unsigned(body, "ttl_s", CONTROL_LEASE_MIN_TTL_S, CONTROL_LEASE_MAX_TTL_S, &ttl)) {
            return control_error(request, CONTROL_LEASE_INVALID);
        }
        if (board_reboot_is_pending()) return control_error(request, CONTROL_LEASE_BUSY);
        result = control_lease_acquire(ttl, token_buffer);
        if (result != CONTROL_LEASE_OK) return control_error(request, result);
        if (board_reboot_is_pending()) {
            control_lease_release(token_buffer);
            return control_error(request, CONTROL_LEASE_BUSY);
        }
        return send_control_status(request, token_buffer);
    }
    if (strcmp(action, "renew") == 0) {
        if (!form_unsigned(body, "ttl_s", CONTROL_LEASE_MIN_TTL_S, CONTROL_LEASE_MAX_TTL_S, &ttl)) {
            return control_error(request, CONTROL_LEASE_INVALID);
        }
        result = control_lease_renew(token, ttl);
    } else if (strcmp(action, "release") == 0) {
        result = control_lease_release(token);
    } else {
        return control_error(request, CONTROL_LEASE_INVALID);
    }
    return result == CONTROL_LEASE_OK ? send_control_status(request, NULL) : control_error(request, result);
}

typedef struct {
    const char *uri;
    esp_err_t (*handler)(httpd_req_t *request, bool lease_holder);
} mutation_route_t;

static esp_err_t post_mutation(httpd_req_t *request)
{
    if (!authorized(request)) return require_key(request);
    char buffer[CONTROL_LEASE_TOKEN_LENGTH + 1];
    const char *token = lease_header(request, buffer);
    const control_lease_result_t permit = control_lease_begin_mutation(token);
    if (permit != CONTROL_LEASE_OK) return control_error(request, permit);

    const mutation_route_t *route = request->user_ctx;
    esp_err_t result = board_reboot_is_pending() ? control_error(request, CONTROL_LEASE_BUSY)
                                                 : route->handler(request, token != NULL);
    control_lease_end_mutation();
    return result;
}

static bool lua_timeout(const char *query, unsigned *timeout_ms)
{
    *timeout_ms = BOARD_LUA_DEFAULT_TIMEOUT_MS;
    char value[6];
    if (httpd_query_key_value(query, "timeout_ms", value, sizeof(value)) == ESP_ERR_NOT_FOUND) return true;
    return form_unsigned(query, "timeout_ms", BOARD_LUA_MIN_TIMEOUT_MS, BOARD_LUA_MAX_TIMEOUT_MS, timeout_ms);
}

static esp_err_t respond_lua(httpd_req_t *request, const char *source, size_t length, unsigned timeout_ms,
                             bool publish)
{
    char *response = NULL;
    esp_err_t result = board_lua_run(source, length, timeout_ms, &response);
    if (result != ESP_OK) {
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(result));
    }
    if (publish) workbench_publish_result("lua", response);
    result = json(request, response);
    free(response);
    return result;
}

static esp_err_t script_storage_error(httpd_req_t *request, esp_err_t error)
{
    if (error == ESP_ERR_NOT_FOUND) return httpd_resp_send_err(request, HTTPD_404_NOT_FOUND, "Script not found");
    if (error == ESP_ERR_INVALID_ARG) return bad_request(request, "Invalid script name or source");
    if (error == ESP_ERR_NOT_FINISHED) {
        httpd_resp_set_status(request, "507 Insufficient Storage");
        return httpd_resp_sendstr(request, "Script storage is full");
    }
    if (error == ESP_ERR_INVALID_STATE) {
        httpd_resp_set_status(request, "503 Service Unavailable");
        return httpd_resp_sendstr(request, "Script storage is unavailable");
    }
    return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(error));
}

enum {
    SCRIPT_ETAG_DIGEST_BYTES = 32,
    SCRIPT_ETAG_HEX_LENGTH = 2 * SCRIPT_ETAG_DIGEST_BYTES,
    SCRIPT_ETAG_SIZE = SCRIPT_ETAG_HEX_LENGTH + 3,
};

static bool script_etag(const char *source, size_t length, char tag[SCRIPT_ETAG_SIZE])
{
    uint8_t digest[SCRIPT_ETAG_DIGEST_BYTES];
    size_t digest_length;
    if (psa_hash_compute(PSA_ALG_SHA_256, (const uint8_t *)source, length,
                         digest, sizeof(digest), &digest_length) != PSA_SUCCESS ||
        digest_length != sizeof(digest)) {
        return false;
    }

    static const char hex[] = "0123456789abcdef";
    tag[0] = '"';
    for (size_t i = 0; i < sizeof(digest); ++i) {
        tag[1 + 2 * i] = hex[digest[i] >> 4];
        tag[2 + 2 * i] = hex[digest[i] & 0xf];
    }
    tag[SCRIPT_ETAG_SIZE - 2] = '"';
    tag[SCRIPT_ETAG_SIZE - 1] = '\0';
    return true;
}

static esp_err_t script_etag_error(httpd_req_t *request)
{
    return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "Script version unavailable");
}

static esp_err_t save_lua(httpd_req_t *request, const char *name, const char *source, size_t length,
                          bool include_etag)
{
    if (memchr(source, '\0', length)) return bad_request(request, "Expected Lua source text");

    char tag[SCRIPT_ETAG_SIZE];
    if (include_etag && !script_etag(source, length, tag)) return script_etag_error(request);
    const esp_err_t written = script_store_write(name, source, length);
    if (written != ESP_OK) return script_storage_error(request, written);

    char response[128 + SCRIPT_ETAG_SIZE];
    if (include_etag) {
        snprintf(response, sizeof(response),
                 "{\"saved\":true,\"name\":\"%s\",\"bytes\":%zu,\"etag\":\"\\\"%.*s\\\"\"}",
                 name, length, SCRIPT_ETAG_HEX_LENGTH, tag + 1);
    } else {
        snprintf(response, sizeof(response), "{\"saved\":true,\"name\":\"%s\",\"bytes\":%zu}", name, length);
    }
    return json(request, response);
}

static esp_err_t receive_lua(httpd_req_t *request, const char *save_as, unsigned timeout_ms, bool cli)
{
    const size_t length = request->content_len;
    if (!length || length > BOARD_LUA_MAX_SOURCE_BYTES) {
        return bad_request(request, "Send 1 to 8192 bytes of Lua source");
    }
    char *source = malloc(length);
    if (!source) return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "Lua source allocation failed");
    size_t used = 0;
    while (used < length) {
        const int received = httpd_req_recv(request, source + used, length - used);
        if (received <= 0) {
            free(source);
            return bad_request(request, "Incomplete Lua source");
        }
        used += received;
    }
    const esp_err_t result = save_as ? save_lua(request, save_as, source, length, !cli)
                                    : respond_lua(request, source, length, timeout_ms, cli);
    free(source);
    return result;
}

static esp_err_t run_lua(httpd_req_t *request, bool lease_holder)
{
    if (!lease_holder) return control_error(request, CONTROL_LEASE_DENIED);
    unsigned timeout_ms = BOARD_LUA_DEFAULT_TIMEOUT_MS;
    if (httpd_req_get_url_query_len(request)) {
        char query[40];
        if (httpd_req_get_url_query_str(request, query, sizeof(query)) != ESP_OK ||
            !form_unsigned(query, "timeout_ms", BOARD_LUA_MIN_TIMEOUT_MS, BOARD_LUA_MAX_TIMEOUT_MS, &timeout_ms)) {
            return bad_request(request, "Lua timeout_ms must be between 100 and 10000");
        }
    }
    return receive_lua(request, NULL, timeout_ms, true);
}

static bool script_query_name(const char *query, char name[SCRIPT_STORE_NAME_MAX + 1])
{
    return httpd_query_key_value(query, "name", name, SCRIPT_STORE_NAME_MAX + 1) == ESP_OK &&
           script_store_validate_name(name);
}

static bool script_query(httpd_req_t *request, char *query, size_t query_size,
                         char name[SCRIPT_STORE_NAME_MAX + 1])
{
    return httpd_req_get_url_query_str(request, query, query_size) == ESP_OK &&
           script_query_name(query, name);
}

static esp_err_t list_scripts(httpd_req_t *request)
{
    if (!authorized(request)) return require_key(request);
    char *response = NULL;
    esp_err_t result = script_store_list(&response);
    if (result != ESP_OK) return script_storage_error(request, result);
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    result = json(request, response);
    free(response);
    return result;
}

static esp_err_t upload_script(httpd_req_t *request, bool lease_holder)
{
    if (!lease_holder) return control_error(request, CONTROL_LEASE_DENIED);
    char query[112], name[SCRIPT_STORE_NAME_MAX + 1];
    if (!script_query(request, query, sizeof(query), name)) return bad_request(request, "Invalid Lua script name");
    return receive_lua(request, name, 0, true);
}

static esp_err_t run_script(httpd_req_t *request, bool lease_holder)
{
    if (!lease_holder) return control_error(request, CONTROL_LEASE_DENIED);
    char query[112], name[SCRIPT_STORE_NAME_MAX + 1];
    unsigned timeout_ms;
    if (!script_query(request, query, sizeof(query), name)) return bad_request(request, "Invalid Lua script name");
    if (!lua_timeout(query, &timeout_ms)) return bad_request(request, "Lua timeout_ms must be between 100 and 10000");
    if (request->content_len) return bad_request(request, "Stored script run requires an empty body");
    char *source = NULL;
    size_t length = 0;
    esp_err_t result = script_store_read(name, &source, &length);
    if (result != ESP_OK) return script_storage_error(request, result);
    result = respond_lua(request, source, length, timeout_ms, true);
    free(source);
    return result;
}

static esp_err_t get_script_source(httpd_req_t *request)
{
    if (!authorized(request)) return require_key(request);
    char query[112], name[SCRIPT_STORE_NAME_MAX + 1];
    if (!script_query(request, query, sizeof(query), name)) return bad_request(request, "Invalid Lua script name");

    char *source = NULL;
    size_t length = 0;
    const esp_err_t read = script_store_read(name, &source, &length);
    if (read != ESP_OK) return script_storage_error(request, read);

    char tag[SCRIPT_ETAG_SIZE];
    esp_err_t result;
    if (script_etag(source, length, tag)) {
        httpd_resp_set_hdr(request, "Cache-Control", "no-store");
        httpd_resp_set_hdr(request, "X-Content-Type-Options", "nosniff");
        httpd_resp_set_hdr(request, "ETag", tag);
        httpd_resp_set_type(request, "text/plain; charset=utf-8");
        result = httpd_resp_send(request, source, length);
    } else {
        result = script_etag_error(request);
    }
    free(source);
    return result;
}

static esp_err_t save_lua_from_workbench(httpd_req_t *request, const char *name)
{
    char expected[SCRIPT_ETAG_SIZE], none[2];
    const esp_err_t match = httpd_req_get_hdr_value_str(request, "If-Match", expected, sizeof(expected));
    const esp_err_t no_match = httpd_req_get_hdr_value_str(request, "If-None-Match", none, sizeof(none));
    const bool create = match == ESP_ERR_NOT_FOUND && no_match == ESP_OK && strcmp(none, "*") == 0;
    const bool update = match == ESP_OK && no_match == ESP_ERR_NOT_FOUND;
    if (!create && !update) return bad_request(request, "Supply an edit version or If-None-Match: *");

    // The mutation gate stays held from this read through the atomic write.
    char *current = NULL;
    size_t length = 0;
    const esp_err_t read = script_store_read(name, &current, &length);
    if (read != ESP_OK && read != ESP_ERR_NOT_FOUND) return script_storage_error(request, read);
    if (create && read == ESP_ERR_NOT_FOUND) return receive_lua(request, name, 0, false);

    bool same_version = false;
    if (update && read == ESP_OK) {
        char tag[SCRIPT_ETAG_SIZE];
        const bool tagged = script_etag(current, length, tag);
        free(current);
        if (!tagged) return script_etag_error(request);
        same_version = strcmp(expected, tag) == 0;
    } else {
        free(current);
    }
    if (same_version) return receive_lua(request, name, 0, false);

    httpd_resp_set_status(request, "412 Precondition Failed");
    return httpd_resp_sendstr(request, "The script already exists or has changed. Reopen it to see the saved version.");
}

static esp_err_t workbench_lua(httpd_req_t *request, bool lease_holder)
{
    if (lease_holder) return control_error(request, CONTROL_LEASE_DENIED);
    char query[112], operation[5];
    if (httpd_req_get_url_query_str(request, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "op", operation, sizeof(operation)) != ESP_OK) {
        return bad_request(request, "Expected a Lua workbench operation");
    }
    if (strcmp(operation, "save") == 0) {
        char name[SCRIPT_STORE_NAME_MAX + 1];
        if (!script_query_name(query, name)) return bad_request(request, "Invalid Lua script name");
        return save_lua_from_workbench(request, name);
    }
    if (strcmp(operation, "run") == 0) {
        unsigned timeout_ms;
        if (!lua_timeout(query, &timeout_ms)) return bad_request(request, "Lua timeout_ms must be between 100 and 10000");
        return receive_lua(request, NULL, timeout_ms, false);
    }
    return bad_request(request, "Unknown Lua workbench operation");
}

static esp_err_t web_status_response(httpd_req_t *request, const web_store_state_t *state)
{
    char response[48];
    snprintf(response, sizeof(response), "{\"active\":\"%s\"}", state->active);
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return json(request, response);
}

static esp_err_t web_change_error(httpd_req_t *request, esp_err_t error)
{
    if (error == ESP_ERR_INVALID_SIZE || error == ESP_ERR_INVALID_ARG || error == ESP_ERR_INVALID_CRC) {
        return bad_request(request, "Invalid web bundle size, format, or SHA-256");
    }
    if (error == ESP_ERR_TIMEOUT) {
        return httpd_resp_send_err(request, HTTPD_408_REQ_TIMEOUT, "Incomplete web bundle");
    }
    if (error == ESP_ERR_NOT_FINISHED) {
        httpd_resp_set_status(request, "507 Insufficient Storage");
        return httpd_resp_sendstr(request, "LittleFS has insufficient space for a complete staged web bundle");
    }
    if (error == ESP_ERR_INVALID_STATE) {
        httpd_resp_set_status(request, "409 Conflict");
        return httpd_resp_sendstr(request, "Web version is already active");
    }
    return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(error));
}

static esp_err_t web_status_unavailable(httpd_req_t *request)
{
    httpd_resp_set_status(request, "503 Service Unavailable");
    return httpd_resp_sendstr(request, "LittleFS web storage is unavailable");
}

static esp_err_t get_web_status(httpd_req_t *request)
{
    if (!authorized(request)) return require_key(request);
    web_store_state_t state;
    if (web_store_status(&state) != ESP_OK) return web_status_unavailable(request);
    return web_status_response(request, &state);
}

static int receive_web_bundle(void *context, void *buffer, size_t capacity)
{
    return httpd_req_recv(context, buffer, capacity);
}

static esp_err_t upload_web(httpd_req_t *request, bool lease_holder)
{
    if (!lease_holder) return control_error(request, CONTROL_LEASE_DENIED);
    web_store_state_t state;
    if (web_store_status(&state) != ESP_OK) return web_status_unavailable(request);
    const esp_err_t result = web_store_install(request->content_len, receive_web_bundle, request, &state);
    board_event_record(BOARD_EVENT_WEB_PUBLISH, result, request->content_len);
    return result == ESP_OK ? web_status_response(request, &state) : web_change_error(request, result);
}

static void escape_ssid(char escaped[193], const uint8_t ssid[33])
{
    char *out = escaped;
    for (size_t i = 0; i < 32 && ssid[i] != 0; ++i) {
        uint8_t c = ssid[i];
        if (c < 32) {
            snprintf(out, 7, "\\u%04x", (unsigned)c);
            out += 6;
        } else {
            if (c == '"' || c == '\\') *out++ = '\\';
            *out++ = (char)c;
        }
    }
    *out = 0;
}

void board_status_json(char *response, size_t capacity)
{
    esp_netif_ip_info_t ip = {0};
    esp_netif_get_ip_info(station, &ip);
    wifi_ap_record_t connected = {0};
    esp_wifi_sta_get_ap_info(&connected);
    char ssid[193];
    escape_ssid(ssid, connected.ssid);
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_app_desc_t *app = esp_app_get_description();
    esp_ota_img_states_t state;
    bool pending = esp_ota_get_state_partition(running, &state) == ESP_OK &&
                   state == ESP_OTA_IMG_PENDING_VERIFY;
    char sha[65];
    for (size_t i = 0; i < sizeof(app->app_elf_sha256); ++i) {
        snprintf(sha + 2 * i, 3, "%02x", app->app_elf_sha256[i]);
    }
    snprintf(response, capacity,
             "{\"project\":\"ai_board\",\"built\":\"%s %s\",\"partition\":\"%s\","
             "\"app_sha256\":\"%s\",\"ota_pending\":%s,\"ap_ssid\":\"%s\",\"station_ssid\":\"%s\","
             "\"station_ip\":\"" IPSTR "\",\"station_netmask\":\"" IPSTR "\",\"matrix_armed\":%s}",
             app->date, app->time, running->label, sha, pending ? "true" : "false",
             ap_name, ssid, IP2STR(&ip.ip), IP2STR(&ip.netmask), atomic_load(&matrix_armed) ? "true" : "false");
}

static esp_err_t status(httpd_req_t *request)
{
    char response[512];
    board_status_json(response, sizeof(response));
    return json(request, response);
}

static esp_err_t diagnostics(httpd_req_t *request)
{
    if (!authorized(request)) return require_key(request);
    wifi_ap_record_t access_point = {0};
    esp_err_t connected = esp_wifi_sta_get_ap_info(&access_point);
    const esp_app_desc_t *app = esp_app_get_description();
    char response[256];
    snprintf(response, sizeof(response),
             "{\"version\":\"%s\",\"uptime_ms\":%" PRId64 ",\"reset_reason\":%u,"
             "\"free_heap_bytes\":%" PRIu32 ",\"minimum_free_heap_bytes\":%" PRIu32 ","
             "\"station_connected\":%s,\"rssi_dbm\":%d,\"wifi_channel\":%u}",
             app->version, esp_timer_get_time() / 1000, (unsigned)esp_reset_reason(),
             esp_get_free_heap_size(), esp_get_minimum_free_heap_size(),
             connected == ESP_OK ? "true" : "false",
             access_point.rssi, access_point.primary);
    return json(request, response);
}

static bool read_log_parameter(const char *query, const char *key, uint32_t *number, bool *present)
{
    char text[11];
    const esp_err_t found = httpd_query_key_value(query, key, text, sizeof(text));
    *present = found != ESP_ERR_NOT_FOUND;
    if (!*present) return true;
    if (found != ESP_OK || !text[0]) return false;
    uint32_t value = 0;
    for (const char *p = text; *p; ++p) {
        if (*p < '0' || *p > '9') return false;
        const unsigned digit = *p - '0';
        if (value > (UINT32_MAX - digit) / 10) return false;
        value = value * 10 + digit;
    }
    *number = value;
    return true;
}

static esp_err_t get_logs(httpd_req_t *request)
{
    if (!authorized(request)) return require_key(request);
    char query[48] = "";
    uint32_t after = 0, limit = 16;
    bool incremental, limit_present;
    if ((httpd_req_get_url_query_len(request) &&
         httpd_req_get_url_query_str(request, query, sizeof(query)) != ESP_OK) ||
        !read_log_parameter(query, "limit", &limit, &limit_present) || limit == 0 || limit > BOARD_EVENT_CAPACITY ||
        !read_log_parameter(query, "after", &after, &incremental)) {
        return bad_request(request, "Expected limit 1-64 and optional uint32 after");
    }
    board_event_t *entries = malloc(limit * sizeof(*entries));
    if (!entries) return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "Log snapshot allocation failed");

    uint32_t latest, dropped;
    const size_t count = board_events_read(after, incremental, limit, entries, &latest, &dropped);
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    char chunk[160];
    snprintf(chunk, sizeof(chunk), "{\"latest\":%" PRIu32 ",\"dropped\":%" PRIu32 ",\"events\":[", latest, dropped);
    esp_err_t result = httpd_resp_sendstr_chunk(request, chunk);
    for (size_t i = 0; i < count && result == ESP_OK; ++i) {
        const board_event_t *entry = &entries[i];
        snprintf(chunk, sizeof(chunk), "%s{\"seq\":%" PRIu32 ",\"uptime_ms\":%" PRIu64
                 ",\"kind\":\"%s\",\"code\":%" PRId32 ",\"detail\":%" PRIu32 "}",
                 i ? "," : "", entry->seq, entry->uptime_ms, board_event_kind_name(entry->kind),
                 entry->code, entry->detail);
        result = httpd_resp_sendstr_chunk(request, chunk);
    }
    free(entries);
    if (result == ESP_OK) result = httpd_resp_sendstr_chunk(request, "]}");
    return result == ESP_OK ? httpd_resp_send_chunk(request, NULL, 0) : result;
}

esp_err_t board_scan_json(char *response, size_t capacity)
{
    if (xSemaphoreTake(scan_lock, 0) != pdTRUE) return ESP_ERR_INVALID_STATE;
    wifi_scan_config_t config = {.show_hidden = false};
    esp_err_t result = esp_wifi_scan_start(&config, true);
    static wifi_ap_record_t networks[12];
    uint16_t count = 12;
    if (result == ESP_OK) result = esp_wifi_scan_get_ap_records(&count, networks);
    if (result == ESP_OK) {
        int written = snprintf(response, capacity, "{\"networks\":[");
        size_t used = written;
        for (uint16_t i = 0; i < count; ++i) {
            char ssid[193];
            escape_ssid(ssid, networks[i].ssid);
            written = snprintf(response + used, capacity - used,
                               "%s{\"ssid\":\"%s\",\"channel\":%u,\"rssi\":%d,\"auth\":%u}",
                               i ? "," : "", ssid, networks[i].primary, networks[i].rssi, networks[i].authmode);
            if (written < 0 || (size_t)written + 3 > capacity - used) {
                result = ESP_ERR_INVALID_SIZE;
                break;
            }
            used += written;
        }
        if (result == ESP_OK) snprintf(response + used, capacity - used, "]}");
    }
    xSemaphoreGive(scan_lock);
    return result;
}

static esp_err_t scan_wifi(httpd_req_t *request, bool lease_holder)
{
    (void)lease_holder;
    char response[3072];
    esp_err_t result = board_scan_json(response, sizeof(response));
    if (result != ESP_OK) return bad_request(request, esp_err_to_name(result));
    return json(request, response);
}

static esp_err_t index_page(httpd_req_t *request)
{
    static const char page[] =
        "<!doctype html><html lang='en'><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>AI Board Wi-Fi setup</title><h1>AI Board Wi-Fi setup</h1>"
        "<p id='control' role='status'></p>"
        "<form><fieldset disabled><p><label>Network name <input name='ssid' required maxlength='32'></label></p>"
        "<p><label>Wi-Fi password <input name='password' type='password' maxlength='63'></label></p>"
        "<p><label>Board access key <input name='key' type='password' required></label></p>"
        "<button>Save and restart</button></fieldset></form><p id='result' role='status'></p>"
        "<script>const fields=document.querySelector('fieldset'),control=document.getElementById('control');"
        "async function refresh(){try{const r=await fetch('/api/control',{cache:'no-store'});"
        "if(!r.ok)throw Error();const s=await r.json();fields.disabled=s.active;"
        "control.textContent=s.active?'AI is in control. This page is read-only.':'';}"
        "catch{fields.disabled=true;control.textContent='Board control status unavailable.';}}"
        "refresh();setInterval(refresh,3000);"
        "document.querySelector('form').onsubmit=async(e)=>{e.preventDefault();if(fields.disabled)return;"
        "const f=new FormData(e.target),key=f.get('key');f.delete('key');"
        "const result=document.getElementById('result');"
        "try{const r=await fetch('/api/wifi',{method:'POST',headers:{'X-Board-Key':key},"
        "body:new URLSearchParams(f)});result.textContent=r.ok?"
        "'Saved. Reconnect to the board AP and open /api/status to see its LAN IP.':await r.text();}"
        "catch{result.textContent='Connection lost. Reconnect and check /api/status.';}"
        "refresh();};</script></html>";
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    return httpd_resp_sendstr(request, page);
}

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool decode_form(char *value)
{
    char *out = value;
    for (const char *in = value; *in; ++in) {
        if (*in == '%') {
            if (!in[1] || !in[2]) return false;
            int hi = hex_digit(in[1]), lo = hex_digit(in[2]);
            if (hi < 0 || lo < 0 || (hi == 0 && lo == 0)) return false;
            *out++ = (char)((hi << 4) | lo);
            in += 2;
        } else {
            *out++ = *in == '+' ? ' ' : *in;
        }
    }
    *out = 0;
    return true;
}

esp_err_t board_save_wifi(const char *ssid, const char *password)
{
    size_t ssid_length = strlen(ssid), password_length = strlen(password);
    if (ssid_length == 0 || ssid_length > 32 ||
        (password_length != 0 && (password_length < 8 || password_length > 63))) {
        return ESP_ERR_INVALID_ARG;
    }
    wifi_config_t config = {0};
    memcpy(config.sta.ssid, ssid, ssid_length);
    memcpy(config.sta.password, password, password_length);
    xSemaphoreTake(settings_lock, portMAX_DELAY);
    esp_err_t result = nvs_set_blob(storage, "station", &config.sta, sizeof(config.sta));
    if (result == ESP_OK) result = nvs_commit(storage);
    xSemaphoreGive(settings_lock);
    board_event_record(BOARD_EVENT_WIFI_CONFIG, result, 0);
    return result;
}

static esp_err_t configure_wifi(httpd_req_t *request, bool lease_holder)
{
    (void)lease_holder;
    char body[384], ssid[97], password[190];
    if (request->content_len == 0 || request->content_len >= sizeof(body)) {
        return bad_request(request, "Expected URL-encoded ssid and password");
    }
    size_t received = 0;
    while (received < request->content_len) {
        int count = httpd_req_recv(request, body + received, request->content_len - received);
        if (count <= 0) return bad_request(request, "Incomplete Wi-Fi configuration");
        received += count;
    }
    body[received] = 0;
    if (httpd_query_key_value(body, "ssid", ssid, sizeof(ssid)) != ESP_OK ||
        httpd_query_key_value(body, "password", password, sizeof(password)) != ESP_OK ||
        !decode_form(ssid) || !decode_form(password)) {
        return bad_request(request, "Invalid Wi-Fi name or password");
    }
    esp_err_t result = board_save_wifi(ssid, password);
    if (result == ESP_ERR_INVALID_ARG) return bad_request(request, "Invalid Wi-Fi name or password length");
    if (result != ESP_OK) return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "Save failed");
    result = json(request, "{\"saved\":true,\"rebooting\":true}");
    board_schedule_reboot();
    return result;
}

#ifdef CONFIG_AI_BOARD_OUT_CONTROL
bool board_out_grounded(void)
{
    return gpio_get_level(GPIO_NUM_0) != 0;
}

esp_err_t board_set_out_grounded(bool grounded)
{
    // GPIO0 drives Q1 (disable U5) and Q3 (discharge OUT through R9).
    esp_err_t result = gpio_set_level(GPIO_NUM_0, grounded);
    return result == ESP_OK ? gpio_set_direction(GPIO_NUM_0, GPIO_MODE_INPUT_OUTPUT) : result;
}

#endif

void board_matrix_arm(void)
{
    atomic_store(&matrix_armed, true);
    board_event_record(BOARD_EVENT_MATRIX_ARM, ESP_OK, 0);
}

bool board_matrix_is_armed(void)
{
    return atomic_load(&matrix_armed);
}

bool board_matrix_acquire(void)
{
    return xSemaphoreTake(matrix_lock, 0) == pdTRUE;
}

void board_matrix_release(void)
{
    xSemaphoreGive(matrix_lock);
}

esp_err_t board_matrix_run(int *failures)
{
    if (!board_matrix_is_armed()) return ESP_ERR_INVALID_STATE;
    if (!board_matrix_acquire()) return ESP_ERR_TIMEOUT;
    if (matrix_has_routes()) {
        board_matrix_release();
        return ESP_ERR_INVALID_STATE;
    }
    *failures = matrix_selftest();
    board_matrix_release();
    board_event_record(BOARD_EVENT_MATRIX_SELFTEST, *failures ? ESP_FAIL : ESP_OK, *failures);
    return ESP_OK;
}

static esp_err_t arm_matrix(httpd_req_t *request, bool lease_holder)
{
    (void)lease_holder;
    char confirmation[8];
    if (httpd_req_get_hdr_value_str(request, "X-DTR-Disconnected", confirmation, sizeof(confirmation)) != ESP_OK ||
        strcmp(confirmation, "yes") != 0) {
        return bad_request(request, "Disconnect DTR from GPIO9 before arming");
    }
    board_matrix_arm();
    return json(request, "{\"matrix_armed\":true}");
}

static esp_err_t run_matrix(httpd_req_t *request, bool lease_holder)
{
    (void)lease_holder;
    int failures;
    esp_err_t result = board_matrix_run(&failures);
    if (result == ESP_ERR_INVALID_STATE) return bad_request(request, "Arm the matrix and clear all routes before selftest");
    if (result != ESP_OK) {
        httpd_resp_set_status(request, "503 Service Unavailable");
        return httpd_resp_sendstr(request, "Matrix is busy");
    }
    char response[96];
    snprintf(response, sizeof(response), "{\"passed\":%d,\"failed\":%d,\"total\":448}",
             448 - failures, failures);
    return json(request, response);
}

static esp_err_t get_workbench(httpd_req_t *request)
{
    return authorized(request) ? workbench_state(request) : require_key(request);
}

static esp_err_t get_workbench_result(httpd_req_t *request)
{
    return authorized(request) ? workbench_result(request) : require_key(request);
}

static esp_err_t update_firmware(httpd_req_t *request, bool lease_holder)
{
    (void)lease_holder;
    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    if (!target || request->content_len == 0 || request->content_len > target->size) {
        return bad_request(request, "Firmware image does not fit OTA slot");
    }
    esp_ota_handle_t handle;
    esp_err_t result = esp_ota_begin(target, request->content_len, &handle);
    if (result != ESP_OK) return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(result));
    char buffer[1024];
    size_t remaining = request->content_len;
    while (remaining) {
        size_t length = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
        int got = httpd_req_recv(request, buffer, length);
        if (got <= 0) {
            esp_ota_abort(handle);
            return bad_request(request, "Incomplete firmware upload");
        }
        result = esp_ota_write(handle, buffer, got);
        if (result != ESP_OK) {
            esp_ota_abort(handle);
            return bad_request(request, esp_err_to_name(result));
        }
        remaining -= got;
    }
    result = esp_ota_end(handle);
    if (result != ESP_OK) return bad_request(request, esp_err_to_name(result));
    result = esp_ota_set_boot_partition(target);
    if (result != ESP_OK) return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(result));
    board_event_record(BOARD_EVENT_OTA, ESP_OK, request->content_len);
    result = json(request, "{\"accepted\":true,\"rebooting\":true}");
    board_schedule_reboot();
    return result;
}

static void connect_station(void *context)
{
    esp_wifi_connect();
}

static void station_event(void *context, esp_event_base_t base, int32_t id, void *event)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        connect_station(NULL);
    } else if (base == WIFI_EVENT) {
        wifi_event_sta_disconnected_t *disconnected = event;
        board_event_record(BOARD_EVENT_WIFI_DOWN, disconnected->reason, 0);
        printf("Wi-Fi disconnected: reason=%u\n", disconnected->reason);
        esp_timer_start_once(wifi_retry_timer, 5000000);
    } else {
        ip_event_got_ip_t *connected = event;
        wifi_ap_record_t station_ap = {0};
        esp_wifi_sta_get_ap_info(&station_ap);
        board_event_record(BOARD_EVENT_WIFI_UP, ESP_OK, station_ap.primary);
        printf("LAN IP: " IPSTR "\n", IP2STR(&connected->ip_info.ip));
    }
}

static void load_access_key(void)
{
    size_t length = sizeof(access_key);
    esp_err_t result = nvs_get_str(storage, "access_key", access_key, &length);
    if (result == ESP_ERR_NVS_NOT_FOUND) {
        uint8_t random[12];
        esp_fill_random(random, sizeof(random));
        for (size_t i = 0; i < sizeof(random); ++i) {
            snprintf(access_key + 2 * i, 3, "%02x", random[i]);
        }
        ESP_ERROR_CHECK(nvs_set_str(storage, "access_key", access_key));
        ESP_ERROR_CHECK(nvs_commit(storage));
    } else {
        ESP_ERROR_CHECK(result);
    }
}

void app_main(void)
{
    board_event_record(BOARD_EVENT_BOOT, esp_reset_reason(), 0);
    ESP_ERROR_CHECK(gpio_set_direction(GPIO_NUM_9, GPIO_MODE_INPUT));
    matrix_boot_reset();
#ifdef CONFIG_AI_BOARD_OUT_CONTROL
    ESP_ERROR_CHECK(gpio_set_pull_mode(GPIO_NUM_0, GPIO_FLOATING));
    ESP_ERROR_CHECK(gpio_set_direction(GPIO_NUM_0, GPIO_MODE_INPUT));
#endif
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(nvs_open("ai_board", NVS_READWRITE, &storage));
    const esp_err_t scripts = script_store_init();
    if (scripts != ESP_OK) printf("Script storage unavailable: %s\n", esp_err_to_name(scripts));
    const esp_err_t web_storage = web_store_init();
    if (web_storage != ESP_OK) printf("Web storage unavailable: %s\n", esp_err_to_name(web_storage));
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();
    station = esp_netif_create_default_wifi_sta();
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_wifi_get_mac(WIFI_IF_AP, mac));
    snprintf(ap_name, sizeof(ap_name), "AI-Board-%02X%02X%02X", mac[3], mac[4], mac[5]);
    // Start the radio for entropy before bringing up the password-protected AP.
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    load_access_key();
    ESP_ERROR_CHECK(esp_wifi_stop());

    matrix_lock = xSemaphoreCreateMutex();
    settings_lock = xSemaphoreCreateMutex();
    scan_lock = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(matrix_lock && settings_lock && scan_lock ? ESP_OK : ESP_ERR_NO_MEM);
    const esp_timer_create_args_t timer_args = {.callback = reboot, .name = "reboot"};
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &reboot_timer));
    const esp_timer_create_args_t retry_args = {.callback = connect_station, .name = "wifi_retry"};
    ESP_ERROR_CHECK(esp_timer_create(&retry_args, &wifi_retry_timer));

    wifi_config_t stored = {0};
    size_t length = sizeof(stored.sta);
    bool configured = nvs_get_blob(storage, "station", &stored.sta, &length) == ESP_OK &&
                      length == sizeof(stored.sta) && stored.sta.ssid[0] != 0;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    if (configured) {
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &stored));
        ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_START, station_event, NULL));
        ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, station_event, NULL));
        ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, station_event, NULL));
    }
    wifi_config_t ap = {0};
    memcpy(ap.ap.ssid, ap_name, strlen(ap_name));
    memcpy(ap.ap.password, access_key, strlen(access_key));
    ap.ap.ssid_len = strlen(ap_name);
    ap.ap.channel = 6;
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap.ap.max_connection = 4;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_start());
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 9216;
    config.max_uri_handlers = 27;
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.lru_purge_enable = true;
    httpd_handle_t server = NULL;
    ESP_ERROR_CHECK(httpd_start(&server, &config));
    const httpd_uri_t routes[] = {
        {.uri = "/", .method = HTTP_GET, .handler = web_index},
        {.uri = "/recovery", .method = HTTP_GET, .handler = web_recovery},
        {.uri = "/style.css", .method = HTTP_GET, .handler = web_style},
        {.uri = "/app.js", .method = HTTP_GET, .handler = web_script},
        {.uri = "/_web/*", .method = HTTP_GET, .handler = web_version_asset},
        {.uri = "/setup", .method = HTTP_GET, .handler = index_page},
        {.uri = "/api/workbench", .method = HTTP_GET, .handler = get_workbench},
        {.uri = "/api/workbench/result", .method = HTTP_GET, .handler = get_workbench_result},
        {.uri = "/api/scripts", .method = HTTP_GET, .handler = list_scripts},
        {.uri = "/api/scripts/source", .method = HTTP_GET, .handler = get_script_source},
        {.uri = "/api/web", .method = HTTP_GET, .handler = get_web_status},
        {.uri = "/api/status", .method = HTTP_GET, .handler = status},
        {.uri = "/api/diagnostics", .method = HTTP_GET, .handler = diagnostics},
        {.uri = "/api/logs", .method = HTTP_GET, .handler = get_logs},
        {.uri = "/api/control", .method = HTTP_GET, .handler = get_control},
        {.uri = "/api/control", .method = HTTP_POST, .handler = post_control},
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); ++i) {
        ESP_ERROR_CHECK(httpd_register_uri_handler(server, &routes[i]));
    }
    static const mutation_route_t mutations[] = {
        {"/api/workbench", workbench_command},
        {"/api/workbench/lua", workbench_lua},
        {"/api/lua", run_lua},
        {"/api/scripts", upload_script},
        {"/api/scripts/run", run_script},
        {"/api/web", upload_web},
        {"/api/wifi", configure_wifi},
        {"/api/wifi/scan", scan_wifi},
        {"/api/matrix/arm", arm_matrix},
        {"/api/matrix/selftest", run_matrix},
        {"/api/ota", update_firmware},
    };
    for (size_t i = 0; i < sizeof(mutations) / sizeof(mutations[0]); ++i) {
        const httpd_uri_t route = {
            .uri = mutations[i].uri, .method = HTTP_POST, .handler = post_mutation,
            .user_ctx = (void *)&mutations[i],
        };
        ESP_ERROR_CHECK(httpd_register_uri_handler(server, &route));
    }
    ESP_ERROR_CHECK(board_console_start());
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) == ESP_OK && state == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_ERROR_CHECK(esp_ota_mark_app_valid_cancel_rollback());
    }
    printf("RPC READY: AP %s at http://" AP_IP "/, partition=%s\n", ap_name, running->label);
    printf("AP password and X-Board-Key: %s\n", access_key);
}

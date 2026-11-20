#include "web.h"

#include <stdio.h>
#include <string.h>

#include "web_store.h"

extern const char index_html_gz_start[] asm("_binary_index_html_gz_start");
extern const char index_html_gz_end[] asm("_binary_index_html_gz_end");
extern const char style_css_gz_start[] asm("_binary_style_css_gz_start");
extern const char style_css_gz_end[] asm("_binary_style_css_gz_end");
extern const char app_js_gz_start[] asm("_binary_app_js_gz_start");
extern const char app_js_gz_end[] asm("_binary_app_js_gz_end");

static esp_err_t send_asset(httpd_req_t *request, const char *type, const char *start, const char *end)
{
    httpd_resp_set_type(request, type);
    httpd_resp_set_hdr(request, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    httpd_resp_set_hdr(request, "X-Content-Type-Options", "nosniff");
    return httpd_resp_send(request, start, end - start);
}

static esp_err_t send_web_file(httpd_req_t *request, const char *type, FILE *file, size_t remaining)
{
    httpd_resp_set_type(request, type);
    httpd_resp_set_hdr(request, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    httpd_resp_set_hdr(request, "X-Content-Type-Options", "nosniff");

    char buffer[2048];
    esp_err_t result = ESP_OK;
    while (remaining > 0) {
        const size_t take = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
        if (fread(buffer, 1, take, file) != take) {
            result = ESP_FAIL;
            break;
        }
        result = httpd_resp_send_chunk(request, buffer, take);
        if (result != ESP_OK) break;
        remaining -= take;
    }
    fclose(file);
    return result == ESP_OK ? httpd_resp_send_chunk(request, NULL, 0) : result;
}

esp_err_t web_recovery(httpd_req_t *request)
{
    return send_asset(request, "text/html; charset=utf-8", index_html_gz_start, index_html_gz_end);
}

esp_err_t web_index(httpd_req_t *request)
{
    web_store_state_t state;
    FILE *file;
    size_t length;
    if (web_store_status(&state) == ESP_OK &&
        strcmp(state.active, WEB_STORE_EMBEDDED) != 0 &&
        web_store_open(state.active, WEB_STORE_INDEX, &file, &length) == ESP_OK) {
        return send_web_file(request, "text/html; charset=utf-8", file, length);
    }
    return web_recovery(request);
}

esp_err_t web_version_asset(httpd_req_t *request)
{
    static const char prefix[] = "/_web/";
    const char *uri = request->uri;
    const size_t path_length = strcspn(uri, "?");
    const size_t prefix_length = sizeof(prefix) - 1;
    const size_t asset_start = prefix_length + WEB_STORE_VERSION_LEN + 1;
    if (path_length < asset_start || strncmp(uri, prefix, prefix_length) != 0 || uri[asset_start - 1] != '/') {
        return httpd_resp_send_err(request, HTTPD_404_NOT_FOUND, "Web asset not found");
    }

    const char *name = uri + asset_start;
    const size_t name_length = path_length - asset_start;
    web_store_asset_t asset;
    const char *type;
    if (name_length == sizeof("style.css") - 1 && memcmp(name, "style.css", name_length) == 0) {
        asset = WEB_STORE_STYLE;
        type = "text/css; charset=utf-8";
    } else if (name_length == sizeof("app.js") - 1 && memcmp(name, "app.js", name_length) == 0) {
        asset = WEB_STORE_SCRIPT;
        type = "application/javascript; charset=utf-8";
    } else {
        return httpd_resp_send_err(request, HTTPD_404_NOT_FOUND, "Web asset not found");
    }

    char version[WEB_STORE_VERSION_LEN + 1];
    memcpy(version, uri + prefix_length, WEB_STORE_VERSION_LEN);
    version[WEB_STORE_VERSION_LEN] = '\0';
    FILE *file;
    size_t length;
    if (web_store_open(version, asset, &file, &length) != ESP_OK) {
        return httpd_resp_send_err(request, HTTPD_404_NOT_FOUND, "Web asset not found");
    }
    return send_web_file(request, type, file, length);
}

esp_err_t web_style(httpd_req_t *request)
{
    return send_asset(request, "text/css; charset=utf-8", style_css_gz_start, style_css_gz_end);
}

esp_err_t web_script(httpd_req_t *request)
{
    return send_asset(request, "application/javascript; charset=utf-8", app_js_gz_start, app_js_gz_end);
}

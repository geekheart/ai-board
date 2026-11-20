#pragma once

#include "esp_http_server.h"

esp_err_t web_index(httpd_req_t *request);
esp_err_t web_recovery(httpd_req_t *request);
esp_err_t web_version_asset(httpd_req_t *request);
esp_err_t web_style(httpd_req_t *request);
esp_err_t web_script(httpd_req_t *request);

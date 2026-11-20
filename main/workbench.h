#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_http_server.h"

esp_err_t workbench_state(httpd_req_t *request);
esp_err_t workbench_command(httpd_req_t *request, bool publish_result);
uint32_t workbench_result_revision(void);
esp_err_t workbench_result(httpd_req_t *request);
void workbench_publish_result(const char *operation, const char *response);

/* Shares the HTTP workbench operations. The caller holds the board lease.
 * On success the caller owns the returned JSON buffer. */
esp_err_t workbench_lua_command(const char *op, const char *form, uint32_t remaining_ms,
                                char **output, const char **diagnostic);

/* On success the caller owns the returned JSON buffer. */
esp_err_t workbench_serial_capture(const char *form, char **json, const char **diagnostic);

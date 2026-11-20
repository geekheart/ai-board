#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

enum { SCRIPT_STORE_NAME_MAX = 48 };

// Initialize once before starting the HTTP server, which serializes operations.
esp_err_t script_store_init(void);
bool script_store_validate_name(const char *name);
esp_err_t script_store_write(const char *name, const char *bytes, size_t size);

// On success, the caller owns the returned buffer.
esp_err_t script_store_read(const char *name, char **buffer, size_t *size);
esp_err_t script_store_list(char **json);

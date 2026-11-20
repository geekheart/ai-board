#pragma once

#include <stddef.h>
#include <stdio.h>

#include "esp_err.h"

#define WEB_STORE_VERSION_LEN 16
#define WEB_STORE_MAX_BUNDLE (96 * 1024)
#define WEB_STORE_EMBEDDED "embedded"

typedef enum {
    WEB_STORE_INDEX,
    WEB_STORE_STYLE,
    WEB_STORE_SCRIPT,
} web_store_asset_t;

typedef struct {
    char active[WEB_STORE_VERSION_LEN + 1];
} web_store_state_t;

// Positive byte count on success, 0 or negative on interrupted receipt.
typedef int (*web_store_receive_t)(void *context, void *buffer, size_t capacity);

// Initialize after the existing workspace is mounted. Runtime calls are
// serialized by the HTTP server.
esp_err_t web_store_init(void);
esp_err_t web_store_status(web_store_state_t *state);
esp_err_t web_store_install(size_t length, web_store_receive_t receive, void *context, web_store_state_t *state);

// On success the caller owns the file, positioned at the asset's first byte.
esp_err_t web_store_open(const char *version, web_store_asset_t asset, FILE **file, size_t *length);

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

enum {
    BOARD_LUA_MAX_SOURCE_BYTES = 8192,
    BOARD_LUA_MIN_TIMEOUT_MS = 100,
    BOARD_LUA_DEFAULT_TIMEOUT_MS = 5000,
    BOARD_LUA_MAX_TIMEOUT_MS = 10000,
};

// Takes a text script. Caller must already hold the board mutation gate.
// Successful transport owns the returned JSON string, including script errors.
esp_err_t board_lua_run(const char *source, size_t length, uint32_t timeout_ms, char **response);

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { BOARD_EVENT_CAPACITY = 64 };

typedef enum {
    BOARD_EVENT_BOOT,
    BOARD_EVENT_WIFI_UP,
    BOARD_EVENT_WIFI_DOWN,
    BOARD_EVENT_MATRIX_ARM,
    BOARD_EVENT_MATRIX_SELFTEST,
    BOARD_EVENT_CAPTURE,
    BOARD_EVENT_UART_RECEIVE,
    BOARD_EVENT_I2C_RECEIVE,
    BOARD_EVENT_SPI_RECEIVE,
    BOARD_EVENT_LUA,
    BOARD_EVENT_WEB_PUBLISH,
    BOARD_EVENT_OTA,
    BOARD_EVENT_WIFI_CONFIG,
} board_event_kind_t;

typedef struct {
    uint32_t seq;
    uint64_t uptime_ms;
    board_event_kind_t kind;
    int32_t code;
    uint32_t detail;
} board_event_t;

const char *board_event_kind_name(board_event_kind_t kind);
void board_event_record(board_event_kind_t kind, int32_t code, uint32_t detail);

// Returns entries in sequence order, either the latest entries or the first
// entries after the cursor. Dropped counts overwritten entries in that range.
// If the latest sequence is below the cursor, read again without the cursor.
size_t board_events_read(uint32_t after, bool incremental, size_t limit, board_event_t *entries,
                         uint32_t *latest, uint32_t *dropped);

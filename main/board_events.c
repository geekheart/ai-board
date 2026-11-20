#include "board_events.h"

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

static const char *const kind_names[] = {
    [BOARD_EVENT_BOOT] = "boot",
    [BOARD_EVENT_WIFI_UP] = "wifi_up",
    [BOARD_EVENT_WIFI_DOWN] = "wifi_down",
    [BOARD_EVENT_MATRIX_ARM] = "matrix_arm",
    [BOARD_EVENT_MATRIX_SELFTEST] = "matrix_selftest",
    [BOARD_EVENT_CAPTURE] = "capture",
    [BOARD_EVENT_UART_RECEIVE] = "uart_receive",
    [BOARD_EVENT_I2C_RECEIVE] = "i2c_receive",
    [BOARD_EVENT_SPI_RECEIVE] = "spi_receive",
    [BOARD_EVENT_LUA] = "lua",
    [BOARD_EVENT_WEB_PUBLISH] = "web_publish",
    [BOARD_EVENT_OTA] = "ota",
    [BOARD_EVENT_WIFI_CONFIG] = "wifi_config",
};

static portMUX_TYPE event_lock = portMUX_INITIALIZER_UNLOCKED;
static board_event_t events[BOARD_EVENT_CAPACITY];
static uint32_t last_sequence;

static bool valid_kind(board_event_kind_t kind)
{
    return (unsigned)kind < sizeof(kind_names) / sizeof(kind_names[0]);
}

const char *board_event_kind_name(board_event_kind_t kind)
{
    return valid_kind(kind) ? kind_names[kind] : "unknown";
}

void board_event_record(board_event_kind_t kind, int32_t code, uint32_t detail)
{
    if (!valid_kind(kind)) return;

    portENTER_CRITICAL(&event_lock);
    if (last_sequence == UINT32_MAX) last_sequence = 0;
    uint32_t seq = ++last_sequence;
    events[(seq - 1) % BOARD_EVENT_CAPACITY] = (board_event_t){
        .seq = seq,
        .uptime_ms = (uint64_t)(esp_timer_get_time() / 1000),
        .kind = kind,
        .code = code,
        .detail = detail,
    };
    portEXIT_CRITICAL(&event_lock);
}

size_t board_events_read(uint32_t after, bool incremental, size_t limit, board_event_t *entries,
                         uint32_t *latest, uint32_t *dropped)
{
    if (!entries) limit = 0;
    if (limit > BOARD_EVENT_CAPACITY) limit = BOARD_EVENT_CAPACITY;

    portENTER_CRITICAL(&event_lock);
    uint32_t end = last_sequence;
    uint32_t overwritten = end > BOARD_EVENT_CAPACITY ? end - BOARD_EVENT_CAPACITY : 0;
    uint32_t cursor = incremental ? after : 0;
    uint32_t start = cursor > overwritten ? cursor : overwritten;
    size_t count = start < end ? end - start : 0;
    if (count > limit) {
        if (!incremental) start = end - (uint32_t)limit;
        count = limit;
    }
    for (size_t i = 0; i < count; ++i) {
        entries[i] = events[(start + i) % BOARD_EVENT_CAPACITY];
    }
    if (latest) *latest = end;
    if (dropped) *dropped = cursor < overwritten ? overwritten - cursor : 0;
    portEXIT_CRITICAL(&event_lock);
    return count;
}

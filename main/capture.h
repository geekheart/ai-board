#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "driver/gpio.h"
#include "esp_err.h"

#define CAPTURE_MAX_CHANNELS 4
#define CAPTURE_MAX_EDGES 256
#define CAPTURE_MAX_UART_BYTES 64
#define CAPTURE_MIN_DURATION_MS 10
#define CAPTURE_MAX_DURATION_MS 500
#define CAPTURE_MIN_UART_BAUD 1200
#define CAPTURE_MAX_UART_BAUD 4000000
#define CAPTURE_MIN_WAIT_MS 100
#define CAPTURE_MAX_WAIT_MS 5000

typedef enum {
    CAPTURE_MANUAL,
    CAPTURE_RISING,
    CAPTURE_FALLING,
} capture_trigger_t;

typedef struct {
    uint32_t duration_ms;
    uint32_t baud_hint;
    capture_trigger_t trigger;
    size_t trigger_index;
    uint32_t wait_ms;
} capture_options_t;

typedef struct {
    uint32_t time_ns;
    bool level;
} capture_edge_t;

typedef struct {
    uint32_t window_ns;
    uint32_t resolution_ns;
    uint32_t baud;
    uint16_t edge_count;
    uint8_t uart_count;
    uint8_t framing_errors;
    bool initial_level;
    bool truncated;
    bool uart_truncated;
    capture_edge_t edges[CAPTURE_MAX_EDGES];
    uint8_t uart_bytes[CAPTURE_MAX_UART_BYTES];
} capture_channel_t;

/* Uses existing GPIO input/pull settings. Set baud_hint to zero for automatic detection. */
esp_err_t capture_run(const gpio_num_t *pins, size_t pin_count, uint32_t duration_ms,
                      uint32_t baud_hint, capture_channel_t *channels);

/* The selected trigger edge is recorded at t=0; return ESP_ERR_TIMEOUT if none arrives. */
esp_err_t capture_run_window(const gpio_num_t *pins, size_t pin_count,
                             const capture_options_t *options, capture_channel_t *channels);

void capture_decode_uart(capture_channel_t *channel, uint32_t baud_hint);

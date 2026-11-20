#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_err.h"

#define UART_BATCH_MIN_BAUD 9600
#define UART_BATCH_MAX_BAUD 5000000
#define UART_BATCH_MAX_BYTES 2048
#define UART_BATCH_MAX_WAIT_MS 10000

typedef struct {
    gpio_num_t rx;
    unsigned baud;
    uint16_t capacity;
    uint32_t wait_ms;
    uint32_t idle_ms;  // Zero waits for capacity or the overall deadline.
    uint8_t *data;    // Caller-owned buffer with room for capacity bytes.
    void (*armed)(void *);
    void *armed_context;
} uart_batch_capture_options_t;

typedef struct {
    unsigned actual_baud;
    size_t bytes;
    bool timed_out;
    bool idle;
    bool limit_reached;
    bool overflow;
    bool events_lost;
    unsigned frame_errors;
    unsigned parity_errors;
    unsigned break_events;
} uart_batch_capture_result_t;

// UART1 receives 8N1 and never routes its TX. The caller owns UART1 and the
// selected GPIO for the batch. Idle time is measured from delivered RX data
// at task/tick granularity. Timeout returns ESP_OK with any captured bytes.
esp_err_t uart_batch_capture_run(const uart_batch_capture_options_t *options,
                                 uart_batch_capture_result_t *result);

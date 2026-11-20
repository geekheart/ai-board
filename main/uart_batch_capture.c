#include "uart_batch_capture.h"

#include "driver/uart.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "soc/uart_reg.h"

enum {
    CAPTURE_UART = UART_NUM_1,
    RX_FIFO_THRESHOLD = 32,
    RX_TIMEOUT_SYMBOLS = 2,
    RX_EVENT_QUEUE_SIZE = 128,
};

static void count_event(const uart_event_t *event, uart_batch_capture_result_t *result)
{
    switch (event->type) {
    case UART_FRAME_ERR:
        ++result->frame_errors;
        break;
    case UART_PARITY_ERR:
        ++result->parity_errors;
        break;
    case UART_BREAK:
        ++result->break_events;
        break;
    case UART_FIFO_OVF:
    case UART_BUFFER_FULL:
        result->overflow = true;
        break;
    default:
        break;
    }
}

static bool take_event(QueueHandle_t events, TickType_t wait_ticks, uart_event_t *event,
                       uart_batch_capture_result_t *result)
{
    if (xQueuePeek(events, event, wait_ticks) != pdTRUE) return false;

    // The IDF UART ISR silently drops events when its queue is full. Check
    // before freeing a slot, with the UART ISR excluded from this interval.
    portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
    portENTER_CRITICAL(&lock);
    result->events_lost |= uxQueueSpacesAvailable(events) == 0;
    xQueueReceive(events, event, 0);
    portEXIT_CRITICAL(&lock);
    return true;
}

static esp_err_t drain_rx(const uart_batch_capture_options_t *options,
                          uart_batch_capture_result_t *result)
{
    const size_t remaining = options->capacity - result->bytes;
    if (!remaining) return ESP_OK;
    const int read = uart_read_bytes(CAPTURE_UART, options->data + result->bytes, remaining, 0);
    if (read < 0) return ESP_FAIL;
    result->bytes += read;
    return ESP_OK;
}

static esp_err_t receive_batch(const uart_batch_capture_options_t *options,
                               uart_batch_capture_result_t *result, QueueHandle_t events)
{
    const int64_t deadline = esp_timer_get_time() + (int64_t)options->wait_ms * 1000;
    int64_t end = deadline;
    if (options->armed) options->armed(options->armed_context);

    while (result->bytes < options->capacity) {
        const int64_t now = esp_timer_get_time();
        if (now >= deadline) {
            end = deadline;
            break;
        }
        const int64_t remaining_us = end > now ? end - now : 0;
        const int64_t tick_us = 1000000 / configTICK_RATE_HZ;
        const TickType_t wait_ticks = (remaining_us + tick_us - 1) / tick_us;
        uart_event_t event;
        if (!take_event(events, wait_ticks, &event, result)) {
            if (end == deadline || esp_timer_get_time() < end) continue;
            // The new burst may not have produced an interrupt yet.
            if (!REG_GET_FIELD(UART_STATUS_REG(CAPTURE_UART), UART_RXFIFO_CNT) &&
                !REG_GET_FIELD(UART_FSM_STATUS_REG(CAPTURE_UART), UART_ST_URX_OUT)) break;
            end = deadline;
            continue;
        }
        count_event(&event, result);

        esp_err_t status = drain_rx(options, result);
        if (status != ESP_OK) return status;
        if (options->idle_ms && event.type == UART_DATA && result->bytes) {
            // A watermark notification can happen during a continuous
            // burst. Only hardware RX timeout confirms the line paused.
            const int64_t idle_end = event.timeout_flag
                                     ? esp_timer_get_time() + (int64_t)options->idle_ms * 1000
                                     : deadline;
            end = idle_end < deadline ? idle_end : deadline;
        }
    }

    // Prevent further ISR events before collecting the final errors and
    // bytes that already reached the ring buffer.
    esp_err_t status = uart_disable_intr_mask(CAPTURE_UART, UINT32_MAX);
    if (status != ESP_OK) return status;
    uart_event_t event;
    while (take_event(events, 0, &event, result)) count_event(&event, result);
    const uint32_t pending = REG_READ(UART_INT_RAW_REG(CAPTURE_UART));
    if (pending & UART_FRM_ERR_INT_RAW_M) ++result->frame_errors;
    if (pending & UART_PARITY_ERR_INT_RAW_M) ++result->parity_errors;
    if (pending & UART_BRK_DET_INT_RAW_M) ++result->break_events;
    if (pending & UART_RXFIFO_OVF_INT_RAW_M) result->overflow = true;
    status = drain_rx(options, result);
    if (status != ESP_OK) return status;

    result->limit_reached = result->bytes == options->capacity;
    result->idle = !result->limit_reached && end < deadline;
    result->timed_out = !result->limit_reached && end == deadline;
    return ESP_OK;
}

esp_err_t uart_batch_capture_run(const uart_batch_capture_options_t *options,
                                 uart_batch_capture_result_t *result)
{
    if (!result) return ESP_ERR_INVALID_ARG;
    *result = (uart_batch_capture_result_t){0};
    if (!options || !GPIO_IS_VALID_GPIO(options->rx) || !options->data ||
        options->baud < UART_BATCH_MIN_BAUD || options->baud > UART_BATCH_MAX_BAUD ||
        !options->capacity || options->capacity > UART_BATCH_MAX_BYTES ||
        !options->wait_ms || options->wait_ms > UART_BATCH_MAX_WAIT_MS ||
        options->idle_ms > options->wait_ms) {
        return ESP_ERR_INVALID_ARG;
    }

    QueueHandle_t events = NULL;
    const int ring_size = options->capacity + UART_HW_FIFO_LEN(CAPTURE_UART);
    esp_err_t status = uart_driver_install(CAPTURE_UART, ring_size, 0, RX_EVENT_QUEUE_SIZE, &events, 0);
    if (status != ESP_OK) return status;

    const uart_config_t config = {
        .baud_rate = options->baud,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_APB,
    };
    status = uart_param_config(CAPTURE_UART, &config);
    if (status == ESP_OK) status = uart_set_rx_full_threshold(CAPTURE_UART, RX_FIFO_THRESHOLD);
    if (status == ESP_OK) status = uart_set_rx_timeout(CAPTURE_UART, RX_TIMEOUT_SYMBOLS);
    uint32_t actual_baud;
    if (status == ESP_OK) {
        status = uart_get_baudrate(CAPTURE_UART, &actual_baud);
        if (status == ESP_OK) result->actual_baud = actual_baud;
    }
    if (status == ESP_OK) {
        status = uart_set_pin(CAPTURE_UART, UART_PIN_NO_CHANGE, options->rx,
                              UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    }
    if (status == ESP_OK) status = gpio_set_pull_mode(options->rx, GPIO_PULLUP_ONLY);
    if (status == ESP_OK) status = uart_flush_input(CAPTURE_UART);
    if (status == ESP_OK) {
        uart_set_always_rx_timeout(CAPTURE_UART, true);
        status = uart_enable_intr_mask(CAPTURE_UART, UART_FRM_ERR_INT_ENA_M | UART_PARITY_ERR_INT_ENA_M);
    }
    if (status == ESP_OK) {
        xQueueReset(events);
        status = receive_batch(options, result, events);
    }

    const esp_err_t deleted = uart_driver_delete(CAPTURE_UART);
    gpio_set_direction(options->rx, GPIO_MODE_INPUT);
    gpio_set_pull_mode(options->rx, GPIO_FLOATING);
    return status == ESP_OK ? deleted : status;
}

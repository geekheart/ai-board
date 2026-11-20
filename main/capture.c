#include "capture.h"

#include <string.h>
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

typedef struct capture_session capture_session_t;

typedef struct {
    gpio_num_t pin;
    capture_channel_t *channel;
    capture_session_t *session;
    unsigned interrupt_count;
    bool previous_level;
} capture_input_t;

struct capture_session {
    capture_input_t inputs[CAPTURE_MAX_CHANNELS];
    size_t count;
    capture_input_t *trigger_input;
    SemaphoreHandle_t fired;
    int64_t start_us;
    int64_t trigger_deadline_us;
    uint32_t duration_us;
    bool trigger_level;
    bool started;
};

static void start_capture(capture_session_t *session, int64_t now)
{
    session->start_us = now;
    for (size_t i = 0; i < session->count; ++i) {
        capture_input_t *input = &session->inputs[i];
        bool level = input == session->trigger_input ? !session->trigger_level : gpio_get_level(input->pin);
        input->channel->initial_level = level;
        input->previous_level = level;
    }
    session->started = true;
}

static void capture_edge(void *arg)
{
    capture_input_t *input = arg;
    capture_session_t *session = input->session;
    capture_channel_t *channel = input->channel;
    int64_t now = esp_timer_get_time();
    bool level = gpio_get_level(input->pin);
    BaseType_t wake_task = pdFALSE;
    if (!session->started) {
        if (input != session->trigger_input || now >= session->trigger_deadline_us) {
            gpio_intr_disable(input->pin);
            return;
        }
        level = session->trigger_level;
        start_capture(session, now);
        gpio_set_intr_type(input->pin, GPIO_INTR_ANYEDGE);
        for (size_t i = 0; i < session->count; ++i) {
            if (&session->inputs[i] != input) gpio_intr_enable(session->inputs[i].pin);
        }
        xSemaphoreGiveFromISR(session->fired, &wake_task);
    }
    uint32_t elapsed = (uint32_t)(now - session->start_us);
    if (elapsed >= session->duration_us) {
        gpio_intr_disable(input->pin);
        return;
    }

    if (++input->interrupt_count > CAPTURE_MAX_EDGES * 2 ||
        (level != input->previous_level && channel->edge_count == CAPTURE_MAX_EDGES)) {
        channel->truncated = true;
        gpio_intr_disable(input->pin);
    } else if (level != input->previous_level) {
        channel->edges[channel->edge_count++] = (capture_edge_t){.time_ns = elapsed * 1000, .level = level};
        input->previous_level = level;
    }
    if (wake_task) portYIELD_FROM_ISR();
}

typedef struct {
    uint8_t bytes[CAPTURE_MAX_UART_BYTES];
    unsigned frames;
    unsigned errors;
    unsigned bit_intervals;
} uart_decode_t;

static bool sample_at(const capture_channel_t *channel, float time_ns, size_t *edge_index)
{
    while (*edge_index < channel->edge_count && channel->edges[*edge_index].time_ns <= time_ns) {
        ++*edge_index;
    }
    return *edge_index == 0 ? channel->initial_level : channel->edges[*edge_index - 1].level;
}

static uart_decode_t decode_uart(const capture_channel_t *channel, uint32_t baud)
{
    uart_decode_t result = {0};
    const float bit_ns = 1000000000.0f / baud;
    const float tolerance = channel->resolution_ns <= 250
                            ? 2.0f * channel->resolution_ns + bit_ns * .12f
                            : 1500.0f + bit_ns * .18f;

    size_t next = 0;
    while (next < channel->edge_count) {
        size_t start_index = next++;
        const capture_edge_t *start = &channel->edges[start_index];
        if (start->level) continue;

        const float stop_center = start->time_ns + 9.5f * bit_ns;
        if (stop_center >= channel->window_ns) break;

        uint8_t byte = 0;
        bool valid_start = !sample_at(channel, start->time_ns + 0.5f * bit_ns, &next);
        for (unsigned bit = 0; bit < 8; ++bit) {
            if (sample_at(channel, start->time_ns + (1.5f + bit) * bit_ns, &next)) {
                byte |= (uint8_t)(1U << bit);
            }
        }
        bool valid_stop = sample_at(channel, stop_center, &next);

        bool aligned = true;
        unsigned bit_intervals = 0;
        uint32_t last = start->time_ns;
        for (size_t i = start_index + 1; i < next; ++i) {
            const capture_edge_t *edge = &channel->edges[i];
            float delta = edge->time_ns - start->time_ns;
            unsigned bit_number = (unsigned)(delta / bit_ns + 0.5f);
            float offset = delta - bit_number * bit_ns;
            if (!bit_number || offset < -tolerance || offset > tolerance) {
                aligned = false;
                break;
            }
            float width_error = (edge->time_ns - last) - bit_ns;
            if (width_error >= -tolerance && width_error <= tolerance) ++bit_intervals;
            last = edge->time_ns;
        }
        if (!valid_start || !valid_stop || !aligned) {
            ++result.errors;
            continue;
        }
        if (result.frames < CAPTURE_MAX_UART_BYTES) {
            result.bytes[result.frames] = byte;
        }
        ++result.frames;
        result.bit_intervals += bit_intervals;
    }
    return result;
}

static void report_uart(capture_channel_t *channel, uint32_t baud, const uart_decode_t *decoded)
{
    channel->baud = baud;
    channel->uart_truncated = decoded->frames > CAPTURE_MAX_UART_BYTES;
    channel->uart_count = channel->uart_truncated ? CAPTURE_MAX_UART_BYTES : decoded->frames;
    channel->framing_errors = decoded->errors > UINT8_MAX ? UINT8_MAX : decoded->errors;
    memcpy(channel->uart_bytes, decoded->bytes, channel->uart_count);
}

void capture_decode_uart(capture_channel_t *channel, uint32_t baud_hint)
{
    if (baud_hint) {
        uart_decode_t decoded = decode_uart(channel, baud_hint);
        report_uart(channel, baud_hint, &decoded);
        return;
    }

    static const uint32_t baud_rates[] = {1200, 2400, 4800, 9600, 19200, 38400, 57600, 74880, 115200,
                                           230400, 460800, 921600, 1000000, 1500000, 2000000, 3000000, 4000000};
    uint32_t matched_baud = 0;
    uart_decode_t match;
    for (size_t i = 0; i < sizeof(baud_rates) / sizeof(baud_rates[0]); ++i) {
        if (channel->resolution_ns > 250 && baud_rates[i] > 115200) break;
        uart_decode_t decoded = decode_uart(channel, baud_rates[i]);
        if (decoded.frames < 3 || decoded.bit_intervals < 3 || decoded.errors > 0) continue;
        if (matched_baud) return;
        matched_baud = baud_rates[i];
        match = decoded;
    }
    if (matched_baud) report_uart(channel, matched_baud, &match);
}

esp_err_t capture_run_window(const gpio_num_t *pins, size_t pin_count,
                             const capture_options_t *options, capture_channel_t *channels)
{
    if (!pins || !channels || !options || pin_count == 0 || pin_count > CAPTURE_MAX_CHANNELS ||
        options->duration_ms < CAPTURE_MIN_DURATION_MS || options->duration_ms > CAPTURE_MAX_DURATION_MS ||
        (options->baud_hint != 0 && (options->baud_hint < CAPTURE_MIN_UART_BAUD ||
                                     options->baud_hint > CAPTURE_MAX_UART_BAUD)) ||
        (options->trigger != CAPTURE_MANUAL && options->trigger != CAPTURE_RISING &&
         options->trigger != CAPTURE_FALLING) ||
        (options->trigger != CAPTURE_MANUAL && (options->trigger_index >= pin_count ||
         options->wait_ms < CAPTURE_MIN_WAIT_MS || options->wait_ms > CAPTURE_MAX_WAIT_MS))) {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < pin_count; ++i) {
        if (!GPIO_IS_VALID_GPIO(pins[i])) return ESP_ERR_INVALID_ARG;
        for (size_t previous = 0; previous < i; ++previous) {
            if (pins[previous] == pins[i]) return ESP_ERR_INVALID_ARG;
        }
    }

    esp_err_t result = gpio_install_isr_service(0);
    if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) return result;

    memset(channels, 0, pin_count * sizeof(*channels));
    capture_session_t session = {.count = pin_count, .duration_us = options->duration_ms * 1000};
    if (options->trigger != CAPTURE_MANUAL) {
        session.trigger_input = &session.inputs[options->trigger_index];
        session.trigger_level = options->trigger == CAPTURE_RISING;
        session.fired = xSemaphoreCreateBinary();
        if (!session.fired) return ESP_ERR_NO_MEM;
    }

    size_t registered = 0;
    result = ESP_OK;
    for (size_t i = 0; i < pin_count; ++i) {
        gpio_num_t pin = pins[i];
        capture_input_t *input = &session.inputs[i];
        *input = (capture_input_t){.pin = pin, .channel = &channels[i], .session = &session};
        result = gpio_set_intr_type(pin, GPIO_INTR_DISABLE);
        if (result == ESP_OK) result = gpio_isr_handler_add(pin, capture_edge, input);
        if (result != ESP_OK) break;
        ++registered;
        result = gpio_intr_disable(pin);
        if (result != ESP_OK) break;
        gpio_int_type_t type = GPIO_INTR_ANYEDGE;
        if (input == session.trigger_input) {
            type = session.trigger_level ? GPIO_INTR_POSEDGE : GPIO_INTR_NEGEDGE;
        }
        result = gpio_set_intr_type(pin, type);
        if (result != ESP_OK) break;
    }
    if (result == ESP_OK) {
        if (session.trigger_input) {
            session.trigger_deadline_us = esp_timer_get_time() + options->wait_ms * 1000;
            result = gpio_intr_enable(session.trigger_input->pin);
            if (result == ESP_OK && xSemaphoreTake(session.fired, pdMS_TO_TICKS(options->wait_ms) + 1) != pdTRUE) {
                result = ESP_ERR_TIMEOUT;
            }
        } else {
            start_capture(&session, esp_timer_get_time());
            for (size_t i = 0; i < pin_count && result == ESP_OK; ++i) result = gpio_intr_enable(pins[i]);
        }
        if (result == ESP_OK) {
            int64_t remaining = session.start_us + session.duration_us - esp_timer_get_time();
            if (remaining > 0) vTaskDelay(pdMS_TO_TICKS((remaining + 999) / 1000) + 1);
        }
    }
    for (size_t i = 0; i < registered; ++i) {
        gpio_isr_handler_remove(pins[i]);
        gpio_set_intr_type(pins[i], GPIO_INTR_DISABLE);
    }
    if (session.fired) vSemaphoreDelete(session.fired);
    if (result != ESP_OK) return result;

    for (size_t i = 0; i < pin_count; ++i) {
        capture_channel_t *channel = &channels[i];
        channel->window_ns = session.duration_us * 1000;
        channel->resolution_ns = 1000;
        if (channel->truncated) {
            channel->window_ns = channel->edge_count ? channel->edges[channel->edge_count - 1].time_ns : 0;
        }
        capture_decode_uart(channel, options->baud_hint);
    }
    return ESP_OK;
}

esp_err_t capture_run(const gpio_num_t *pins, size_t pin_count, uint32_t duration_ms,
                      uint32_t baud_hint, capture_channel_t *channels)
{
    const capture_options_t options = {.duration_ms = duration_ms, .baud_hint = baud_hint};
    return capture_run_window(pins, pin_count, &options, channels);
}

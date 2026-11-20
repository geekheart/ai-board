#include <ctype.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "board.h"
#include "board_events.h"
#include "capture.h"
#include "capture_dma.h"
#include "i2c_device_capture.h"
#include "matrix.h"
#include "protocol.h"
#include "protocol_receive_fixture.h"
#include "spi_benchmark.h"
#include "spi_device_capture.h"
#include "spi_i2s_host.h"
#include "spi_reply_probe.h"
#include "spi_stimulus.h"
#include "uart_batch_capture.h"
#include "workbench.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

typedef struct {
    char *data;
    size_t available;
    bool overflow;
} response_buffer_t;

// Only the HTTP server task reads or publishes these snapshots.
enum { MAX_WORKBENCH_SNAPSHOT_BYTES = 24 * 1024 };
static char *latest_result;
static char latest_op[32];
static uint32_t latest_revision;

uint32_t workbench_result_revision(void)
{
    return latest_revision;
}

esp_err_t workbench_result(httpd_req_t *request)
{
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    if (!latest_revision) return httpd_resp_sendstr(request, "{\"revision\":0,\"op\":null,\"result\":null}");

    char prefix[96];
    snprintf(prefix, sizeof(prefix), "{\"revision\":%" PRIu32 ",\"op\":\"%s\",\"result\":",
             latest_revision, latest_op);
    esp_err_t result = httpd_resp_sendstr_chunk(request, prefix);
    if (result == ESP_OK) result = httpd_resp_sendstr_chunk(request, latest_result ? latest_result : "null");
    if (result == ESP_OK) result = httpd_resp_sendstr_chunk(request, "}");
    if (result == ESP_OK) result = httpd_resp_sendstr_chunk(request, NULL);
    return result;
}

static void append(response_buffer_t *buffer, const char *format, ...)
{
    if (buffer->overflow) return;
    va_list args;
    va_start(args, format);
    int written = vsnprintf(buffer->data, buffer->available, format, args);
    va_end(args);
    if (written < 0 || (size_t)written >= buffer->available) {
        buffer->overflow = true;
        return;
    }
    buffer->data += written;
    buffer->available -= written;
}

typedef struct {
    httpd_req_t *http;
    const char *publish_operation;
    char **output;
    const char **diagnostic;
} operation_reply_t;

void workbench_publish_result(const char *operation, const char *response)
{
    free(latest_result);
    latest_result = strlen(response) <= MAX_WORKBENCH_SNAPSHOT_BYTES ? strdup(response) : NULL;
    snprintf(latest_op, sizeof(latest_op), "%s", operation);
    if (++latest_revision == 0) ++latest_revision;
}

static esp_err_t json(operation_reply_t *request, const char *response)
{
    if (!request->http) {
        *request->output = strdup(response);
        return *request->output ? ESP_OK : ESP_ERR_NO_MEM;
    }
    if (request->publish_operation) workbench_publish_result(request->publish_operation, response);
    httpd_resp_set_type(request->http, "application/json");
    return httpd_resp_sendstr(request->http, response);
}

static esp_err_t error_response(operation_reply_t *request, const char *status,
                                const char *message, esp_err_t code)
{
    if (!request->http) {
        *request->diagnostic = message;
        return code;
    }
    httpd_resp_set_status(request->http, status);
    httpd_resp_set_type(request->http, "text/plain; charset=utf-8");
    return httpd_resp_sendstr(request->http, message);
}

static esp_err_t invalid(operation_reply_t *request, const char *message)
{
    return error_response(request, "400 Bad Request", message, ESP_ERR_INVALID_ARG);
}

static esp_err_t conflict(operation_reply_t *request, const char *message)
{
    return error_response(request, "409 Conflict", message, ESP_ERR_INVALID_STATE);
}

static esp_err_t protocol_error(operation_reply_t *request, esp_err_t error)
{
    return error_response(request, "422 Unprocessable Content", esp_err_to_name(error), error);
}

static bool field(const char *form, const char *key, char *value, size_t size)
{
    return httpd_query_key_value(form, key, value, size) == ESP_OK;
}

static bool number(const char *form, const char *key, int min, int max, int *value)
{
    char digits[16];
    if (!field(form, key, digits, sizeof(digits)) ||
        !(isdigit((unsigned char)digits[0]) || (digits[0] == '-' && isdigit((unsigned char)digits[1])))) {
        return false;
    }
    char *end;
    long parsed = strtol(digits, &end, 10);
    if (*end || parsed < min || parsed > max) return false;
    *value = parsed;
    return true;
}

static int nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static bool hex_field(const char *form, const char *key,
                      uint8_t data[PROTOCOL_MAX_BYTES], size_t *length)
{
    char digits[2 * PROTOCOL_MAX_BYTES + 1] = "";
    esp_err_t result = httpd_query_key_value(form, key, digits, sizeof(digits));
    if (result != ESP_OK && result != ESP_ERR_NOT_FOUND) return false;
    size_t size = strlen(digits);
    if (size % 2) return false;
    *length = size / 2;
    for (size_t i = 0; i < *length; ++i) {
        int high = nibble(digits[2 * i]), low = nibble(digits[2 * i + 1]);
        if (high < 0 || low < 0) return false;
        data[i] = (high << 4) | low;
    }
    return true;
}

static bool hex_data(const char *form, uint8_t data[PROTOCOL_MAX_BYTES], size_t *length)
{
    return hex_field(form, "hex", data, length);
}

static void to_hex(const uint8_t *data, size_t length, char *hex)
{
    const char digits[] = "0123456789ABCDEF";
    for (size_t i = 0; i < length; ++i) {
        hex[2 * i] = digits[data[i] >> 4];
        hex[2 * i + 1] = digits[data[i] & 0x0f];
    }
    hex[length * 2] = 0;
}

static esp_err_t hex_response(operation_reply_t *request, const uint8_t *bytes, size_t count)
{
    char hex[2 * PROTOCOL_MAX_BYTES + 1], response[192];
    to_hex(bytes, count, hex);
    snprintf(response, sizeof(response), "{\"hex\":\"%s\",\"received\":%u}", hex, (unsigned)count);
    return json(request, response);
}

static esp_err_t state_locked(operation_reply_t *request)
{
    char response[384];
    response_buffer_t buffer = {response, sizeof(response), false};
    append(&buffer, "{\"armed\":%s,\"routes\":[", board_matrix_is_armed() ? "true" : "false");
    for (unsigned y = 0; y < MATRIX_Y_COUNT; ++y) {
        append(&buffer, "%s%u", y ? "," : "", matrix_route_mask(y));
    }
    append(&buffer, "],\"levels\":[");
    for (unsigned y = 0; y < MATRIX_Y_COUNT; ++y) {
        append(&buffer, "%s%d", y ? "," : "", matrix_link_level(y));
    }
    append(&buffer, "],\"modes\":[");
    for (unsigned y = 0; y < MATRIX_Y_COUNT; ++y) {
        append(&buffer, "%s\"%s\"", y ? "," : "", matrix_mode_name(matrix_link_mode(y)));
    }
    append(&buffer, "],\"gpios\":[");
    for (unsigned y = 0; y < MATRIX_Y_COUNT; ++y) {
        append(&buffer, "%s%d", y ? "," : "", matrix_link_gpio(y));
    }
    append(&buffer, "]");
#ifdef CONFIG_AI_BOARD_OUT_CONTROL
    append(&buffer, ",\"out_mode\":\"%s\"", board_out_grounded() ? "gnd" : "5v");
#endif
    append(&buffer, "}");
    return json(request, response);
}

static esp_err_t read_state(operation_reply_t *reply)
{
    if (!board_matrix_acquire()) return conflict(reply, "Matrix is busy");
    const esp_err_t result = state_locked(reply);
    board_matrix_release();
    return result;
}

esp_err_t workbench_state(httpd_req_t *request)
{
    operation_reply_t reply = {.http = request};
    return read_state(&reply);
}

static esp_err_t configure_routes(operation_reply_t *request, const char *form, bool clear)
{
    uint16_t routes[MATRIX_Y_COUNT] = {0};
    if (!clear) {
        for (unsigned y = 0; y < MATRIX_Y_COUNT; ++y) {
            char key[] = {'y', '0' + y, 0};
            int mask;
            if (!number(form, key, 0, UINT16_MAX, &mask)) {
                return invalid(request, "Provide all 8 route masks y0 through y7");
            }
            routes[y] = mask;
        }
    }
    matrix_apply(routes);
    return state_locked(request);
}

static esp_err_t configure_gpio(operation_reply_t *request, const char *form)
{
    int y, hz = 0, duty = 0;
    char requested[12];
    if (!number(form, "y", 0, MATRIX_Y_COUNT - 1, &y) ||
        !field(form, "mode", requested, sizeof(requested))) {
        return invalid(request, "Specify Y and GPIO mode");
    }
    matrix_mode_t mode;
    for (mode = MATRIX_INPUT; mode <= MATRIX_PWM; ++mode) {
        if (strcmp(requested, matrix_mode_name(mode)) == 0) break;
    }
    if (mode > MATRIX_PWM ||
        (mode == MATRIX_PWM && (!number(form, "hz", 100, 20000, &hz) ||
                                !number(form, "duty", 0, 100, &duty)))) {
        return invalid(request, "Unknown mode or PWM outside 100-20000 Hz and 0-100 percent");
    }
    esp_err_t result = matrix_configure_link(y, mode, hz, duty);
    if (result == ESP_ERR_INVALID_STATE) {
        return conflict(request, "A driven GPIO requires one BBX and no other output on its net. Only one PWM channel is available.");
    }
    if (result != ESP_OK) return protocol_error(request, result);
    return state_locked(request);
}

static const char protocol_pin_error[] =
    "Protocol signals need separate nets, each with one BBX, a floating selected Y and no GPIO output";

static bool active_pins(const char *form, const char *const *fields, size_t count, gpio_num_t *pins)
{
    uint8_t used_links = 0;
    for (size_t i = 0; i < count; ++i) {
        int y;
        if (!number(form, fields[i], 0, MATRIX_Y_COUNT - 1, &y) || matrix_link_mode(y) != MATRIX_INPUT) return false;
        const matrix_net_t net = matrix_link_net(y);
        if ((used_links & net.links) || !net.ports || (net.ports & (net.ports - 1)) ||
            matrix_links_have_output(net.links)) return false;
        used_links |= net.links;
        pins[i] = matrix_link_gpio(y);
    }
    return true;
}

static esp_err_t uart_io(operation_reply_t *request, const char *form)
{
    const char *fields[] = {"tx", "rx"};
    gpio_num_t pins[2];
    int baud, timeout, read;
    uint8_t tx[PROTOCOL_MAX_BYTES], rx[PROTOCOL_MAX_BYTES];
    size_t sent, received;
    if (!active_pins(form, fields, 2, pins)) return conflict(request, protocol_pin_error);
    if (!number(form, "baud", 1200, 115200, &baud) ||
        !number(form, "read", 1, PROTOCOL_MAX_BYTES, &read) ||
        !number(form, "timeout", 20, 500, &timeout) || !hex_data(form, tx, &sent)) {
        return invalid(request, "UART requires baud, read limit, timeout, and optional even-length hex");
    }
    esp_err_t result = protocol_uart(pins[0], pins[1], baud, tx, sent, rx, read, timeout, &received);
    if (result != ESP_OK) return protocol_error(request, result);
    return hex_response(request, rx, received);
}

static esp_err_t uart_proxy(operation_reply_t *request, const char *form)
{
    // The labels describe which external device transmits on each wire.
    const char *fields[] = {"host_rx", "host_tx", "device_rx", "device_tx"};
    gpio_num_t pins[4];
    int baud, timeout;
    uint8_t query[PROTOCOL_MAX_BYTES], reply[PROTOCOL_MAX_BYTES];
    size_t query_count, reply_count;
    if (!active_pins(form, fields, 4, pins)) return conflict(request, protocol_pin_error);
    if (!number(form, "baud", 1200, 115200, &baud) || !number(form, "timeout", 20, 500, &timeout)) {
        return invalid(request, "Specify UART baud and timeout");
    }
    esp_err_t result = protocol_uart_proxy(pins[0], pins[1], pins[2], pins[3], baud, timeout,
                                           query, &query_count, reply, &reply_count);
    if (result != ESP_OK) return protocol_error(request, result);
    char tx[2 * PROTOCOL_MAX_BYTES + 1], rx[2 * PROTOCOL_MAX_BYTES + 1], response[312];
    to_hex(query, query_count, tx);
    to_hex(reply, reply_count, rx);
    snprintf(response, sizeof(response), "{\"request_hex\":\"%s\",\"response_hex\":\"%s\"}", tx, rx);
    return json(request, response);
}

static esp_err_t i2c_io(operation_reply_t *request, const char *form, bool scan, bool transfer)
{
    const char *fields[] = {"sda", "scl"};
    gpio_num_t pins[2];
    if (!active_pins(form, fields, 2, pins)) return conflict(request, protocol_pin_error);
    if (scan) {
        uint8_t addresses[PROTOCOL_I2C_MAX_ADDRESSES];
        size_t count;
        esp_err_t result = protocol_i2c_scan(pins[0], pins[1], addresses, &count);
        if (result != ESP_OK) return protocol_error(request, result);
        char response[512];
        response_buffer_t output = {response, sizeof(response), false};
        append(&output, "{\"addresses\":[");
        for (size_t i = 0; i < count; ++i) append(&output, "%s%u", i ? "," : "", addresses[i]);
        append(&output, "]}");
        return json(request, response);
    }
    if (transfer) {
        int address, hz, read;
        uint8_t tx[PROTOCOL_MAX_BYTES], rx[PROTOCOL_MAX_BYTES];
        size_t written;
        if (!number(form, "address", 8, 119, &address) || !number(form, "hz", 5000, 400000, &hz) ||
            !number(form, "read", 0, PROTOCOL_MAX_BYTES, &read) || !hex_data(form, tx, &written) ||
            (!written && !read)) {
            return invalid(request, "Specify I2C address, clock, read count and optional write hex");
        }
        const esp_err_t result = protocol_i2c_transfer(pins[0], pins[1], address, hz, tx, written, rx, read);
        return result == ESP_OK ? hex_response(request, rx, read) : protocol_error(request, result);
    }
    int address, reg, count;
    if (!number(form, "address", 8, 119, &address) || !number(form, "reg", 0, 255, &reg) ||
        !number(form, "count", 1, PROTOCOL_MAX_BYTES, &count)) {
        return invalid(request, "Specify 7-bit I2C address, 8-bit register and read count");
    }
    uint8_t prefix = reg, bytes[PROTOCOL_MAX_BYTES];
    esp_err_t result = protocol_i2c_read(pins[0], pins[1], address, &prefix, 1, bytes, count);
    if (result != ESP_OK) return protocol_error(request, result);
    return hex_response(request, bytes, count);
}

static esp_err_t spi_io(operation_reply_t *request, const char *form)
{
    const char *fields[] = {"sck", "mosi", "miso", "cs"};
    gpio_num_t pins[4];
    int hz;
    size_t sent;
    uint8_t tx[PROTOCOL_MAX_BYTES], rx[PROTOCOL_MAX_BYTES];
    if (!active_pins(form, fields, 4, pins)) return conflict(request, protocol_pin_error);
    if (!number(form, "hz", 1000, 20000, &hz) || !hex_data(form, tx, &sent) || !sent) {
        return invalid(request, "SPI requires 1000-20000 Hz and 1-64 hex bytes");
    }
    esp_err_t result = protocol_spi(pins[0], pins[1], pins[2], pins[3], hz, tx, rx, sent);
    if (result != ESP_OK) return protocol_error(request, result);
    return hex_response(request, rx, sent);
}

static bool capture_trigger(const char *form, capture_options_t *options, char name[12])
{
    esp_err_t found = httpd_query_key_value(form, "trigger", name, 12);
    if (found == ESP_ERR_NOT_FOUND) {
        strcpy(name, "manual");
    } else if (found != ESP_OK) {
        return false;
    }
    if (strcmp(name, "manual") == 0) options->trigger = CAPTURE_MANUAL;
    else if (strcmp(name, "rising") == 0) options->trigger = CAPTURE_RISING;
    else if (strcmp(name, "falling") == 0) options->trigger = CAPTURE_FALLING;
    else return false;
    return true;
}

typedef struct {
    capture_options_t options;
    char trigger_name[12];
    gpio_num_t pins[CAPTURE_MAX_CHANNELS];
    int ys[CAPTURE_MAX_CHANNELS];
    unsigned slots[CAPTURE_MAX_CHANNELS];
    size_t count;
    bool dma;
    uint32_t window_us;
    uint32_t sample_hz;
    uint32_t sample_start_ns;
    uint32_t duration_ns;
} capture_request_t;

static bool capture_engine(const char *form, capture_request_t *capture)
{
    char engine[12];
    esp_err_t found = httpd_query_key_value(form, "engine", engine, sizeof(engine));
    if (found == ESP_ERR_NOT_FOUND) return true;
    if (found != ESP_OK) return false;
    if (strcmp(engine, "gpio") == 0) return true;
    if (strcmp(engine, "dma") != 0) return false;
    int samples_per_second, window;
    if (!number(form, "sample_hz", 1000000, 20000000, &samples_per_second) ||
        !number(form, "window_us", 100, 65536, &window) ||
        (uint64_t)samples_per_second * window > 65536ULL * 1000000) return false;
    capture->dma = true;
    capture->sample_hz = samples_per_second;
    capture->window_us = window;
    capture->duration_ns = window * 1000;
    return true;
}

static esp_err_t parse_capture(const char *form, capture_request_t *capture, const char **diagnostic)
{
    int duration = 0, baud = 0;
    char explicit_baud[16];
    esp_err_t baud_present = httpd_query_key_value(form, "baud", explicit_baud, sizeof(explicit_baud));
    *diagnostic = "Select GPIO for a 10-500 ms window or DMA for up to 65536 samples";
    if (!capture_engine(form, capture) || (!capture->dma &&
        !number(form, "ms", CAPTURE_MIN_DURATION_MS, CAPTURE_MAX_DURATION_MS, &duration))) {
        return ESP_ERR_INVALID_ARG;
    }
    *diagnostic = "Invalid UART baud";
    if (baud_present != ESP_ERR_NOT_FOUND &&
        (baud_present != ESP_OK || !number(form, "baud", 0, CAPTURE_MAX_UART_BAUD, &baud) ||
            (baud && baud < CAPTURE_MIN_UART_BAUD))) return ESP_ERR_INVALID_ARG;
    capture->options.duration_ms = duration;
    capture->options.baud_hint = baud;
    *diagnostic = "Select 1-4 distinct Y channels with A required";
    unsigned used = 0;
    for (unsigned slot = 0; slot < CAPTURE_MAX_CHANNELS; ++slot) {
        char key[] = {'a' + slot, 0};
        char value[16];
        esp_err_t found = httpd_query_key_value(form, key, value, sizeof(value));
        if (slot > 0 && found == ESP_ERR_NOT_FOUND) continue;
        int y;
        if (found != ESP_OK || !number(form, key, slot == 0 ? 0 : -1, MATRIX_Y_COUNT - 1, &y)) {
            return ESP_ERR_INVALID_ARG;
        }
        if (y == -1) continue;
        if (used & (1U << y)) return ESP_ERR_INVALID_ARG;
        used |= 1U << y;
        size_t index = capture->count++;
        capture->pins[index] = matrix_link_gpio(y);
        capture->ys[index] = y;
        capture->slots[index] = slot;
    }
    *diagnostic = "Unknown trigger mode";
    if (!capture_trigger(form, &capture->options, capture->trigger_name)) return ESP_ERR_INVALID_ARG;
    if (capture->options.trigger != CAPTURE_MANUAL) {
        int slot, wait;
        *diagnostic = "Edge mode requires a selected trigger channel and 100-5000 ms timeout";
        if (!number(form, "trigger_channel", 0, CAPTURE_MAX_CHANNELS - 1, &slot) ||
            !number(form, "wait_ms", CAPTURE_MIN_WAIT_MS, CAPTURE_MAX_WAIT_MS, &wait)) {
            return ESP_ERR_INVALID_ARG;
        }
        size_t index = 0;
        while (index < capture->count && capture->slots[index] != (unsigned)slot) ++index;
        if (index == capture->count) return ESP_ERR_INVALID_ARG;
        capture->options.trigger_index = index;
        capture->options.wait_ms = wait;
    }
    *diagnostic = "Capture needs connected Ys in input mode";
    for (size_t i = 0; i < capture->count; ++i) {
        if (!matrix_route_mask(capture->ys[i]) || matrix_link_mode(capture->ys[i]) > MATRIX_PULLDOWN) {
            return ESP_ERR_INVALID_STATE;
        }
    }
    return ESP_OK;
}

static bool sample_level(const capture_dma_result_t *data, size_t sample, unsigned lane)
{
    unsigned shift = (sample & 1U) ? 0 : 4;
    return (data->packed[sample / 2] >> (shift + lane)) & 1U;
}

static void add_sample_edge(capture_channel_t *channel, uint32_t time_ns, bool level)
{
    if (channel->edge_count == CAPTURE_MAX_EDGES) {
        channel->truncated = true;
    } else {
        channel->edges[channel->edge_count++] = (capture_edge_t){.time_ns = time_ns, .level = level};
    }
}

static esp_err_t execute_capture(capture_request_t *capture, capture_channel_t *channels,
                                void (*stimulus)(void *), void *argument)
{
    if (!capture->dma) return capture_run_window(capture->pins, capture->count, &capture->options, channels);
    capture_dma_result_t data = {0};
    esp_err_t result = capture_dma_run(capture->pins, capture->count, capture->sample_hz, capture->window_us,
                                       capture->options.trigger, capture->options.trigger_index,
                                       capture->options.wait_ms, stimulus, argument, &data);
    if (result != ESP_OK) return result;
    capture->sample_hz = data.sample_hz;
    capture->sample_start_ns = data.trigger_delay_ns;
    capture->duration_ns = data.trigger_delay_ns + (uint64_t)data.sample_count * 1000000000 / data.sample_hz;
    for (unsigned i = 0; i < capture->count; ++i) {
        capture_channel_t *channel = &channels[i];
        bool previous = sample_level(&data, 0, i);
        channel->initial_level = previous;
        channel->window_ns = capture->duration_ns;
        channel->resolution_ns = (1000000000U + data.sample_hz - 1) / data.sample_hz;
        if (capture->options.trigger != CAPTURE_MANUAL && i == capture->options.trigger_index) {
            bool triggered_level = capture->options.trigger == CAPTURE_RISING;
            channel->initial_level = !triggered_level;
            add_sample_edge(channel, 0, triggered_level);
            if (previous != triggered_level) add_sample_edge(channel, capture->sample_start_ns, previous);
        }
        for (size_t j = 1; j < data.sample_count && !channel->truncated; ++j) {
            bool level = sample_level(&data, j, i);
            if (level == previous) continue;
            uint32_t time_ns = capture->sample_start_ns + (uint64_t)j * 1000000000 / data.sample_hz;
            add_sample_edge(channel, time_ns, level);
            previous = level;
        }
        if (channel->truncated) channel->window_ns = channel->edges[channel->edge_count - 1].time_ns;
        capture_decode_uart(channel, capture->options.baud_hint);
    }
    free(data.packed);
    return ESP_OK;
}

static esp_err_t serialize_capture(const capture_request_t *capture, const capture_channel_t *channels,
                                   const char *fixture, char **result)
{
    size_t count = channels ? capture->count : 0;
    // 16 characters cover a timestamp/level pair plus punctuation.
    const size_t capacity = 256 + count * (320 + CAPTURE_MAX_EDGES * 16);
    char *response = malloc(capacity);
    if (!response) return ESP_ERR_NO_MEM;

    response_buffer_t output = {response, capacity, false};
    append(&output, "{\"duration_ms\":");
    if (capture->dma) {
        append(&output, "%" PRIu32 ".%06" PRIu32 ",\"engine\":\"dma\",\"sample_hz\":%" PRIu32
               ",\"sample_start_us\":%" PRIu32 ".%03" PRIu32,
               capture->duration_ns / 1000000, capture->duration_ns % 1000000,
               capture->sample_hz, capture->sample_start_ns / 1000, capture->sample_start_ns % 1000);
    } else {
        append(&output, "%" PRIu32 ",\"engine\":\"gpio\"", capture->options.duration_ms);
    }
    append(&output, ",\"trigger\":\"%s\",\"trigger_channel\":%u,\"triggered\":%s",
           capture->trigger_name, capture->slots[capture->options.trigger_index], channels ? "true" : "false");
    if (fixture) append(&output, ",\"fixture\":\"%s\"", fixture);
    append(&output, ",\"channels\":[");
    for (size_t i = 0; i < count; ++i) {
        const capture_channel_t *channel = &channels[i];
        char decoded[2 * CAPTURE_MAX_UART_BYTES + 1];
        to_hex(channel->uart_bytes, channel->uart_count, decoded);
        append(&output, "%s{\"slot\":%u,\"y\":%d,\"initial\":%u,\"edge_count\":%u,\"baud\":",
               i ? "," : "", capture->slots[i], capture->ys[i], channel->initial_level, channel->edge_count);
        if (channel->baud) append(&output, "%" PRIu32, channel->baud);
        else append(&output, "null");
        append(&output, ",\"window_us\":%" PRIu32 ".%03" PRIu32
               ",\"hex\":\"%s\",\"framing_errors\":%u,\"overflow\":%s,\"edges\":[",
               channel->window_ns / 1000, channel->window_ns % 1000, decoded,
               channel->framing_errors, channel->truncated ? "true" : "false");
        for (size_t j = 0; j < channel->edge_count; ++j) {
            uint32_t ns = channel->edges[j].time_ns;
            append(&output, "%s[%" PRIu32 ".%03" PRIu32 ",%u]", j ? "," : "",
                   ns / 1000, ns % 1000, channel->edges[j].level);
        }
        append(&output, "],\"uart_truncated\":%s}", channel->uart_truncated ? "true" : "false");
    }
    append(&output, "]}");
    if (output.overflow) {
        free(response);
        return ESP_ERR_INVALID_SIZE;
    }
    *result = response;
    return ESP_OK;
}

static esp_err_t collect_capture(const char *form, char **output, const char **diagnostic)
{
    capture_request_t capture = {0};
    esp_err_t result = parse_capture(form, &capture, diagnostic);
    if (result != ESP_OK) return result;

    *diagnostic = NULL;
    capture_channel_t *channels = calloc(capture.count, sizeof(*channels));
    if (!channels) return ESP_ERR_NO_MEM;
    const esp_err_t captured = execute_capture(&capture, channels, NULL, NULL);
    result = captured;
    if (result == ESP_OK || result == ESP_ERR_TIMEOUT) {
        result = serialize_capture(&capture, result == ESP_OK ? channels : NULL, NULL, output);
    }
    free(channels);
    board_event_record(BOARD_EVENT_CAPTURE, result == ESP_OK ? captured : result, capture.count);
    return result;
}

esp_err_t workbench_serial_capture(const char *form, char **output, const char **diagnostic)
{
    if (!form || !output || !diagnostic) return ESP_ERR_INVALID_ARG;
    *output = NULL;
    if (!board_matrix_is_armed()) {
        *diagnostic = "Arm the matrix first";
        return ESP_ERR_INVALID_STATE;
    }
    if (!board_matrix_acquire()) {
        *diagnostic = "Matrix is busy";
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t result = collect_capture(form, output, diagnostic);
    board_matrix_release();
    return result;
}

static esp_err_t capture_io(operation_reply_t *request, const char *form)
{
    const char *diagnostic;
    char *response = NULL;
    esp_err_t result = collect_capture(form, &response, &diagnostic);
    if (result == ESP_ERR_INVALID_ARG && diagnostic) return invalid(request, diagnostic);
    if (result == ESP_ERR_INVALID_STATE && diagnostic) return conflict(request, diagnostic);
    if (result != ESP_OK) return protocol_error(request, result);
    result = json(request, response);
    free(response);
    return result;
}

typedef struct {
    SemaphoreHandle_t armed;
    bool manual;
    volatile bool cancelled;
} fixture_gate_t;

static void fixture_ready(void *context)
{
    fixture_gate_t *gate = context;
    xSemaphoreGive(gate->armed);
    // A lower-priority fixture task can start while SPI2 samples in hardware.
    if (gate->manual) vTaskDelay(1);
}

static bool fixture_wait(fixture_gate_t *gate)
{
    if (!gate->armed) {
        vTaskDelay(pdMS_TO_TICKS(30));
        return true;
    }
    xSemaphoreTake(gate->armed, portMAX_DELAY);
    return !gate->cancelled;
}

static void fixture_cancel(fixture_gate_t *gate)
{
    if (!gate->armed) return;
    gate->cancelled = true;
    xSemaphoreGive(gate->armed);
}

static const uint8_t loopback_pattern[] = {0x55, 0xa6, 0x00, 0xff, 0xc3, 0x46};

typedef struct {
    SemaphoreHandle_t done;
    esp_err_t result;
    uint8_t rx[sizeof(loopback_pattern)];
    size_t count;
    uint32_t baud;
    fixture_gate_t gate;
} loopback_transaction_t;

static void run_loopback_uart(void *context)
{
    loopback_transaction_t *test = context;
    test->result = ESP_ERR_INVALID_STATE;
    if (fixture_wait(&test->gate)) {
        test->result = protocol_uart(matrix_link_gpio(7), matrix_link_gpio(2), test->baud,
                                     loopback_pattern, sizeof(loopback_pattern),
                                     test->rx, sizeof(test->rx), 100, &test->count);
    }
    xSemaphoreGive(test->done);
    vTaskDelete(NULL);
}

static esp_err_t uart_loopback(operation_reply_t *request, const char *form)
{
    char confirmed[8];
    if (matrix_has_routes() || !field(form, "confirm_empty", confirmed, sizeof(confirmed)) ||
        strcmp(confirmed, "yes") != 0) {
        return conflict(request, "Clear routes and unload all BBX pins before internal loopback");
    }
    int baud = 9600;
    char baud_field[16];
    esp_err_t baud_present = httpd_query_key_value(form, "baud", baud_field, sizeof(baud_field));
    if (baud_present != ESP_ERR_NOT_FOUND &&
        (baud_present != ESP_OK || !number(form, "baud", CAPTURE_MIN_UART_BAUD, CAPTURE_MAX_UART_BAUD, &baud))) {
        return invalid(request, "Test UART baud must be 1200-4000000");
    }
    capture_request_t capture = {
        .options = {.duration_ms = 100, .wait_ms = 250, .baud_hint = baud_present == ESP_OK ? (uint32_t)baud : 0},
        .count = 1, .ys = {4},
    };
    if (!capture_engine(form, &capture)) return invalid(request, "Invalid capture engine or DMA window");
    if (!capture_trigger(form, &capture.options, capture.trigger_name)) return invalid(request, "Unknown trigger mode");
    capture_channel_t *trace = calloc(1, sizeof(*trace));
    loopback_transaction_t test = {
        .done = xSemaphoreCreateBinary(), .baud = baud,
        .gate = {.armed = capture.dma ? xSemaphoreCreateBinary() : NULL,
                 .manual = capture.options.trigger == CAPTURE_MANUAL},
    };
    if (!trace || !test.done || (capture.dma && !test.gate.armed)) {
        free(trace);
        if (test.done) vSemaphoreDelete(test.done);
        if (test.gate.armed) vSemaphoreDelete(test.gate.armed);
        return protocol_error(request, ESP_ERR_NO_MEM);
    }
    gpio_num_t observer = matrix_link_gpio(4);
    capture.pins[0] = observer;
    matrix_test_links(0, (1U << 7) | (1U << 2) | (1U << 4));
    gpio_set_pull_mode(observer, GPIO_PULLUP_ONLY);
    bool started = xTaskCreate(run_loopback_uart, "uart_loopback", 4096, &test, 4, NULL) == pdPASS;
    esp_err_t capture_result = ESP_ERR_NO_MEM;
    if (started) {
        capture_result = execute_capture(&capture, trace, fixture_ready, &test.gate);
        if (capture_result != ESP_OK) fixture_cancel(&test.gate);
        xSemaphoreTake(test.done, portMAX_DELAY);
    }
    const uint16_t cleared[MATRIX_Y_COUNT] = {0};
    matrix_apply(cleared);
    vSemaphoreDelete(test.done);
    if (test.gate.armed) vSemaphoreDelete(test.gate.armed);
    esp_err_t result = !started ? ESP_ERR_NO_MEM : (capture_result != ESP_OK ? capture_result : test.result);
    if (result != ESP_OK) {
        free(trace);
        return protocol_error(request, result);
    }
    bool passed = test.count == sizeof(loopback_pattern) &&
                  memcmp(test.rx, loopback_pattern, sizeof(loopback_pattern)) == 0;
    bool captured = trace->baud == (uint32_t)baud && !trace->framing_errors && !trace->truncated &&
                    trace->uart_count == sizeof(loopback_pattern) &&
                    memcmp(trace->uart_bytes, loopback_pattern, sizeof(loopback_pattern)) == 0;
    char hex[2 * sizeof(test.rx) + 1], captured_hex[2 * CAPTURE_MAX_UART_BYTES + 1], response[512];
    to_hex(test.rx, test.count, hex);
    to_hex(trace->uart_bytes, trace->uart_count, captured_hex);
    snprintf(response, sizeof(response),
             "{\"hex\":\"%s\",\"received\":%u,\"passed\":%s,\"capture_passed\":%s,"
             "\"capture_baud\":%" PRIu32 ",\"capture_hex\":\"%s\",\"capture_edges\":%u,"
             "\"trigger\":\"%s\",\"first_edge_us\":%" PRIu32 ",\"first_edge_level\":%u,"
             "\"requested_baud\":%d,\"framing_errors\":%u,\"engine\":\"%s\",\"sample_hz\":%u}",
             hex, (unsigned)test.count, passed ? "true" : "false", captured ? "true" : "false",
             trace->baud, captured_hex, trace->edge_count, capture.trigger_name,
             trace->edges[0].time_ns / 1000, trace->edges[0].level, baud, trace->framing_errors,
             capture.dma ? "dma" : "gpio", (unsigned)capture.sample_hz);
    free(trace);
    return json(request, response);
}

static void emit_spi_fixture(void *context)
{
    SemaphoreHandle_t done = context;
    const uint8_t mosi[] = {0x9F, 0x00, 0x00, 0xA5};
    const uint8_t miso[] = {0xFF, 0xEF, 0x40, 0x16};
    gpio_num_t cs = matrix_link_gpio(0);
    gpio_num_t sck = matrix_link_gpio(1);
    gpio_num_t tx = matrix_link_gpio(2);
    gpio_num_t rx = matrix_link_gpio(3);
    vTaskDelay(pdMS_TO_TICKS(30));
    gpio_set_level(cs, 0);
    esp_rom_delay_us(100);
    for (size_t i = 0; i < sizeof(mosi); ++i) {
        for (int bit = 7; bit >= 0; --bit) {
            gpio_set_level(tx, (mosi[i] >> bit) & 1);
            gpio_set_level(rx, (miso[i] >> bit) & 1);
            esp_rom_delay_us(50);
            gpio_set_level(sck, 1);
            esp_rom_delay_us(50);
            gpio_set_level(sck, 0);
        }
    }
    esp_rom_delay_us(100);
    gpio_set_level(cs, 1);
    xSemaphoreGive(done);
    vTaskDelete(NULL);
}

static void i2c_fixture_byte(gpio_num_t scl, gpio_num_t sda, uint8_t byte, bool ack)
{
    for (int bit = 8; bit >= 0; --bit) {
        gpio_set_level(sda, bit ? ((byte >> (bit - 1)) & 1) : !ack);
        esp_rom_delay_us(50);
        gpio_set_level(scl, 1);
        esp_rom_delay_us(50);
        gpio_set_level(scl, 0);
    }
}

static void emit_i2c_fixture(void *context)
{
    SemaphoreHandle_t done = context;
    gpio_num_t scl = matrix_link_gpio(0);
    gpio_num_t sda = matrix_link_gpio(1);
    vTaskDelay(pdMS_TO_TICKS(30));

    gpio_set_level(sda, 0);
    esp_rom_delay_us(50);
    gpio_set_level(scl, 0);
    i2c_fixture_byte(scl, sda, 0x90, true);
    i2c_fixture_byte(scl, sda, 0x01, true);

    gpio_set_level(sda, 1);
    esp_rom_delay_us(50);
    gpio_set_level(scl, 1);
    esp_rom_delay_us(50);
    gpio_set_level(sda, 0);
    esp_rom_delay_us(50);
    gpio_set_level(scl, 0);
    i2c_fixture_byte(scl, sda, 0x91, true);
    i2c_fixture_byte(scl, sda, 0xA5, false);

    gpio_set_level(sda, 0);
    esp_rom_delay_us(50);
    gpio_set_level(scl, 1);
    esp_rom_delay_us(50);
    gpio_set_level(sda, 1);
    xSemaphoreGive(done);
    vTaskDelete(NULL);
}

typedef struct {
    SemaphoreHandle_t done;
    esp_err_t result;
    uint32_t hz;
    bool dma;
    fixture_gate_t gate;
} protocol_host_test_t;

static void run_i2c_host(void *context)
{
    protocol_host_test_t *test = context;
    const uint8_t reg = 0x01;
    uint8_t data = 0;
    esp_err_t result = ESP_ERR_INVALID_STATE;
    if (fixture_wait(&test->gate)) {
        result = protocol_i2c_read_at_rate(matrix_link_gpio(1), matrix_link_gpio(0),
                                           0x48, test->hz, &reg, 1, &data, 1);
    }
    // Both matrix branches terminate at the analyzer. There is no I²C responder.
    test->result = result == ESP_ERR_INVALID_RESPONSE ? ESP_OK
                   : result == ESP_OK ? ESP_ERR_INVALID_STATE : result;
    xSemaphoreGive(test->done);
    vTaskDelete(NULL);
}

static void run_spi_host(void *context)
{
    protocol_host_test_t *test = context;
    const uint8_t pattern[] = {0x9F, 0x00, 0x00, 0xA5};
    uint8_t received[sizeof(pattern)] = {0};
    if (!fixture_wait(&test->gate)) {
        test->result = ESP_ERR_INVALID_STATE;
    } else if (test->dma) {
        test->result = spi_stimulus_transfer(matrix_link_gpio(1), matrix_link_gpio(2), matrix_link_gpio(0),
                                             test->hz, pattern, sizeof(pattern));
    } else if (test->hz >= 40000) {
        test->result = spi_benchmark_transfer(matrix_link_gpio(1), matrix_link_gpio(2), matrix_link_gpio(3),
                                              matrix_link_gpio(0), test->hz, pattern, received, sizeof(pattern));
    } else {
        test->result = protocol_spi(matrix_link_gpio(1), matrix_link_gpio(2), matrix_link_gpio(3),
                                    matrix_link_gpio(0), test->hz, pattern, received, sizeof(pattern));
    }
    if (!test->dma && test->result == ESP_OK && memcmp(pattern, received, sizeof(pattern)) != 0) {
        test->result = ESP_ERR_INVALID_RESPONSE;
    }
    xSemaphoreGive(test->done);
    vTaskDelete(NULL);
}

static esp_err_t protocol_capture_loopback(operation_reply_t *request, const char *form, bool i2c, bool host)
{
    char confirmed[8];
    if (matrix_has_routes() || !field(form, "confirm_empty", confirmed, sizeof(confirmed)) ||
        strcmp(confirmed, "yes") != 0) {
        return conflict(request, "Clear routes and unload all BBX pins before protocol capture loopback");
    }
    capture_request_t capture = {
        .options = {.duration_ms = 80, .wait_ms = 250, .trigger_index = i2c ? 1 : 0},
        .count = i2c ? 2 : CAPTURE_MAX_CHANNELS,
    };
    if (!capture_engine(form, &capture) || (capture.dma && !host)) {
        return invalid(request, "Invalid fixture capture engine or window");
    }
    int hz = i2c ? 5000 : 10000;
    if (host) {
        char hz_field[16];
        esp_err_t found = httpd_query_key_value(form, "hz", hz_field, sizeof(hz_field));
        int min_hz = i2c ? 5000 : capture.dma ? SPI_STIMULUS_MIN_HZ : 1000;
        int max_hz = i2c ? 400000 : capture.dma ? SPI_STIMULUS_MAX_HZ : 1000000;
        if (found != ESP_ERR_NOT_FOUND &&
            (found != ESP_OK || !number(form, "hz", min_hz, max_hz, &hz) ||
             (!i2c && !capture.dma && hz > 39000 && hz < 40000))) {
            return invalid(request, "Test clock outside fixture range");
        }
    }
    char mode[12];
    if (!field(form, "trigger", mode, sizeof(mode))) {
        capture.options.trigger = CAPTURE_FALLING;
        strcpy(capture.trigger_name, "falling");
    } else if (!capture_trigger(form, &capture.options, capture.trigger_name)) {
        return invalid(request, "Unknown trigger mode");
    }
    int slot;
    char requested_slot[16];
    if (capture.options.trigger != CAPTURE_MANUAL && field(form, "trigger_channel", requested_slot, sizeof(requested_slot))) {
        if (!number(form, "trigger_channel", 0, capture.count - 1, &slot)) {
            return invalid(request, "Trigger channel is not in the fixture");
        }
        capture.options.trigger_index = slot;
    }
    capture_channel_t *channels = calloc(capture.count, sizeof(*channels));
    SemaphoreHandle_t done = xSemaphoreCreateBinary();
    protocol_host_test_t host_test = {
        .done = done, .hz = hz, .dma = capture.dma,
        .gate = {.armed = capture.dma ? xSemaphoreCreateBinary() : NULL,
                 .manual = capture.options.trigger == CAPTURE_MANUAL},
    };
    if (!channels || !done || (capture.dma && !host_test.gate.armed)) {
        free(channels);
        if (done) vSemaphoreDelete(done);
        if (host_test.gate.armed) vSemaphoreDelete(host_test.gate.armed);
        return protocol_error(request, ESP_ERR_NO_MEM);
    }

    uint8_t links[MATRIX_X_COUNT] = {0};
    for (unsigned i = 0; i < capture.count; ++i) {
        links[i] = (1U << i) | (1U << (i + 4));
        capture.ys[i] = i + 4;
        capture.pins[i] = matrix_link_gpio(capture.ys[i]);
        capture.slots[i] = i;
    }
    if (host && !i2c) {
        // Y2 (MOSI) drives the separate MISO input Y3 and observer Y7 through X3.
        links[3] |= 1U << 2;
    }
    matrix_test_routes(links);
    for (unsigned i = 0; i < capture.count; ++i) {
        if (host && !i2c && i == 3) continue;
        gpio_num_t driver = matrix_link_gpio(i);
        gpio_set_level(driver, i2c || i == 0);
        gpio_set_direction(driver, i2c ? GPIO_MODE_OUTPUT_OD : GPIO_MODE_OUTPUT);
        if (i2c) gpio_set_pull_mode(capture.pins[i], GPIO_PULLUP_ONLY);
    }
    if (host && !i2c) gpio_set_pull_mode(capture.pins[0], GPIO_PULLUP_ONLY);
    TaskFunction_t stimulus = i2c ? emit_i2c_fixture : emit_spi_fixture;
    if (host) stimulus = i2c ? run_i2c_host : run_spi_host;
    esp_err_t result = ESP_ERR_NO_MEM;
    if (xTaskCreate(stimulus, "protocol_fixture", host ? 4096 : 3072,
                    host ? (void *)&host_test : (void *)done, 4, NULL) == pdPASS) {
        result = execute_capture(&capture, channels, fixture_ready, &host_test.gate);
        if (result != ESP_OK) fixture_cancel(&host_test.gate);
        xSemaphoreTake(done, portMAX_DELAY);
        if (host && result == ESP_OK) result = host_test.result;
    }
    const uint16_t cleared[MATRIX_Y_COUNT] = {0};
    matrix_apply(cleared);
    vSemaphoreDelete(done);
    if (host_test.gate.armed) vSemaphoreDelete(host_test.gate.armed);
    char *response = NULL;
    const char *fixture = i2c ? (host ? "i2c_host_nack" : "i2c_7bit")
                              : (host ? "spi_host_loopback" : "spi_mode0");
    if (result == ESP_OK) result = serialize_capture(&capture, channels, fixture, &response);
    free(channels);
    if (result != ESP_OK) return protocol_error(request, result);
    result = json(request, response);
    free(response);
    return result;
}

static const char *json_bool(bool value)
{
    return value ? "true" : "false";
}

static void append_hex(response_buffer_t *buffer, const uint8_t *data, size_t size)
{
    if (buffer->overflow) return;
    if (size * 2 >= buffer->available) {
        buffer->overflow = true;
        return;
    }
    to_hex(data, size, buffer->data);
    buffer->available -= size * 2;
    buffer->data += size * 2;
}

static const uint8_t receive_pattern[RECEIVE_FIXTURE_MAX_BYTES] = {
    0x55, 0xA6, 0x00, 0xFF, 0xC3, 0x46, 0x3C, 0x81,
    0xAA, 0x3B, 0x7F, 0x80, 0x15, 0xEA, 0x0F, 0xF0,
    0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80,
    0xDF, 0x9E, 0xCE, 0x74, 0x5A, 0xA5, 0xFF, 0x00,
};

static const uint8_t i2c_receive_pattern[I2C_DEVICE_CAPTURE_MAX_BYTES] = {
    0x55, 0xa6, 0x00, 0xff, 0xc3, 0x46, 0x3c, 0x81,
    0xaa, 0x3b, 0x7f, 0x80, 0x15, 0xea, 0x0f, 0xf0,
    0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80,
    0xdf, 0x9e, 0xce, 0x74, 0x5a, 0xa5, 0xff, 0x00,
    0x7b, 0x00, 0xff, 0xa5, 0x5a, 0x5d, 0x16, 0x8e,
    0x12, 0x28, 0x44, 0x03, 0x81, 0xfe, 0xc1, 0xe8,
    0xf0, 0xe1, 0xd2, 0xc3, 0xb4, 0xa5, 0x96, 0x87,
    0x78, 0x69, 0x5a, 0x4b, 0x3c, 0x2d, 0x1e, 0x0f,
};

typedef struct {
    unsigned requested_rate;
    unsigned actual_rate;
    esp_err_t status;
    bool passed;
} receive_bench_t;

static void append_receive_bench(response_buffer_t *buffer, const receive_bench_t *bench,
                                 const uint8_t *expected, size_t expected_size)
{
    if (!bench) return;
    append(buffer, ",\"bench\":true,\"passed\":%s,\"requested_rate\":%u,\"fixture_rate\":%u,"
                   "\"fixture_status\":\"%s\",\"expected_hex\":\"",
           json_bool(bench->passed), bench->requested_rate, bench->actual_rate, esp_err_to_name(bench->status));
    append_hex(buffer, expected, expected_size);
    append(buffer, "\"");
}

static esp_err_t send_uart_receive(operation_reply_t *request, unsigned requested_baud, const uint8_t *data,
                                   const uart_batch_capture_result_t *result, const receive_bench_t *bench)
{
    const size_t capacity = 512 + result->bytes * 2;
    char *response = malloc(capacity);
    if (!response) return protocol_error(request, ESP_ERR_NO_MEM);
    response_buffer_t buffer = {response, capacity, false};
    append(&buffer, "{\"operation\":\"uart_receive\",\"requested_baud\":%u,\"actual_baud\":%u,"
                    "\"received\":%zu,\"timed_out\":%s,\"idle\":%s,\"limit_reached\":%s,"
                    "\"overflow\":%s,\"events_lost\":%s,\"frame_errors\":%u,"
                    "\"parity_errors\":%u,\"break_events\":%u,\"rx_hex\":\"",
           requested_baud, result->actual_baud, result->bytes,
           json_bool(result->timed_out), json_bool(result->idle), json_bool(result->limit_reached),
           json_bool(result->overflow), json_bool(result->events_lost), result->frame_errors,
           result->parity_errors, result->break_events);
    append_hex(&buffer, data, result->bytes);
    append(&buffer, "\"");
    append_receive_bench(&buffer, bench, receive_pattern, sizeof(receive_pattern));
    append(&buffer, "}");
    esp_err_t status = buffer.overflow ? protocol_error(request, ESP_ERR_INVALID_SIZE) : json(request, response);
    free(response);
    return status;
}

static esp_err_t uart_receive_io(operation_reply_t *request, const char *form)
{
    const char *fields[] = {"rx"};
    gpio_num_t rx;
    int baud, bytes, wait, idle;
    if (!number(form, "baud", UART_BATCH_MIN_BAUD, UART_BATCH_MAX_BAUD, &baud) ||
        !number(form, "bytes", 1, UART_BATCH_MAX_BYTES, &bytes) ||
        !number(form, "wait_ms", 100, 5000, &wait) ||
        !number(form, "idle_ms", 0, 1000, &idle) || (idle && (idle < 10 || idle > wait))) {
        return invalid(request, "Invalid UART baud, capacity, wait or idle window");
    }
    if (!active_pins(form, fields, 1, &rx)) {
        return conflict(request, protocol_pin_error);
    }
    uint8_t *data = malloc(bytes);
    if (!data) return protocol_error(request, ESP_ERR_NO_MEM);
    const uart_batch_capture_options_t options = {
        .rx = rx, .baud = baud, .capacity = bytes, .wait_ms = wait, .idle_ms = idle, .data = data,
    };
    uart_batch_capture_result_t capture;
    esp_err_t status = uart_batch_capture_run(&options, &capture);
    board_event_record(BOARD_EVENT_UART_RECEIVE, status == ESP_OK && capture.timed_out ? ESP_ERR_TIMEOUT : status,
                       status == ESP_OK ? capture.bytes : 0);
    status = status == ESP_OK ? send_uart_receive(request, baud, data, &capture, NULL)
                              : protocol_error(request, status);
    free(data);
    return status;
}

static void append_i2c_write_frames(response_buffer_t *buffer, const i2c_device_capture_result_t *capture)
{
    append(buffer, ",\"frames\":[");
    for (size_t i = 0; i < capture->frame_count; ++i) {
        const i2c_device_capture_frame_t *frame = &capture->frame[i];
        append(buffer, "%s{\"bytes\":%zu,\"total_bytes\":%zu,\"truncated\":%s,\"rx_hex\":\"",
               i ? "," : "", frame->bytes, frame->total_bytes, json_bool(frame->truncated));
        append_hex(buffer, frame->data, frame->bytes);
        append(buffer, "\"}");
    }
    append(buffer, "]");
}

static esp_err_t send_i2c_receive(operation_reply_t *request, const i2c_device_capture_options_t *options,
                                  const i2c_device_capture_result_t *capture, const receive_bench_t *bench,
                                  const uint8_t *readback, size_t readback_size)
{
    const size_t capacity = 512 + capture->frame_count * (options->capacity * 2 + 80);
    char *response = malloc(capacity);
    if (!response) return protocol_error(request, ESP_ERR_NO_MEM);
    response_buffer_t buffer = {response, capacity, false};
    append(&buffer, "{\"operation\":\"i2c_slave\",\"address\":%u,\"queued\":%u,\"captured\":%zu,\"timed_out\":%s",
           options->address, options->frames, capture->frame_count, json_bool(capture->timed_out));
    if (options->reply_len) {
        append(&buffer, ",\"register\":%u,\"read_requests\":%zu,\"reply_loaded\":%s,\"reply_loaded_bytes\":%zu,"
                        "\"read_completed\":%s,\"reply_hex\":\"",
               options->register_address, capture->read_requests, json_bool(capture->reply_loaded),
               capture->reply_loaded, json_bool(capture->read_completed));
        append_hex(&buffer, options->reply, options->reply_len);
        append(&buffer, "\"");
        append_receive_bench(&buffer, bench, &options->register_address, 1);
        if (bench) {
            append(&buffer, ",\"readback_hex\":\"");
            append_hex(&buffer, readback, readback_size);
            append(&buffer, "\"");
        }
    } else {
        append_receive_bench(&buffer, bench, i2c_receive_pattern, sizeof(i2c_receive_pattern));
    }
    append_i2c_write_frames(&buffer, capture);
    append(&buffer, "}");
    esp_err_t status = buffer.overflow ? protocol_error(request, ESP_ERR_INVALID_SIZE) : json(request, response);
    free(response);
    return status;
}

static bool i2c_fixed_reply(const char *form, bool required, uint8_t *register_address,
                            uint8_t reply[PROTOCOL_MAX_BYTES], size_t *reply_len)
{
    if (!hex_field(form, "reply", reply, reply_len)) return false;
    char value[4];
    const esp_err_t found = httpd_query_key_value(form, "register", value, sizeof(value));
    if (found == ESP_ERR_NOT_FOUND) return !required && *reply_len == 0;
    int reg;
    if (found != ESP_OK || !number(form, "register", 0, 255, &reg) ||
        !*reply_len || *reply_len > I2C_DEVICE_CAPTURE_MAX_REPLY) {
        return false;
    }
    *register_address = reg;
    return true;
}

static int hex_byte(const char *pair)
{
    const int high = nibble(pair[0]);
    const int low = nibble(pair[1]);
    return high < 0 || low < 0 ? -1 : high * 16 + low;
}

static const i2c_device_register_t *find_i2c_register(const i2c_device_register_t *entries, size_t count,
                                                      uint8_t address)
{
    for (size_t i = 0; i < count; ++i) {
        if (entries[i].address == address) return &entries[i];
    }
    return NULL;
}

static bool parse_i2c_registers(const char *form, const char *key,
                                i2c_device_register_t *registers, size_t *count)
{
    // Each entry is two header bytes (register, length), followed by payload.
    // Hex keeps the complete table within the existing form size limit.
    char encoded[1 + 2 * I2C_DEVICE_CAPTURE_MAX_REGISTERS * (2 + I2C_DEVICE_CAPTURE_MAX_REPLY)];
    const esp_err_t found = httpd_query_key_value(form, key, encoded, sizeof(encoded));
    *count = 0;
    if (found == ESP_ERR_NOT_FOUND) return true;
    if (found != ESP_OK || !encoded[0]) return false;

    const size_t size = strlen(encoded);
    size_t offset = 0;
    while (offset < size) {
        if (size - offset < 4 || *count == I2C_DEVICE_CAPTURE_MAX_REGISTERS) return false;
        const int address = hex_byte(encoded + offset);
        const int length = hex_byte(encoded + offset + 2);
        offset += 4;
        if (address < 0 || length < 1 || length > I2C_DEVICE_CAPTURE_MAX_REPLY ||
            size - offset < (size_t)length * 2) return false;
        if (find_i2c_register(registers, *count, address)) return false;
        i2c_device_register_t *entry = &registers[*count];
        entry->address = address;
        entry->reply_len = length;
        for (int i = 0; i < length; ++i) {
            const int byte = hex_byte(encoded + offset);
            if (byte < 0) return false;
            entry->reply[i] = byte;
            offset += 2;
        }
        ++*count;
    }
    return true;
}

static bool i2c_auto_increment(const char *form, bool *enabled)
{
    char value[4];
    const esp_err_t found = httpd_query_key_value(form, "auto_increment", value, sizeof(value));
    *enabled = found == ESP_OK && strcmp(value, "yes") == 0;
    return found == ESP_ERR_NOT_FOUND || *enabled;
}

typedef struct {
    uint8_t readback[I2C_DEVICE_CAPTURE_MAX_REPLY];
    size_t bytes_read;
    esp_err_t status;
} i2c_register_readback_t;

typedef struct {
    const i2c_device_register_t *value;
    bool writes_value;
    i2c_register_readback_t response;
} i2c_table_step_t;

typedef struct {
    fixture_gate_t gate;
    SemaphoreHandle_t done;
    const i2c_device_capture_options_t *options;
    unsigned requested_rate;
    unsigned actual_rate;
    size_t attempted;
    uint8_t burst_count;
    uint8_t burst_lengths[I2C_DEVICE_CAPTURE_MAX_REGISTERS];
    i2c_table_step_t steps[I2C_DEVICE_CAPTURE_MAX_FRAMES];
} i2c_table_test_t;

static bool parse_i2c_burst(const char *form, const i2c_device_register_t *registers, size_t register_count,
                            i2c_device_register_t *burst, i2c_table_test_t *test)
{
    int start;
    size_t length;
    uint8_t bytes[PROTOCOL_MAX_BYTES];
    if (!number(form, "start", 0, UINT8_MAX, &start) ||
        !hex_field(form, "burst", bytes, &length) || !length || length > sizeof(burst->reply)) {
        return false;
    }
    size_t offset = 0;
    for (int address = start; offset < length; ++address) {
        if (address > UINT8_MAX) return false;
        const i2c_device_register_t *entry = find_i2c_register(registers, register_count, address);
        if (!entry || entry->reply_len > length - offset) return false;
        test->burst_lengths[test->burst_count++] = entry->reply_len;
        offset += entry->reply_len;
    }
    burst->address = start;
    burst->reply_len = length;
    memcpy(burst->reply, bytes, length);
    return true;
}

static const i2c_device_capture_frame_t *i2c_table_write(const i2c_device_capture_result_t *capture,
                                                        const i2c_device_capture_transaction_t *transaction)
{
    return transaction && transaction->frame_index < capture->frame_count
            ? &capture->frame[transaction->frame_index] : NULL;
}

static bool i2c_table_verified(const i2c_table_step_t *step,
                               const i2c_device_capture_transaction_t *transaction,
                               const i2c_device_capture_frame_t *write, const i2c_register_readback_t *read)
{
    const i2c_device_register_t *expected = step->value;
    const size_t written = step->writes_value ? expected->reply_len : 0;
    return read && read->status == ESP_OK && transaction && transaction->matched && transaction->read_completed &&
           transaction->write_applied == step->writes_value &&
           write && write->total_bytes == 1 + written && write->bytes == write->total_bytes &&
           write->data[0] == expected->address && memcmp(write->data + 1, expected->reply, written) == 0 &&
           transaction->reply_loaded == expected->reply_len &&
           memcmp(transaction->reply, expected->reply, expected->reply_len) == 0 &&
           read->bytes_read == expected->reply_len && memcmp(read->readback, expected->reply, expected->reply_len) == 0;
}

static void append_i2c_burst_registers(response_buffer_t *buffer, const i2c_device_capture_transaction_t *transaction,
                                      const i2c_device_capture_frame_t *write, const i2c_register_readback_t *read,
                                      bool benchmark)
{
    append(buffer, ",\"registers\":[");
    if (transaction && write && write->bytes) {
        size_t offset = 0;
        const size_t write_bytes = write->bytes - 1;
        for (size_t i = 0; i < transaction->span_count; ++i) {
            const size_t length = transaction->span_lengths[i];
            append(buffer, "%s{\"register\":%u,\"write_hex\":\"", i ? "," : "", write->data[0] + (unsigned)i);
            if (offset < write_bytes) {
                const size_t available = write_bytes - offset;
                append_hex(buffer, write->data + 1 + offset, available < length ? available : length);
            }
            append(buffer, "\",\"reply_hex\":\"");
            append_hex(buffer, transaction->reply + offset, length);
            append(buffer, "\"");
            if (benchmark) {
                append(buffer, ",\"readback_hex\":\"");
                if (read && offset < read->bytes_read) {
                    const size_t available = read->bytes_read - offset;
                    append_hex(buffer, read->readback + offset, available < length ? available : length);
                }
                append(buffer, "\"");
            }
            append(buffer, "}");
            offset += length;
        }
    }
    append(buffer, "]");
}

static esp_err_t send_i2c_table(operation_reply_t *request, const i2c_device_capture_options_t *options,
                                const i2c_device_capture_result_t *capture, const i2c_table_test_t *test)
{
    size_t count = capture->transaction_count;
    if (test && test->attempted > count) count = test->attempted;
    size_t burst_capacity = 0;
    if (options->auto_increment) {
        for (size_t i = 0; i < capture->transaction_count; ++i) {
            const i2c_device_capture_transaction_t *transaction = &capture->transaction[i];
            burst_capacity += 80 * transaction->span_count + 6 * transaction->reply_loaded;
        }
        burst_capacity += 16 * count;
    }
    const size_t capacity = 512 + (capture->frame_count + count) * options->capacity * 2 +
                            capture->frame_count * 80 + count * (256 + 4 * I2C_DEVICE_CAPTURE_MAX_REPLY) +
                            burst_capacity;
    char *response = malloc(capacity);
    if (!response) return protocol_error(request, ESP_ERR_NO_MEM);
    response_buffer_t buffer = {response, capacity, false};

    append(&buffer, "{\"operation\":\"i2c_slave\",\"register_table\":true,\"address\":%u,\"queued\":%u,"
                    "\"captured\":%zu,\"timed_out\":%s",
           options->address, options->frames, capture->transaction_count, json_bool(capture->timed_out));
    if (options->auto_increment) append(&buffer, ",\"auto_increment\":true");
    append(&buffer, ",\"transactions\":[");
    bool passed = test && !capture->timed_out && capture->transaction_count == options->frames &&
                  test->attempted == options->frames;
    for (size_t i = 0; i < count; ++i) {
        const i2c_device_capture_transaction_t *transaction = i < capture->transaction_count
                                                              ? &capture->transaction[i] : NULL;
        const i2c_device_capture_frame_t *write = i2c_table_write(capture, transaction);
        append(&buffer, "%s{\"register\":", i ? "," : "");
        if (write && write->bytes) append(&buffer, "%u", write->data[0]);
        else append(&buffer, "null");
        append(&buffer, ",\"matched\":%s,\"write_applied\":%s,\"reply_loaded_bytes\":%zu,"
                        "\"read_completed\":%s,\"write_hex\":\"",
               json_bool(transaction && transaction->matched), json_bool(transaction && transaction->write_applied),
               transaction ? transaction->reply_loaded : 0,
               json_bool(transaction && transaction->read_completed));
        if (write) append_hex(&buffer, write->data, write->bytes);
        append(&buffer, "\",\"reply_hex\":\"");
        if (transaction) append_hex(&buffer, transaction->reply, transaction->reply_loaded);
        append(&buffer, "\"");
        const i2c_register_readback_t *read = NULL;
        if (test && i < options->frames) {
            const i2c_table_step_t *step = &test->steps[i];
            read = i < test->attempted ? &step->response : NULL;
            bool verified = i2c_table_verified(step, transaction, write, read);
            if (verified && options->auto_increment) {
                verified = transaction->span_count == test->burst_count &&
                           memcmp(transaction->span_lengths, test->burst_lengths, test->burst_count) == 0;
            }
            passed &= verified;
            append(&buffer, ",\"expected_register\":%u,\"passed\":%s,\"fixture_status\":\"%s\",\"readback_hex\":\"",
                   step->value->address, json_bool(verified), esp_err_to_name(read ? read->status : ESP_ERR_INVALID_STATE));
            if (read) append_hex(&buffer, read->readback, read->bytes_read);
            append(&buffer, "\"");
        }
        if (options->auto_increment) append_i2c_burst_registers(&buffer, transaction, write, read, test != NULL);
        append(&buffer, "}");
    }
    append(&buffer, "]");
    append_i2c_write_frames(&buffer, capture);
    if (test) {
        append(&buffer, ",\"bench\":true,\"passed\":%s,\"requested_rate\":%u,\"fixture_rate\":%u",
               json_bool(passed), test->requested_rate, test->actual_rate);
    }
    append(&buffer, "}");
    esp_err_t status = buffer.overflow ? protocol_error(request, ESP_ERR_INVALID_SIZE) : json(request, response);
    free(response);
    return status;
}

static esp_err_t i2c_slave_io(operation_reply_t *request, const char *form)
{
    const char *fields[] = {"sda", "scl"};
    gpio_num_t pins[2];
    int address, bytes, frames, wait;
    if (!number(form, "address", 8, 119, &address) ||
        !number(form, "bytes", 1, I2C_DEVICE_CAPTURE_MAX_BYTES, &bytes) ||
        !number(form, "count", 1, I2C_DEVICE_CAPTURE_MAX_FRAMES, &frames) ||
        !number(form, "wait_ms", 100, 5000, &wait)) {
        return invalid(request, "Invalid I2C address, capacity, frame count or wait");
    }
    uint8_t reg = 0, reply[PROTOCOL_MAX_BYTES];
    size_t reply_len;
    i2c_device_register_t registers[I2C_DEVICE_CAPTURE_MAX_REGISTERS];
    size_t register_count;
    bool auto_increment;
    if (!parse_i2c_registers(form, "registers", registers, &register_count) ||
        !i2c_auto_increment(form, &auto_increment) || (auto_increment && !register_count)) {
        return invalid(request, "Invalid I2C register table or duplicate register");
    }
    if (!i2c_fixed_reply(form, false, &reg, reply, &reply_len) || (reply_len && frames != 1)) {
        return invalid(request, "I2C fixed reply requires one register byte, one exchange and 1-32 reply bytes");
    }
    if (register_count && reply_len) return invalid(request, "Choose a register table or a single fixed reply");
    if (!active_pins(form, fields, 2, pins)) {
        return conflict(request, protocol_pin_error);
    }
    const i2c_device_capture_options_t options = {
        .sda = pins[0], .scl = pins[1], .address = address,
        .capacity = bytes, .frames = frames, .timeout_ms = wait,
        .register_address = reg, .reply = reply, .reply_len = reply_len,
        .registers = registers, .register_count = register_count, .auto_increment = auto_increment,
    };
    i2c_device_capture_result_t capture;
    const esp_err_t status = i2c_device_capture_run(&options, &capture);
    board_event_record(BOARD_EVENT_I2C_RECEIVE, status == ESP_OK && capture.timed_out ? ESP_ERR_TIMEOUT : status,
                       status == ESP_OK ? (register_count ? capture.transaction_count : capture.frame_count) : 0);
    if (status != ESP_OK) return protocol_error(request, status);
    return register_count ? send_i2c_table(request, &options, &capture, NULL)
                          : send_i2c_receive(request, &options, &capture, NULL, NULL, 0);
}

typedef struct {
    uart_receive_fixture_t *uart;
    receive_bench_t bench;
} uart_receive_test_t;

static void send_uart_test(void *context)
{
    uart_receive_test_t *test = context;
    test->bench.status = receive_fixture_uart_send(test->uart);
}

static esp_err_t uart_receive_bench(operation_reply_t *request, unsigned rate)
{
    matrix_test_links(0, (1U << 0) | (1U << 4));
    uart_receive_test_t test = {.bench = {.requested_rate = rate}};
    esp_err_t status = receive_fixture_uart_open(matrix_link_gpio(0), rate, receive_pattern,
                                                  sizeof(receive_pattern), &test.uart, &test.bench.actual_rate);
    uint8_t data[sizeof(receive_pattern)];
    uart_batch_capture_result_t result;
    if (status == ESP_OK) {
        const uart_batch_capture_options_t options = {
            .rx = matrix_link_gpio(4), .baud = test.bench.actual_rate, .wait_ms = 120,
            .capacity = sizeof(data), .data = data, .armed = send_uart_test, .armed_context = &test,
        };
        status = uart_batch_capture_run(&options, &result);
    }
    receive_fixture_uart_close(test.uart);
    const uint16_t cleared[MATRIX_Y_COUNT] = {0};
    matrix_apply(cleared);
    if (status != ESP_OK) return protocol_error(request, status);
    test.bench.passed = test.bench.status == ESP_OK && result.bytes == sizeof(receive_pattern) &&
                        !result.timed_out && !result.overflow && !result.events_lost &&
                        !result.frame_errors && !result.parity_errors && !result.break_events &&
                        memcmp(data, receive_pattern, sizeof(receive_pattern)) == 0;
    return send_uart_receive(request, rate, data, &result, &test.bench);
}

static void send_i2c_test(void *context)
{
    receive_bench_t *bench = context;
    unsigned total_hz = 0;
    for (unsigned i = 0; i < 2; ++i) {
        bench->status = receive_fixture_i2c_write(matrix_link_gpio(0), matrix_link_gpio(1), 0x48,
                                                 bench->requested_rate, i2c_receive_pattern,
                                                 sizeof(i2c_receive_pattern), &bench->actual_rate);
        if (bench->status != ESP_OK) break;
        total_hz += bench->actual_rate;
        esp_rom_delay_us(100);
    }
    if (bench->status == ESP_OK) bench->actual_rate = total_hz / 2;
}

static void setup_i2c_fixture(void)
{
    // Two additional input-pad pull-ups per wire support the open-drain
    // fixture. Put SCL on Y1 to use the board's 10 kΩ pull-up.
    const uint8_t links[MATRIX_X_COUNT] = {0x1d, 0xe2};
    matrix_test_routes(links);
    for (unsigned y = 0; y < MATRIX_Y_COUNT; ++y) {
        gpio_set_pull_mode(matrix_link_gpio(y), GPIO_PULLUP_ONLY);
    }
    // Prepare the host once so each following transfer can start as soon as
    // the bus is idle, without reconfiguring or logging the pin state.
    for (unsigned y = 0; y < 2; ++y) {
        const gpio_num_t pin = matrix_link_gpio(y);
        gpio_set_level(pin, 1);
        gpio_set_direction(pin, GPIO_MODE_INPUT_OUTPUT_OD);
    }
}

static esp_err_t i2c_slave_bench(operation_reply_t *request, unsigned rate)
{
    setup_i2c_fixture();
    receive_bench_t bench = {.requested_rate = rate};
    const i2c_device_capture_options_t options = {
        .sda = matrix_link_gpio(4), .scl = matrix_link_gpio(5), .address = 0x48,
        .frames = 2, .capacity = sizeof(i2c_receive_pattern), .timeout_ms = 200, .internal_pullup = true,
        .armed = send_i2c_test, .armed_context = &bench,
    };
    i2c_device_capture_result_t result;
    const esp_err_t status = i2c_device_capture_run(&options, &result);
    const uint16_t cleared[MATRIX_Y_COUNT] = {0};
    matrix_apply(cleared);
    if (status != ESP_OK) return protocol_error(request, status);
    bench.passed = bench.status == ESP_OK && result.frame_count == options.frames && !result.timed_out;
    for (size_t i = 0; i < result.frame_count; ++i) {
        const i2c_device_capture_frame_t *frame = &result.frame[i];
        bench.passed &= !frame->truncated && frame->bytes == sizeof(i2c_receive_pattern) &&
                        memcmp(frame->data, i2c_receive_pattern, sizeof(i2c_receive_pattern)) == 0;
    }
    return send_i2c_receive(request, &options, &result, &bench, NULL, 0);
}

typedef struct {
    fixture_gate_t gate;
    SemaphoreHandle_t done;
    receive_bench_t bench;
    const i2c_device_capture_options_t *options;
    uint8_t readback[I2C_DEVICE_CAPTURE_MAX_REPLY];
    size_t bytes_read;
} i2c_register_test_t;

static void run_i2c_register_host(void *context)
{
    i2c_register_test_t *test = context;
    test->bench.status = ESP_ERR_INVALID_STATE;
    if (fixture_wait(&test->gate)) {
        test->bench.status = receive_fixture_i2c_write_read(
            matrix_link_gpio(0), matrix_link_gpio(1),
            test->options->address, test->bench.requested_rate, &test->options->register_address, 1,
            test->readback, test->options->reply_len, &test->bytes_read, &test->bench.actual_rate);
    }
    xSemaphoreGive(test->done);
    vTaskDelete(NULL);
}

static bool receive_bench_confirmed(const char *form)
{
    char confirm[8];
    return !matrix_has_routes() && field(form, "confirm_empty", confirm, sizeof(confirm)) && strcmp(confirm, "yes") == 0;
}

static esp_err_t i2c_register_bench(operation_reply_t *request, const char *form)
{
    if (!receive_bench_confirmed(form)) {
        return conflict(request, "Clear routes and unload all BBX pins before I2C register benchmark");
    }
    int address, rate;
    uint8_t reg, reply[PROTOCOL_MAX_BYTES];
    size_t reply_len;
    if (!number(form, "rate", 10000, 400000, &rate) || !number(form, "address", 8, 119, &address) ||
        !i2c_fixed_reply(form, true, &reg, reply, &reply_len)) {
        return invalid(request, "Invalid I2C register benchmark address, reply or clock");
    }

    i2c_register_test_t test = {
        .gate = {.armed = xSemaphoreCreateBinary()}, .done = xSemaphoreCreateBinary(),
        .bench = {.requested_rate = rate},
    };
    const i2c_device_capture_options_t options = {
        .sda = matrix_link_gpio(4), .scl = matrix_link_gpio(5), .address = address,
        .frames = 1, .capacity = I2C_DEVICE_CAPTURE_MAX_BYTES, .timeout_ms = 200, .internal_pullup = true,
        .register_address = reg, .reply = reply, .reply_len = reply_len,
        .armed = fixture_ready, .armed_context = &test.gate,
    };
    test.options = &options;
    esp_err_t status = ESP_ERR_NO_MEM;
    i2c_device_capture_result_t result;
    if (test.gate.armed && test.done) {
        setup_i2c_fixture();
        if (xTaskCreate(run_i2c_register_host, "i2c_register_host", 3072, &test, 4, NULL) == pdPASS) {
            status = i2c_device_capture_run(&options, &result);
            fixture_cancel(&test.gate);
            xSemaphoreTake(test.done, portMAX_DELAY);
        }
        const uint16_t cleared[MATRIX_Y_COUNT] = {0};
        matrix_apply(cleared);
    }
    if (test.gate.armed) vSemaphoreDelete(test.gate.armed);
    if (test.done) vSemaphoreDelete(test.done);
    if (status != ESP_OK) return protocol_error(request, status);

    test.bench.passed = test.bench.status == ESP_OK && !result.timed_out &&
                        result.reply_loaded == reply_len && result.read_requests == 1 &&
                        result.read_completed && result.frame_count == 1 &&
                        result.frame[0].total_bytes == 1 && result.frame[0].data[0] == reg &&
                        test.bytes_read == reply_len && memcmp(test.readback, reply, reply_len) == 0;
    return send_i2c_receive(request, &options, &result, &test.bench, test.readback, test.bytes_read);
}

static void run_i2c_table_host(void *context)
{
    i2c_table_test_t *test = context;
    if (fixture_wait(&test->gate)) {
        const i2c_device_capture_options_t *options = test->options;
        unsigned total_rate = 0;
        for (size_t i = 0; i < options->frames; ++i) {
            i2c_table_step_t *step = &test->steps[i];
            const i2c_device_register_t *entry = step->value;
            uint8_t write[1 + I2C_DEVICE_CAPTURE_MAX_REPLY] = {entry->address};
            const size_t write_bytes = step->writes_value ? entry->reply_len : 0;
            memcpy(write + 1, entry->reply, write_bytes);
            i2c_register_readback_t *read = &step->response;
            unsigned actual_rate = 0;
            read->status = receive_fixture_i2c_write_read(
                matrix_link_gpio(0), matrix_link_gpio(1), options->address, test->requested_rate,
                write, 1 + write_bytes, read->readback, entry->reply_len, &read->bytes_read, &actual_rate);
            total_rate += actual_rate;
            ++test->attempted;
            if (read->status != ESP_OK) break;
        }
        if (test->attempted) test->actual_rate = total_rate / test->attempted;
    }
    xSemaphoreGive(test->done);
    vTaskDelete(NULL);
}

static esp_err_t i2c_register_table_bench(operation_reply_t *request, const char *form)
{
    if (!receive_bench_confirmed(form)) {
        return conflict(request, "Clear routes and unload all BBX pins before I2C register table benchmark");
    }
    int address, rate;
    i2c_device_register_t registers[I2C_DEVICE_CAPTURE_MAX_REGISTERS];
    i2c_device_register_t writes[I2C_DEVICE_CAPTURE_MAX_REGISTERS];
    size_t register_count, write_count;
    bool auto_increment;
    if (!number(form, "rate", 10000, 400000, &rate) || !number(form, "address", 8, 119, &address) ||
        !parse_i2c_registers(form, "registers", registers, &register_count) || !register_count ||
        !parse_i2c_registers(form, "writes", writes, &write_count) ||
        !i2c_auto_increment(form, &auto_increment)) {
        return invalid(request, "Invalid I2C register table, address or clock");
    }

    i2c_table_test_t test = {.requested_rate = rate};
    i2c_device_register_t burst;
    size_t frames = 1;
    if (auto_increment) {
        if (write_count || !parse_i2c_burst(form, registers, register_count, &burst, &test)) {
            return invalid(request, "I2C burst needs a start and 1-32 bytes spanning whole adjacent registers");
        }
        test.steps[0] = (i2c_table_step_t){.value = &burst, .writes_value = true};
    } else {
        for (size_t i = 0; i < write_count; ++i) {
            if (!find_i2c_register(registers, register_count, writes[i].address)) {
                return invalid(request, "I2C test writes must target configured registers");
            }
        }
        for (size_t i = 0; i < register_count; ++i) {
            const i2c_device_register_t *update = find_i2c_register(writes, write_count, registers[i].address);
            test.steps[i] = (i2c_table_step_t){.value = update ? update : &registers[i], .writes_value = update != NULL};
        }
        frames = register_count;
        // Read updated values again after visiting the other registers.
        for (size_t i = register_count; i > 0 && frames < I2C_DEVICE_CAPTURE_MAX_FRAMES; --i) {
            const i2c_table_step_t *step = &test.steps[i - 1];
            if (step->writes_value) test.steps[frames++].value = step->value;
        }
    }
    test.gate.armed = xSemaphoreCreateBinary();
    test.done = xSemaphoreCreateBinary();
    const i2c_device_capture_options_t options = {
        .sda = matrix_link_gpio(4), .scl = matrix_link_gpio(5), .address = address,
        .frames = frames, .capacity = I2C_DEVICE_CAPTURE_MAX_BYTES,
        .timeout_ms = 1000, .internal_pullup = true,
        .registers = registers, .register_count = register_count, .auto_increment = auto_increment,
        .armed = fixture_ready, .armed_context = &test.gate,
    };
    test.options = &options;
    esp_err_t status = ESP_ERR_NO_MEM;
    i2c_device_capture_result_t result;
    if (test.gate.armed && test.done) {
        setup_i2c_fixture();
        if (xTaskCreate(run_i2c_table_host, "i2c_table_host", 3072, &test, 4, NULL) == pdPASS) {
            status = i2c_device_capture_run(&options, &result);
            fixture_cancel(&test.gate);
            xSemaphoreTake(test.done, portMAX_DELAY);
        }
        const uint16_t cleared[MATRIX_Y_COUNT] = {0};
        matrix_apply(cleared);
    }
    if (test.gate.armed) vSemaphoreDelete(test.gate.armed);
    if (test.done) vSemaphoreDelete(test.done);
    if (status != ESP_OK) return protocol_error(request, status);
    return send_i2c_table(request, &options, &result, &test);
}

static esp_err_t protocol_receive_bench(operation_reply_t *request, const char *form)
{
    char mode[8];
    if (!receive_bench_confirmed(form)) {
        return conflict(request, "Clear routes and unload all BBX pins before protocol receive benchmark");
    }
    if (!field(form, "mode", mode, sizeof(mode)) || (strcmp(mode, "uart") && strcmp(mode, "i2c"))) {
        return invalid(request, "Invalid receive benchmark protocol");
    }
    int rate;
    const bool uart = strcmp(mode, "uart") == 0;
    if (!number(form, "rate", uart ? UART_BATCH_MIN_BAUD : 10000, uart ? UART_BATCH_MAX_BAUD : 400000, &rate)) {
        return invalid(request, "Invalid receive benchmark rate");
    }
    return uart ? uart_receive_bench(request, rate) : i2c_slave_bench(request, rate);
}

static void append_spi_slave_frames(response_buffer_t *buffer, const spi_device_capture_result_t *capture)
{
    append(buffer, ",\"captured\":%zu,\"timed_out\":%s,\"frames\":[",
           capture->frame_count, json_bool(capture->timed_out));
    for (size_t i = 0; i < capture->frame_count; ++i) {
        const spi_device_capture_frame_t *frame = &capture->frame[i];
        char hex[2 * SPI_DEVICE_CAPTURE_MAX_BYTES + 1];
        to_hex(frame->data, frame->bytes, hex);
        append(buffer, "%s{\"bits\":%zu,\"rx_hex\":\"%s\",\"truncated\":%s,\"length_saturated\":%s}",
               i ? "," : "", frame->bits, hex, json_bool(frame->truncated), json_bool(frame->length_saturated));
    }
    append(buffer, "]}");
}

static esp_err_t spi_slave_io(operation_reply_t *request, const char *form)
{
    const char *fields[] = {"cs", "sck", "mosi", "miso"};
    gpio_num_t pins[] = {GPIO_NUM_NC, GPIO_NUM_NC, GPIO_NUM_NC, GPIO_NUM_NC};
    int mode, count, capacity, wait_ms, miso;
    char order[4];
    uint8_t reply[PROTOCOL_MAX_BYTES] = {0};
    size_t reply_len;
    if (!number(form, "mode", 0, 3, &mode) ||
        !number(form, "count", 1, SPI_DEVICE_CAPTURE_MAX_FRAMES, &count) ||
        !number(form, "bytes", 4, PROTOCOL_MAX_BYTES, &capacity) || capacity % 4 ||
        !number(form, "wait_ms", 100, 5000, &wait_ms) ||
        !number(form, "miso", -1, MATRIX_Y_COUNT - 1, &miso) ||
        !field(form, "order", order, sizeof(order)) || (strcmp(order, "msb") && strcmp(order, "lsb")) ||
        !hex_field(form, "reply", reply, &reply_len) || reply_len > capacity || (miso < 0) != (reply_len == 0)) {
        return invalid(request, "Invalid SPI slave options or reply. Use 4-byte aligned frame capacity.");
    }
    if (!active_pins(form, fields, miso < 0 ? 3 : 4, pins)) {
        return conflict(request, protocol_pin_error);
    }
    spi_device_capture_result_t *capture = malloc(sizeof(*capture));
    size_t response_size = 256 + count * (capacity * 2 + 100);
    char *response = malloc(response_size);
    if (!capture || !response) {
        free(response);
        free(capture);
        return protocol_error(request, ESP_ERR_NO_MEM);
    }
    const spi_device_capture_options_t options = {
        .cs = pins[0], .sck = pins[1], .mosi = pins[2], .miso = pins[3],
        .mode = mode, .lsb_first = strcmp(order, "lsb") == 0, .frames = count, .capacity = capacity,
        .timeout_ms = wait_ms, .reply = reply, .reply_len = reply_len,
    };
    gpio_set_pull_mode(options.cs, GPIO_PULLUP_ONLY);
    esp_err_t result = spi_device_capture_run(&options, capture);
    board_event_record(BOARD_EVENT_SPI_RECEIVE, result == ESP_OK && capture->timed_out ? ESP_ERR_TIMEOUT : result,
                       result == ESP_OK ? capture->frame_count : 0);
    size_t pin_count = miso < 0 ? 3 : 4;
    for (size_t i = 0; i < pin_count; ++i) {
        gpio_set_direction(pins[i], GPIO_MODE_INPUT);
        gpio_set_pull_mode(pins[i], GPIO_FLOATING);
    }
    if (result == ESP_OK) {
        char hex[2 * PROTOCOL_MAX_BYTES + 1];
        to_hex(reply, reply_len ? capacity : 0, hex);
        response_buffer_t buffer = {response, response_size, false};
        append(&buffer, "{\"operation\":\"spi_slave\",\"mode\":%d,\"order\":\"%s\","
                        "\"reply_hex\":\"%s\",\"queued\":%d", mode, order, hex, count);
        append_spi_slave_frames(&buffer, capture);
        result = !buffer.overflow ? json(request, response) : protocol_error(request, ESP_ERR_INVALID_SIZE);
    } else {
        result = protocol_error(request, result);
    }
    free(response);
    free(capture);
    return result;
}

enum { SPI_SLAVE_BENCH_BYTES = 4 };
static const uint8_t spi_slave_pattern[SPI_SLAVE_BENCH_BYTES] = {0x9f, 0x51, 0x36, 0xa5};
static const uint8_t spi_slave_zeros[SPI_SLAVE_BENCH_BYTES] = {0};
static const uint8_t spi_slave_reply[SPI_SLAVE_BENCH_BYTES] = {0xd2, 0x69, 0x8b, 0x54};

typedef struct {
    fixture_gate_t gate;
    SemaphoreHandle_t prepared;
    SemaphoreHandle_t done;
    bool pair;
    bool duplex;
    bool i2s;
    unsigned hz;
    unsigned actual_hz;
    uint32_t gap_ns;
    uint32_t actual_gap_ns;
    esp_err_t result;
    esp_err_t probe_result;
    bool outputs_ready;
    unsigned replies;
    uint8_t observed[2 * SPI_SLAVE_BENCH_BYTES];
} spi_slave_fixture_t;

static bool spi_slave_host_ready(void *context)
{
    spi_slave_fixture_t *test = context;
    test->outputs_ready = true;
    xSemaphoreGive(test->prepared);
    return fixture_wait(&test->gate);
}

static void run_spi_slave_host(void *context)
{
    spi_slave_fixture_t *test = context;
    const gpio_num_t sck = matrix_link_gpio(0), mosi = matrix_link_gpio(1), cs = matrix_link_gpio(2);
    if (test->pair) {
        test->result = spi_stimulus_transfer_pair(sck, mosi, cs, test->hz, test->gap_ns, false,
                                                  &test->actual_hz, &test->actual_gap_ns,
                                                  spi_slave_host_ready, test);
    } else if (test->i2s) {
        const gpio_num_t miso = test->duplex ? matrix_link_gpio(3) : GPIO_NUM_NC;
        test->result = spi_i2s_host_transfer(sck, mosi, miso, cs, test->hz, spi_slave_pattern,
                                            test->duplex ? test->observed : NULL,
                                            spi_slave_host_ready, test, &test->actual_hz);
        if (test->duplex && test->result == ESP_OK) test->replies = 1;
    } else {
        test->result = spi_stimulus_transfer_bench(sck, mosi, cs, test->hz, spi_slave_pattern, SPI_SLAVE_BENCH_BYTES,
                                                   spi_slave_host_ready, test, &test->actual_hz);
    }
    if (!test->outputs_ready) xSemaphoreGive(test->prepared);
    if (test->result == ESP_OK && test->duplex && !test->i2s) {
        unsigned count = test->pair ? 2 : 1;
        for (; test->replies < count; ++test->replies) {
            test->probe_result = spi_reply_probe_read(test->observed + test->replies * SPI_SLAVE_BENCH_BYTES, 30);
            if (test->probe_result != ESP_OK) break;
        }
    }
    xSemaphoreGive(test->done);
    vTaskDelete(NULL);
}

static esp_err_t spi_slave_bench(operation_reply_t *request, const char *form)
{
    char confirm[8], phase[8];
    int hz, gap_ns = 0;
    if (matrix_has_routes() || !field(form, "confirm_empty", confirm, sizeof(confirm)) || strcmp(confirm, "yes")) {
        return conflict(request, "Clear routes and unload all BBX pins before SPI slave benchmark");
    }
    if (!field(form, "phase", phase, sizeof(phase)) ||
        (strcmp(phase, "rx") && strcmp(phase, "duplex") && strcmp(phase, "gap")) ||
        !number(form, "hz", SPI_STIMULUS_MIN_HZ, SPI_I2S_HOST_MAX_HZ, &hz)) {
        return invalid(request, "Invalid bench phase or clock");
    }
    const bool pair = strcmp(phase, "gap") == 0, duplex = strcmp(phase, "rx") != 0;
    if (pair && hz > SPI_STIMULUS_MAX_HZ) {
        return invalid(request, "Two-frame RMT clock must be at most 3333334 Hz");
    }
    if (pair && !number(form, "gap_ns", SPI_STIMULUS_MIN_GAP_NS, SPI_STIMULUS_MAX_GAP_NS, &gap_ns)) {
        return invalid(request, "CS gap must be 150-1000000 ns");
    }
    spi_device_capture_result_t *capture = malloc(sizeof(*capture));
    spi_slave_fixture_t test = {
        .done = xSemaphoreCreateBinary(), .gate = {.armed = xSemaphoreCreateBinary()},
        .prepared = xSemaphoreCreateBinary(),
        .pair = pair, .duplex = duplex, .i2s = hz > SPI_STIMULUS_MAX_HZ, .hz = hz, .gap_ns = gap_ns,
    };
    esp_err_t result = ESP_ERR_NO_MEM;
    if (!capture || !test.done || !test.gate.armed || !test.prepared) {
        result = protocol_error(request, result);
        goto done;
    }
    // X0–X2 route the host's SCK/MOSI/CS to the slave. X3 returns its MISO.
    uint8_t links[MATRIX_X_COUNT] = {0x11, 0x22, 0x44, duplex ? 0x88 : 0};
    matrix_test_routes(links);
    for (unsigned i = 0; i < 3; ++i) {
        gpio_num_t pin = matrix_link_gpio(i);
        gpio_set_level(pin, i == 2);
        gpio_set_direction(pin, GPIO_MODE_OUTPUT);
    }
    const spi_device_capture_options_t options = {
        .sck = matrix_link_gpio(4), .mosi = matrix_link_gpio(5), .cs = matrix_link_gpio(6),
        .miso = duplex ? matrix_link_gpio(7) : GPIO_NUM_NC,
        .mode = 0, .frames = pair ? 2 : 1, .capacity = SPI_SLAVE_BENCH_BYTES, .timeout_ms = 150,
        .reply = spi_slave_reply, .reply_len = duplex ? sizeof(spi_slave_reply) : 0,
        .armed = fixture_ready, .armed_context = &test.gate,
    };
    gpio_set_pull_mode(options.cs, GPIO_PULLUP_ONLY);
    if (xTaskCreate(run_spi_slave_host, "spi_slave_host", 4096, &test, 4, NULL) == pdPASS) {
        xSemaphoreTake(test.prepared, portMAX_DELAY);
        bool probe_started = false;
        result = test.result;
        if (test.outputs_ready) {
            if (duplex && !test.i2s) {
                result = spi_reply_probe_begin(options.sck, options.cs, matrix_link_gpio(3), pair);
                probe_started = result == ESP_OK;
            }
            if (result == ESP_OK) result = spi_device_capture_run(&options, capture);
        }
        fixture_cancel(&test.gate);
        xSemaphoreTake(test.done, portMAX_DELAY);
        if (result == ESP_OK) result = test.result;
        if (probe_started) {
            esp_err_t status = spi_reply_probe_end();
            if (result == ESP_OK) result = status;
        }
    }
    {
        const uint16_t cleared[MATRIX_Y_COUNT] = {0};
        matrix_apply(cleared);
    }
    if (result == ESP_OK) {
        const uint8_t *expected = pair ? spi_slave_zeros : spi_slave_pattern;
        bool rx_passed = capture->frame_count == options.frames && !capture->timed_out;
        for (size_t i = 0; i < capture->frame_count; ++i) {
            const spi_device_capture_frame_t *frame = &capture->frame[i];
            rx_passed &= frame->bits == SPI_SLAVE_BENCH_BYTES * 8 &&
                         memcmp(expected, frame->data, SPI_SLAVE_BENCH_BYTES) == 0;
        }
        bool tx_verified = duplex && test.replies == options.frames;
        for (unsigned i = 0; i < test.replies; ++i) {
            tx_verified &= memcmp(test.observed + i * SPI_SLAVE_BENCH_BYTES, spi_slave_reply, SPI_SLAVE_BENCH_BYTES) == 0;
        }
        char expected_hex[2 * SPI_SLAVE_BENCH_BYTES + 1], reply_hex[2 * SPI_SLAVE_BENCH_BYTES + 1];
        char observed_hex[4 * SPI_SLAVE_BENCH_BYTES + 1], response[600];
        to_hex(expected, SPI_SLAVE_BENCH_BYTES, expected_hex);
        to_hex(spi_slave_reply, options.reply_len, reply_hex);
        to_hex(test.observed, test.replies * SPI_SLAVE_BENCH_BYTES, observed_hex);
        response_buffer_t buffer = {response, sizeof(response), false};
        append(&buffer, "{\"operation\":\"spi_slave_bench\",\"phase\":\"%s\",\"fixture\":\"%s\",\"requested_hz\":%d,"
                        "\"clock_hz\":%u,\"gap_ns\":%" PRIu32 ",\"passed\":%s,\"rx_passed\":%s,\"tx_verified\":%s,"
                        "\"expected_rx\":\"%s\",\"reply_hex\":\"%s\",\"observed_reply\":\"%s\","
                        "\"observed_reply_frames\":%u,\"probe_status\":\"%s\"",
               phase, test.i2s ? "i2s" : "rmt", hz, test.actual_hz, test.actual_gap_ns, json_bool(rx_passed && (!duplex || tx_verified)),
               json_bool(rx_passed), json_bool(tx_verified), expected_hex, reply_hex, observed_hex,
               test.replies, esp_err_to_name(test.probe_result));
        append_spi_slave_frames(&buffer, capture);
        if (!buffer.overflow) {
            result = json(request, response);
        } else {
            result = protocol_error(request, ESP_ERR_INVALID_SIZE);
        }
    } else {
        result = protocol_error(request, result);
    }
done:
    if (test.prepared) vSemaphoreDelete(test.prepared);
    if (test.gate.armed) vSemaphoreDelete(test.gate.armed);
    if (test.done) vSemaphoreDelete(test.done);
    free(capture);
    return result;
}

#ifdef CONFIG_AI_BOARD_OUT_CONTROL
static esp_err_t configure_out(operation_reply_t *request, const char *form)
{
    char mode[8];
    if (!field(form, "mode", mode, sizeof(mode)) ||
        (strcmp(mode, "5v") != 0 && strcmp(mode, "gnd") != 0)) {
        return invalid(request, "OUT mode must be 5v or gnd");
    }
    esp_err_t result = board_set_out_grounded(strcmp(mode, "gnd") == 0);
    return result == ESP_OK ? state_locked(request) : protocol_error(request, result);
}

#endif

static esp_err_t dispatch(operation_reply_t *request, const char *form, const char *op)
{
#ifdef CONFIG_AI_BOARD_OUT_CONTROL
    if (strcmp(op, "power") == 0) return configure_out(request, form);
#endif
    if (strcmp(op, "route") == 0) return configure_routes(request, form, false);
    if (strcmp(op, "reset") == 0) return configure_routes(request, form, true);
    if (strcmp(op, "gpio") == 0) return configure_gpio(request, form);
    if (strcmp(op, "uart") == 0) return uart_io(request, form);
    if (strcmp(op, "proxy") == 0) return uart_proxy(request, form);
    if (strcmp(op, "i2c_scan") == 0) return i2c_io(request, form, true, false);
    if (strcmp(op, "i2c_read") == 0) return i2c_io(request, form, false, false);
    if (strcmp(op, "i2c_transfer") == 0) return i2c_io(request, form, false, true);
    if (strcmp(op, "spi") == 0) return spi_io(request, form);
    if (strcmp(op, "i2c_slave") == 0) return i2c_slave_io(request, form);
    if (strcmp(op, "i2c_register_bench") == 0) return i2c_register_bench(request, form);
    if (strcmp(op, "i2c_register_table_bench") == 0) return i2c_register_table_bench(request, form);
    if (strcmp(op, "uart_receive") == 0) return uart_receive_io(request, form);
    if (strcmp(op, "protocol_receive_bench") == 0) return protocol_receive_bench(request, form);
    if (strcmp(op, "spi_slave") == 0) return spi_slave_io(request, form);
    if (strcmp(op, "spi_slave_bench") == 0) return spi_slave_bench(request, form);
    if (strcmp(op, "capture") == 0) return capture_io(request, form);
    if (strcmp(op, "uart_loopback") == 0) return uart_loopback(request, form);
    if (strcmp(op, "spi_capture_loopback") == 0) return protocol_capture_loopback(request, form, false, false);
    if (strcmp(op, "i2c_capture_loopback") == 0) return protocol_capture_loopback(request, form, true, false);
    if (strcmp(op, "spi_host_capture") == 0) return protocol_capture_loopback(request, form, false, true);
    if (strcmp(op, "i2c_host_capture") == 0) return protocol_capture_loopback(request, form, true, true);
    return invalid(request, "Unknown workbench operation");
}

static esp_err_t run_command(operation_reply_t *reply, const char *op, const char *form)
{
    bool requires_matrix = true;
#ifdef CONFIG_AI_BOARD_OUT_CONTROL
    requires_matrix = strcmp(op, "power") != 0;
#endif
    if (requires_matrix && !board_matrix_is_armed()) return conflict(reply, "Arm the matrix first");
    if (!board_matrix_acquire()) return conflict(reply, "Matrix is busy");
    const esp_err_t result = dispatch(reply, form, op);
    board_matrix_release();
    return result;
}

esp_err_t workbench_command(httpd_req_t *request, bool publish_result)
{
    char form[1280], op[sizeof(latest_op)];
    operation_reply_t reply = {.http = request};
    if (!request->content_len || request->content_len >= sizeof(form)) return invalid(&reply, "Invalid workbench request size");
    size_t used = 0;
    while (used < request->content_len) {
        int received = httpd_req_recv(request, form + used, request->content_len - used);
        if (received <= 0) return invalid(&reply, "Incomplete workbench request");
        used += received;
    }
    if (memchr(form, 0, used)) return invalid(&reply, "Unexpected NUL in workbench request");
    form[used] = 0;
    if (!field(form, "op", op, sizeof(op))) return invalid(&reply, "Missing workbench operation");
    const bool state_only = strcmp(op, "route") == 0 || strcmp(op, "reset") == 0 ||
                            strcmp(op, "gpio") == 0 || strcmp(op, "power") == 0;
    if (publish_result && !state_only) reply.publish_operation = op;
    return run_command(&reply, op, form);
}

esp_err_t workbench_lua_command(const char *op, const char *form, uint32_t remaining_ms,
                                char **output, const char **diagnostic)
{
    *output = NULL;
    *diagnostic = NULL;
    // Leave time for the driver to finish and restore its pins. Lua checks its
    // deadline again after the bounded hardware operation returns.
    int wait_ms = 0, window_ms = 0, timeout_ms = 0;
    number(form, "wait_ms", 0, 5000, &wait_ms);
    number(form, "ms", 0, 500, &window_ms);
    number(form, "timeout", 0, 500, &timeout_ms);
    if (strcmp(op, "proxy") == 0) timeout_ms *= 2;
    if (remaining_ms <= (uint32_t)(wait_ms + window_ms + timeout_ms)) {
        *diagnostic = "Hardware operation exceeds the remaining Lua time budget";
        return ESP_ERR_TIMEOUT;
    }
    operation_reply_t reply = {.output = output, .diagnostic = diagnostic};
    return strcmp(op, "state") == 0 ? read_state(&reply) : run_command(&reply, op, form);
}

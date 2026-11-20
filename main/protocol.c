#include <stdbool.h>
#include "protocol.h"
#include "driver/i2c_master.h"
#include "driver/uart.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

enum {
    BENCH_UART = UART_NUM_1,
    I2C_FIRST_ADDRESS = 0x08,
    I2C_LAST_ADDRESS = 0x77,
};

_Static_assert(PROTOCOL_I2C_MAX_ADDRESSES == I2C_LAST_ADDRESS - I2C_FIRST_ADDRESS + 1,
               "I2C scan buffer must cover all usable addresses");

static bool valid_pins(const gpio_num_t *pins, size_t count)
{
    uint64_t used = 0;
    for (size_t i = 0; i < count; ++i) {
        if (!GPIO_IS_VALID_OUTPUT_GPIO(pins[i])) return false;
        uint64_t bit = UINT64_C(1) << pins[i];
        if (used & bit) return false;
        used |= bit;
    }
    return true;
}

static void release_pin(gpio_num_t pin)
{
    if (pin == GPIO_NUM_NC) return;
    gpio_reset_pin(pin);
    gpio_set_pull_mode(pin, GPIO_FLOATING);
    gpio_set_direction(pin, GPIO_MODE_INPUT);
}

static TickType_t ticks_for_ms(uint32_t milliseconds)
{
    return pdMS_TO_TICKS(milliseconds) + 1;
}

static bool uart_timing_valid(uint32_t baud, uint32_t timeout_ms)
{
    return baud >= 1200 && baud <= 4000000 && timeout_ms >= 20 && timeout_ms <= 500;
}

static void uart_close(gpio_num_t tx, gpio_num_t rx)
{
    uart_driver_delete(BENCH_UART);
    release_pin(tx);
    release_pin(rx);
}

static esp_err_t uart_open(gpio_num_t tx, gpio_num_t rx, uint32_t baud)
{
    uart_config_t config = {
        .baud_rate = baud,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t result = uart_driver_install(BENCH_UART, 256, 0, 0, NULL, 0);
    if (result != ESP_OK) return result;
    if (rx != GPIO_NUM_NC) gpio_set_pull_mode(rx, GPIO_PULLUP_ONLY);
    result = uart_param_config(BENCH_UART, &config);
    if (result == ESP_OK) {
        result = uart_set_pin(BENCH_UART, tx, rx, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    }
    if (result != ESP_OK) uart_close(tx, rx);
    return result;
}

static esp_err_t uart_send(const uint8_t *bytes, size_t count, uint32_t baud)
{
    if (count == 0) return ESP_OK;
    if (uart_write_bytes(BENCH_UART, bytes, count) != (int)count) return ESP_FAIL;
    // Ten bits on the wire per byte in 8N1, plus time for the UART ISR.
    uint32_t tx_ms = (count * 10000 + baud - 1) / baud;
    return uart_wait_tx_done(BENCH_UART, ticks_for_ms(tx_ms + 30));
}

static esp_err_t uart_read_response(uint8_t *bytes, size_t capacity,
                                    uint32_t timeout_ms, size_t *length)
{
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    while (*length < capacity) {
        int64_t remaining = deadline - esp_timer_get_time();
        if (remaining <= 0) break;
        unsigned wait_ms = (remaining + 999) / 1000;
        int received = uart_read_bytes(BENCH_UART, bytes + *length, 1, ticks_for_ms(wait_ms));
        if (received < 0) return ESP_FAIL;
        if (received == 0) break;
        ++*length;
    }
    return ESP_OK;
}

esp_err_t protocol_uart(gpio_num_t tx, gpio_num_t rx, uint32_t baud,
                        const uint8_t *send, size_t send_len,
                        uint8_t *recv, size_t recv_capacity,
                        uint32_t timeout_ms, size_t *recv_len)
{
    const gpio_num_t pins[] = {tx, rx};
    if (!valid_pins(pins, 2) || !uart_timing_valid(baud, timeout_ms) ||
        (send_len > 0 && !send) || send_len > PROTOCOL_MAX_BYTES ||
        !recv || recv_capacity == 0 || recv_capacity > PROTOCOL_MAX_BYTES || !recv_len) {
        return ESP_ERR_INVALID_ARG;
    }

    *recv_len = 0;
    gpio_num_t active_tx = send_len > 0 ? tx : GPIO_NUM_NC;
    esp_err_t result = uart_open(active_tx, rx, baud);
    if (result != ESP_OK) return result;
    result = uart_send(send, send_len, baud);
    if (result == ESP_OK) result = uart_read_response(recv, recv_capacity, timeout_ms, recv_len);
    uart_close(active_tx, rx);
    return result;
}

static esp_err_t uart_read_burst(uint8_t *bytes, size_t *length, uint32_t baud, uint32_t timeout_ms)
{
    int received = uart_read_bytes(BENCH_UART, bytes, 1, ticks_for_ms(timeout_ms));
    if (received < 0) return ESP_FAIL;
    if (received == 0) return ESP_ERR_TIMEOUT;
    *length = 1;

    // Always leave time for a short idle gap, even at the highest baud rates.
    uint32_t gap_ms = (35000 + baud - 1) / baud;
    if (gap_ms < 12) gap_ms = 12;
    int64_t deadline = esp_timer_get_time() + 750000;
    while (esp_timer_get_time() < deadline) {
        uint8_t byte;
        received = uart_read_bytes(BENCH_UART, &byte, 1, ticks_for_ms(gap_ms));
        if (received < 0) return ESP_FAIL;
        if (received == 0) return ESP_OK;
        if (*length == PROTOCOL_MAX_BYTES) return ESP_ERR_INVALID_SIZE;
        bytes[(*length)++] = byte;
    }
    return ESP_ERR_TIMEOUT;
}

esp_err_t protocol_uart_proxy(gpio_num_t host_tx, gpio_num_t host_rx,
                              gpio_num_t device_tx, gpio_num_t device_rx,
                              uint32_t baud, uint32_t timeout_ms,
                              uint8_t *request, size_t *request_len,
                              uint8_t *response, size_t *response_len)
{
    const gpio_num_t pins[] = {host_tx, host_rx, device_tx, device_rx};
    if (!valid_pins(pins, 4) || !uart_timing_valid(baud, timeout_ms) ||
        !request || !request_len || !response || !response_len) {
        return ESP_ERR_INVALID_ARG;
    }
    *request_len = 0;
    *response_len = 0;

    esp_err_t result = uart_open(GPIO_NUM_NC, host_rx, baud);
    if (result != ESP_OK) return result;
    result = uart_read_burst(request, request_len, baud, timeout_ms);
    uart_close(GPIO_NUM_NC, host_rx);
    if (result != ESP_OK) return result;

    result = uart_open(device_tx, device_rx, baud);
    if (result != ESP_OK) return result;
    result = uart_send(request, *request_len, baud);
    if (result == ESP_OK) result = uart_read_burst(response, response_len, baud, timeout_ms);
    uart_close(device_tx, device_rx);
    if (result != ESP_OK) return result;

    result = uart_open(host_tx, GPIO_NUM_NC, baud);
    if (result != ESP_OK) return result;
    result = uart_send(response, *response_len, baud);
    uart_close(host_tx, GPIO_NUM_NC);
    return result;
}

static esp_err_t i2c_open(gpio_num_t sda, gpio_num_t scl, i2c_master_bus_handle_t *bus)
{
    i2c_master_bus_config_t config = {
        .i2c_port = -1,
        .sda_io_num = sda,
        .scl_io_num = scl,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    return i2c_new_master_bus(&config, bus);
}

static esp_err_t i2c_close(i2c_master_bus_handle_t bus, gpio_num_t sda, gpio_num_t scl,
                           esp_err_t result)
{
    esp_err_t cleanup = i2c_del_master_bus(bus);
    release_pin(sda);
    release_pin(scl);
    return result == ESP_OK ? cleanup : result;
}

esp_err_t protocol_i2c_scan(gpio_num_t sda, gpio_num_t scl,
                            uint8_t addresses[PROTOCOL_I2C_MAX_ADDRESSES], size_t *count)
{
    const gpio_num_t pins[] = {sda, scl};
    if (!valid_pins(pins, 2) || !addresses || !count) return ESP_ERR_INVALID_ARG;
    *count = 0;
    i2c_master_bus_handle_t bus;
    esp_err_t result = i2c_open(sda, scl, &bus);
    if (result != ESP_OK) return result;

    for (unsigned addr = I2C_FIRST_ADDRESS; addr <= I2C_LAST_ADDRESS; ++addr) {
        result = i2c_master_probe(bus, addr, 50);
        if (result == ESP_OK) {
            addresses[(*count)++] = addr;
        } else if (result != ESP_ERR_NOT_FOUND) {
            return i2c_close(bus, sda, scl, result);
        }
    }
    return i2c_close(bus, sda, scl, ESP_OK);
}

esp_err_t protocol_i2c_read(gpio_num_t sda, gpio_num_t scl, uint8_t address,
                            const uint8_t *write, size_t write_len,
                            uint8_t *recv, size_t recv_len)
{
    return protocol_i2c_read_at_rate(sda, scl, address, 100000, write, write_len, recv, recv_len);
}

esp_err_t protocol_i2c_read_at_rate(gpio_num_t sda, gpio_num_t scl, uint8_t address, uint32_t hz,
                                    const uint8_t *write, size_t write_len,
                                    uint8_t *recv, size_t recv_len)
{
    if (!recv_len || write_len > PROTOCOL_I2C_MAX_PREFIX) return ESP_ERR_INVALID_ARG;
    return protocol_i2c_transfer(sda, scl, address, hz, write, write_len, recv, recv_len);
}

esp_err_t protocol_i2c_transfer(gpio_num_t sda, gpio_num_t scl, uint8_t address, uint32_t hz,
                               const uint8_t *write, size_t write_len,
                               uint8_t *recv, size_t recv_len)
{
    const gpio_num_t pins[] = {sda, scl};
    if (!valid_pins(pins, 2) || address < I2C_FIRST_ADDRESS || address > I2C_LAST_ADDRESS ||
        hz < 5000 || hz > 400000 ||
        (write_len && !write) || write_len > PROTOCOL_MAX_BYTES ||
        (recv_len && !recv) || recv_len > PROTOCOL_MAX_BYTES || (!write_len && !recv_len)) {
        return ESP_ERR_INVALID_ARG;
    }
    i2c_master_bus_handle_t bus;
    esp_err_t result = i2c_open(sda, scl, &bus);
    if (result != ESP_OK) return result;
    const i2c_device_config_t config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = address,
        .scl_speed_hz = hz,
        .scl_wait_us = 10000,
    };
    i2c_master_dev_handle_t device;
    result = i2c_master_bus_add_device(bus, &config, &device);
    if (result == ESP_OK) {
        if (!write_len) result = i2c_master_receive(device, recv, recv_len, 120);
        else if (!recv_len) result = i2c_master_transmit(device, write, write_len, 120);
        else result = i2c_master_transmit_receive(device, write, write_len, recv, recv_len, 120);
        esp_err_t cleanup = i2c_master_bus_rm_device(device);
        if (cleanup == ESP_ERR_INVALID_STATE && i2c_master_bus_reset(bus) == ESP_OK) {
            cleanup = i2c_master_bus_rm_device(device);
        }
        if (result == ESP_OK) result = cleanup;
    }
    return i2c_close(bus, sda, scl, result);
}

esp_err_t protocol_spi(gpio_num_t sck, gpio_num_t mosi, gpio_num_t miso, gpio_num_t cs,
                       uint32_t hz, const uint8_t *send, uint8_t *recv, size_t len)
{
    const gpio_num_t pins[] = {sck, mosi, miso, cs};
    if (!valid_pins(pins, 4) || hz < 1000 || hz > 39000 || !send || !recv ||
        len == 0 || len > PROTOCOL_MAX_BYTES) {
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t half_period_us = (500000 + hz - 1) / hz;
    gpio_set_level(cs, 1);
    gpio_set_level(sck, 0);
    gpio_set_level(mosi, 0);
    gpio_set_direction(cs, GPIO_MODE_OUTPUT);
    gpio_set_direction(sck, GPIO_MODE_OUTPUT);
    gpio_set_direction(mosi, GPIO_MODE_OUTPUT);
    gpio_set_direction(miso, GPIO_MODE_INPUT);
    gpio_set_level(cs, 0);

    for (size_t i = 0; i < len; ++i) {
        uint8_t received = 0;
        for (int bit = 7; bit >= 0; --bit) {
            gpio_set_level(mosi, (send[i] >> bit) & 1);
            esp_rom_delay_us(half_period_us);
            gpio_set_level(sck, 1);
            esp_rom_delay_us(half_period_us);
            received = (received << 1) | gpio_get_level(miso);
            gpio_set_level(sck, 0);
        }
        recv[i] = received;
    }
    esp_rom_delay_us(half_period_us);
    gpio_set_level(cs, 1);
    esp_rom_delay_us(half_period_us);
    for (size_t i = 0; i < 4; ++i) release_pin(pins[i]);
    return ESP_OK;
}

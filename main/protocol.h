#pragma once

#include <stddef.h>
#include <stdint.h>
#include "driver/gpio.h"
#include "esp_err.h"

#define PROTOCOL_MAX_BYTES 64
#define PROTOCOL_I2C_MAX_ADDRESSES 112
#define PROTOCOL_I2C_MAX_PREFIX 16

// GPIO names are relative to the ESP32-C3. Callers must serialize use and
// ensure the selected routes are electrically compatible with each operation.
esp_err_t protocol_uart(gpio_num_t tx, gpio_num_t rx, uint32_t baud,
                        const uint8_t *send, size_t send_len,
                        uint8_t *recv, size_t recv_capacity,
                        uint32_t timeout_ms, size_t *recv_len);

// One request and reply, with a bounded first-byte timeout on each side.
// Both output buffers have room for PROTOCOL_MAX_BYTES bytes.
esp_err_t protocol_uart_proxy(gpio_num_t host_tx, gpio_num_t host_rx,
                              gpio_num_t device_tx, gpio_num_t device_rx,
                              uint32_t baud, uint32_t timeout_ms,
                              uint8_t *request, size_t *request_len,
                              uint8_t *response, size_t *response_len);

esp_err_t protocol_i2c_scan(gpio_num_t sda, gpio_num_t scl,
                            uint8_t addresses[PROTOCOL_I2C_MAX_ADDRESSES], size_t *count);
esp_err_t protocol_i2c_read(gpio_num_t sda, gpio_num_t scl, uint8_t address,
                            const uint8_t *write, size_t write_len,
                            uint8_t *recv, size_t recv_len);
esp_err_t protocol_i2c_read_at_rate(gpio_num_t sda, gpio_num_t scl, uint8_t address, uint32_t hz,
                                    const uint8_t *write, size_t write_len,
                                    uint8_t *recv, size_t recv_len);
esp_err_t protocol_i2c_transfer(gpio_num_t sda, gpio_num_t scl, uint8_t address, uint32_t hz,
                               const uint8_t *write, size_t write_len,
                               uint8_t *recv, size_t recv_len);

// SPI mode 0, active-low CS and MSB-first bytes.
esp_err_t protocol_spi(gpio_num_t sck, gpio_num_t mosi, gpio_num_t miso, gpio_num_t cs,
                       uint32_t hz, const uint8_t *send, uint8_t *recv, size_t len);

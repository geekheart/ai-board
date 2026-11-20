#pragma once

#include <stddef.h>
#include <stdint.h>
#include "driver/gpio.h"
#include "esp_err.h"

#define RECEIVE_FIXTURE_MAX_BYTES 32

typedef struct uart_receive_fixture uart_receive_fixture_t;

esp_err_t receive_fixture_uart_open(gpio_num_t tx, unsigned baud, const uint8_t *data, size_t size,
                                    uart_receive_fixture_t **fixture, unsigned *actual_baud);
esp_err_t receive_fixture_uart_send(uart_receive_fixture_t *fixture);
void receive_fixture_uart_close(uart_receive_fixture_t *fixture);

// The fixture sets up both GPIOs as released open-drain lines before the
// batch. Send one write and measure across actual SCL rising edges.
esp_err_t receive_fixture_i2c_write(gpio_num_t sda, gpio_num_t scl, uint8_t address, unsigned hz,
                                    const uint8_t *data, size_t size, unsigned *actual_hz);

// On the same open-drain bus, send one write, a repeated START and a read.
// `received` counts complete bytes even if a later byte or STOP fails.
// The measured rate includes SCL stretching.
esp_err_t receive_fixture_i2c_write_read(gpio_num_t sda, gpio_num_t scl, uint8_t address, unsigned hz,
                                         const uint8_t *write_data, size_t write_size,
                                         uint8_t *read_data, size_t read_size,
                                         size_t *received, unsigned *actual_hz);

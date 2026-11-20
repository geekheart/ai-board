#pragma once

#include "spi_stimulus.h"

#define SPI_I2S_HOST_MIN_HZ 1000000
#define SPI_I2S_HOST_MAX_HZ 20000000
#define SPI_I2S_HOST_BYTES 4

// Generate one Mode 0 frame with I2S0: WS low selects 32 SPI clocks, then
// returns high. For RX-only tests set miso to GPIO_NUM_NC and rx to NULL.
esp_err_t spi_i2s_host_transfer(gpio_num_t sck, gpio_num_t mosi, gpio_num_t miso,
                                gpio_num_t cs, unsigned hz,
                                const uint8_t tx[SPI_I2S_HOST_BYTES],
                                uint8_t rx[SPI_I2S_HOST_BYTES],
                                spi_stimulus_ready_fn ready, void *ready_arg,
                                unsigned *actual_hz);

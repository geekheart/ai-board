#pragma once

#include <stddef.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_err.h"

#define SPI_BENCHMARK_MIN_HZ 40000
#define SPI_BENCHMARK_MAX_HZ 1000000

// Hardware SPI2, mode 0 and MSB first. Caller owns exclusive use of SPI2 and GPIOs.
esp_err_t spi_benchmark_transfer(gpio_num_t sck, gpio_num_t mosi, gpio_num_t miso,
                                 gpio_num_t cs, unsigned hz, const uint8_t *tx,
                                 uint8_t *rx, size_t length);

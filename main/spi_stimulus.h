#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_err.h"

#define SPI_STIMULUS_MIN_HZ 10000
#define SPI_STIMULUS_MAX_HZ 3333334
#define SPI_STIMULUS_MIN_GAP_NS 150
#define SPI_STIMULUS_MAX_GAP_NS 1000000

// Called after the fixture pins and RMT outputs reach their idle levels.
// Return false to release the fixture without transmitting.
typedef bool (*spi_stimulus_ready_fn)(void *argument);

// Transmit up to four bytes in mode 0, MSB first, using synchronized RMT TX.
// The caller owns the matrix and GPIOs. MISO is observed by the sampler.
esp_err_t spi_stimulus_transfer(gpio_num_t sck, gpio_num_t mosi, gpio_num_t cs,
                                unsigned hz, const uint8_t *tx, size_t length);

esp_err_t spi_stimulus_transfer_bench(gpio_num_t sck, gpio_num_t mosi, gpio_num_t cs,
                                      unsigned hz, const uint8_t *tx, size_t length,
                                      spi_stimulus_ready_fn ready, void *ready_arg,
                                      unsigned *actual_hz);

// Two mode-0 32-clock frames with constant MOSI. Both SCK and CS come from
// synchronized hardware. Optional outputs report the programmed timings.
esp_err_t spi_stimulus_transfer_pair(gpio_num_t sck, gpio_num_t mosi, gpio_num_t cs,
                                     unsigned hz, uint32_t gap_ns, bool mosi_high,
                                     unsigned *actual_hz, uint32_t *actual_gap_ns,
                                     spi_stimulus_ready_fn ready, void *ready_arg);

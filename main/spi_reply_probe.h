#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_err.h"

#define SPI_REPLY_PROBE_BYTES 4

// Arm before generating mode-0, MSB-first transfers of exactly four bytes.
// Raw clock mode groups each 32 clock edges without needing a clock during CS high.
esp_err_t spi_reply_probe_begin(gpio_num_t sck, gpio_num_t cs, gpio_num_t miso, bool raw_clock);
esp_err_t spi_reply_probe_read(uint8_t reply[SPI_REPLY_PROBE_BYTES], uint32_t timeout_ms);
esp_err_t spi_reply_probe_end(void);

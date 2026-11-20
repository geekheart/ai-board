#pragma once

#include "capture.h"

#define CAPTURE_DMA_MIN_HZ 40000U
#define CAPTURE_DMA_MAX_HZ 20000000U
#define CAPTURE_DMA_MAX_BYTES 32768U

typedef struct {
    // First sample in the high nibble, channel zero in bit zero of a nibble.
    // Caller owns packed and must free it.
    uint8_t *packed;
    uint32_t sample_count;
    uint32_t sample_hz;
    // From GPIO interrupt entry to the prepared SPI launch, at 1 us resolution.
    uint32_t trigger_delay_ns;
} capture_dma_result_t;

// Four passive SPI2 input lanes share the internal SPI clock; no clock or data
// output is attached to board pins. A shorter window is required at higher rates.
esp_err_t capture_dma_run(const gpio_num_t *pins, size_t count, uint32_t requested_hz,
                          uint32_t window_us, capture_trigger_t mode, size_t trigger_index,
                          uint32_t wait_ms, void (*stimulus)(void *), void *arg,
                          capture_dma_result_t *out);

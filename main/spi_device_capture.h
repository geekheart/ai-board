#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_err.h"

#define SPI_DEVICE_CAPTURE_MAX_FRAMES 8
#define SPI_DEVICE_CAPTURE_MAX_BYTES 256
#define SPI_DEVICE_CAPTURE_MAX_TIMEOUT_MS 10000

typedef struct {
    gpio_num_t sck;
    gpio_num_t mosi;
    gpio_num_t miso;             // GPIO_NUM_NC leaves the MISO output unbound.
    gpio_num_t cs;               // Active low.
    uint8_t mode;
    bool lsb_first;
    uint8_t frames;
    uint16_t capacity;           // Maximum bytes retained per frame.
    uint32_t timeout_ms;         // Shared deadline across all requested frames.
    const uint8_t *reply;        // Fixed prefix on every frame, padded with zero bytes.
    size_t reply_len;
    void (*armed)(void *);       // Optional, called in task context after arming; return promptly.
    void *armed_context;
} spi_device_capture_options_t;

typedef struct {
    size_t bits;                 // Length reported by the peripheral.
    size_t bytes;
    bool truncated;             // Length exceeded the requested capacity.
    bool length_saturated;       // Reported bit count reached the hardware buffer limit.
    uint8_t data[SPI_DEVICE_CAPTURE_MAX_BYTES];
} spi_device_capture_frame_t;

typedef struct {
    size_t frame_count;
    bool timed_out;
    spi_device_capture_frame_t frame[SPI_DEVICE_CAPTURE_MAX_FRAMES];
} spi_device_capture_result_t;

// Caller owns SPI2 and the selected GPIOs throughout. A timeout returns ESP_OK
// with timed_out set and any completed frames retained. Start the host after arming.
// ESP-IDF's DMA slave specification calls for host frames in multiples of four bytes.
esp_err_t spi_device_capture_run(const spi_device_capture_options_t *options,
                                 spi_device_capture_result_t *result);

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_err.h"

#define I2C_DEVICE_CAPTURE_MAX_FRAMES 8
#define I2C_DEVICE_CAPTURE_MAX_BYTES 64
#define I2C_DEVICE_CAPTURE_MAX_REGISTERS 8
#define I2C_DEVICE_CAPTURE_MAX_REPLY 32
#define I2C_DEVICE_CAPTURE_MAX_TIMEOUT_MS 10000

typedef struct {
    uint8_t address;
    uint8_t reply_len;
    uint8_t reply[I2C_DEVICE_CAPTURE_MAX_REPLY];
} i2c_device_register_t;

typedef struct {
    gpio_num_t sda;
    gpio_num_t scl;
    uint8_t address;
    uint8_t frames;
    uint16_t capacity;
    uint32_t timeout_ms;
    bool internal_pullup;
    uint8_t register_address;
    const uint8_t *reply;      // A nonempty reply selects one register transaction.
    size_t reply_len;
    const i2c_device_register_t *registers;
    size_t register_count;     // Writable values initialized for this batch, with `frames` read exchanges.
    bool auto_increment;       // Split bursts across consecutive registers by configured widths.
    void (*armed)(void *);       // Optional notification after the slave is ready.
    void *armed_context;
} i2c_device_capture_options_t;

typedef struct {
    size_t bytes;
    size_t total_bytes;          // Bytes delivered by the hardware driver.
    bool truncated;
    uint8_t data[I2C_DEVICE_CAPTURE_MAX_BYTES];
} i2c_device_capture_frame_t;

typedef struct {
    size_t frame_index;        // SIZE_MAX for a read without a pending write.
    bool matched;
    bool write_applied;
    size_t reply_loaded;       // Value bytes loaded, independent of bytes clocked by the host.
    bool read_completed;       // A subsequent address phase or bus idle ended this read.
    uint8_t reply[I2C_DEVICE_CAPTURE_MAX_REPLY];
    uint8_t span_count;        // Auto-increment registers starting at the first captured write byte.
    uint8_t span_lengths[I2C_DEVICE_CAPTURE_MAX_REGISTERS];
} i2c_device_capture_transaction_t;

typedef struct {
    size_t frame_count;
    bool timed_out;
    size_t read_requests;
    size_t reply_loaded;       // Total value bytes loaded into I2C0 TX for recorded reads.
    bool read_completed;       // The last recorded read phase ended.
    i2c_device_capture_frame_t frame[I2C_DEVICE_CAPTURE_MAX_FRAMES];
    size_t transaction_count;
    i2c_device_capture_transaction_t transaction[I2C_DEVICE_CAPTURE_MAX_FRAMES];
} i2c_device_capture_result_t;

// Caller owns I2C0 and both pins. Use the slave's seven-bit address and one
// data-bearing write per phase. In table mode the first byte selects a
// register, and 1–32 following bytes replace its value for the batch.
// Auto-increment mode divides the value among successive register addresses
// and accepts a write only when it covers whole values of configured widths.
// Each read uses the preceding write phase. Timeout returns partial captures.
esp_err_t i2c_device_capture_run(const i2c_device_capture_options_t *options,
                                 i2c_device_capture_result_t *result);

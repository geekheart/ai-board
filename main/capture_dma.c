#include "capture_dma.h"

#include <stdlib.h>

#include "driver/spi_master.h"
#include "esp_clk_tree.h"
#include "esp_rom_gpio.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hal/spi_hal.h"
#include "soc/gpio_pins.h"
#include "soc/gpio_sig_map.h"

static const uint32_t input_signals[] = {FSPID_IN_IDX, FSPIQ_IN_IDX, FSPIWP_IN_IDX, FSPIHD_IN_IDX};

typedef struct {
    gpio_num_t pin;
    capture_trigger_t mode;
    uint32_t wait_ms;
    SemaphoreHandle_t fired;
    int64_t interrupt_us;
    uint32_t delay_ns;
    esp_err_t result;
    void (*stimulus)(void *);
    void *stimulus_arg;
} dma_trigger_t;

static void trigger_edge(void *arg)
{
    dma_trigger_t *trigger = arg;
    trigger->interrupt_us = esp_timer_get_time();
    gpio_intr_disable(trigger->pin);
    BaseType_t wake = pdFALSE;
    xSemaphoreGiveFromISR(trigger->fired, &wake);
    if (wake) portYIELD_FROM_ISR();
}

// This callback is only used with polling_start in task context. The driver
// has configured DMA already and starts the SPI command after we return.
static void wait_for_edge(spi_transaction_t *transaction)
{
    dma_trigger_t *trigger = transaction->user;
    gpio_int_type_t edge = trigger->mode == CAPTURE_RISING ? GPIO_INTR_POSEDGE : GPIO_INTR_NEGEDGE;
    trigger->result = gpio_set_intr_type(trigger->pin, edge);
    if (trigger->result == ESP_OK) trigger->result = gpio_intr_enable(trigger->pin);
    if (trigger->result != ESP_OK) return;

    if (trigger->stimulus) trigger->stimulus(trigger->stimulus_arg);
    if (xSemaphoreTake(trigger->fired, pdMS_TO_TICKS(trigger->wait_ms) + 1) == pdTRUE) {
        trigger->delay_ns = (esp_timer_get_time() - trigger->interrupt_us) * 1000;
    } else {
        trigger->result = ESP_ERR_TIMEOUT;
    }
    gpio_intr_disable(trigger->pin);
}

static esp_err_t register_edge(dma_trigger_t *trigger)
{
    esp_err_t result = gpio_install_isr_service(0);
    if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) return result;

    trigger->fired = xSemaphoreCreateBinary();
    if (!trigger->fired) return ESP_ERR_NO_MEM;
    result = gpio_set_intr_type(trigger->pin, GPIO_INTR_DISABLE);
    if (result == ESP_OK) result = gpio_isr_handler_add(trigger->pin, trigger_edge, trigger);
    if (result == ESP_OK) {
        gpio_intr_disable(trigger->pin);
    } else {
        vSemaphoreDelete(trigger->fired);
        trigger->fired = NULL;
    }
    return result;
}

static bool valid_pins(const gpio_num_t *pins, size_t count)
{
    if (!pins || !count || count > CAPTURE_MAX_CHANNELS) return false;
    uint64_t used = 0;
    for (size_t channel = 0; channel < count; ++channel) {
        gpio_num_t pin = pins[channel];
        if (!GPIO_IS_VALID_GPIO(pin)) return false;
        uint64_t bit = UINT64_C(1) << pin;
        if (used & bit) return false;
        used |= bit;
    }
    return true;
}

esp_err_t capture_dma_run(const gpio_num_t *pins, size_t count, uint32_t requested_hz,
                          uint32_t window_us, capture_trigger_t mode, size_t trigger_index,
                          uint32_t wait_ms, void (*stimulus)(void *), void *arg,
                          capture_dma_result_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    *out = (capture_dma_result_t){0};
    const bool edge = mode == CAPTURE_RISING || mode == CAPTURE_FALLING;
    if (!valid_pins(pins, count) || requested_hz < CAPTURE_DMA_MIN_HZ ||
        requested_hz > CAPTURE_DMA_MAX_HZ || !window_us ||
        window_us > CAPTURE_MAX_DURATION_MS * 1000 ||
        (mode != CAPTURE_MANUAL && !edge) ||
        (edge && (trigger_index >= count || wait_ms < CAPTURE_MIN_WAIT_MS || wait_ms > CAPTURE_MAX_WAIT_MS))) {
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t xtal_hz;
    esp_err_t result = esp_clk_tree_src_get_freq_hz(SPI_CLK_SRC_XTAL,
                                                   ESP_CLK_TREE_SRC_FREQ_PRECISION_CACHED, &xtal_hz);
    if (result != ESP_OK) return result;
    const uint32_t sample_hz = spi_hal_master_cal_clock(xtal_hz, requested_hz, 128);
    if (!sample_hz) return ESP_ERR_INVALID_ARG;
    // A 32-bit aligned DMA length accommodates eight packed samples.
    const uint64_t requested_samples = ((uint64_t)sample_hz * window_us + 999999) / 1000000;
    if (requested_samples > CAPTURE_DMA_MAX_BYTES * 2) return ESP_ERR_INVALID_SIZE;
    const uint32_t sample_count = (requested_samples + 7) & ~UINT32_C(7);
    const size_t size = sample_count / 2;

    const spi_bus_config_t bus = {
        .data0_io_num = -1,
        .data1_io_num = -1,
        .data2_io_num = -1,
        .data3_io_num = -1,
        .data4_io_num = -1,
        .data5_io_num = -1,
        .data6_io_num = -1,
        .data7_io_num = -1,
        .sclk_io_num = -1,
        .max_transfer_sz = size,
        .flags = SPICOMMON_BUSFLAG_GPIO_PINS,
    };
    result = spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO);
    if (result != ESP_OK) return result;

    const spi_device_interface_config_t config = {
        .clock_source = SPI_CLK_SRC_XTAL,
        .clock_speed_hz = requested_hz,
        .spics_io_num = -1,
        .queue_size = 1,
        .flags = SPI_DEVICE_HALFDUPLEX | SPI_DEVICE_NO_DUMMY,
        .pre_cb = edge ? wait_for_edge : NULL,
    };
    spi_device_handle_t device = NULL;
    uint8_t *packed = NULL;
    bool routed = false;
    dma_trigger_t trigger = {
        .pin = edge ? pins[trigger_index] : GPIO_NUM_NC,
        .mode = mode,
        .wait_ms = wait_ms,
        .stimulus = stimulus,
        .stimulus_arg = arg,
    };

    result = spi_bus_add_device(SPI2_HOST, &config, &device);
    if (result != ESP_OK) goto cleanup;

    packed = spi_bus_dma_memory_alloc(SPI2_HOST, size, 0);
    if (!packed) {
        result = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    for (size_t channel = 0; channel < CAPTURE_MAX_CHANNELS; channel++) {
        uint32_t pin = channel < count ? (uint32_t)pins[channel] : GPIO_MATRIX_CONST_ZERO_INPUT;
        if (channel < count) {
            result = gpio_set_direction(pin, GPIO_MODE_INPUT);
            if (result != ESP_OK) goto cleanup;
        }
        esp_rom_gpio_connect_in_signal(pin, input_signals[channel], false);
        routed = true;
    }
    if (edge) {
        result = register_edge(&trigger);
        if (result != ESP_OK) goto cleanup;
    }

    spi_transaction_t transaction = {
        .flags = SPI_TRANS_MODE_QIO,
        .rxlength = size * 8,
        .rx_buffer = packed,
        .user = &trigger,
    };
    result = spi_device_polling_start(device, &transaction, portMAX_DELAY);
    if (result != ESP_OK) goto cleanup;
    if (!edge && stimulus) stimulus(arg);
    // DMA free-runs while the task sleeps; don't hold the CPU in polling_end
    // for a long window on the C3's single core.
    const uint32_t elapsed_ms = ((uint64_t)sample_count * 1000 + sample_hz - 1) / sample_hz;
    vTaskDelay(pdMS_TO_TICKS(elapsed_ms) + 1);
    result = spi_device_polling_end(device, portMAX_DELAY);
    if (result == ESP_OK && edge) result = trigger.result;

cleanup:
    if (trigger.fired) {
        gpio_isr_handler_remove(trigger.pin);
        gpio_set_intr_type(trigger.pin, GPIO_INTR_DISABLE);
        vSemaphoreDelete(trigger.fired);
    }
    if (routed) {
        for (size_t channel = 0; channel < CAPTURE_MAX_CHANNELS; channel++) {
            esp_rom_gpio_connect_in_signal(GPIO_MATRIX_CONST_ZERO_INPUT, input_signals[channel], false);
        }
    }
    if (device) {
        const esp_err_t removed = spi_bus_remove_device(device);
        if (result == ESP_OK) result = removed;
    }
    const esp_err_t freed = spi_bus_free(SPI2_HOST);
    if (result == ESP_OK) result = freed;
    if (result == ESP_OK) {
        out->packed = packed;
        out->sample_count = sample_count;
        out->sample_hz = sample_hz;
        out->trigger_delay_ns = trigger.delay_ns;
    } else {
        free(packed);
    }
    return result;
}

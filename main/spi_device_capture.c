#include "spi_device_capture.h"

#include <stdlib.h>
#include <string.h>

#include "driver/spi_slave.h"
#include "esp_heap_caps.h"
#include "esp_private/spi_common_internal.h"
#include "esp_private/spi_slave_internal.h"
#include "esp_rom_gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hal/spi_ll.h"
#include "soc/gpio_sig_map.h"

static void first_frame_ready(spi_slave_transaction_t *transaction)
{
    if (!transaction->user) return;
    BaseType_t wake = pdFALSE;
    xSemaphoreGiveFromISR(transaction->user, &wake);
    if (wake) portYIELD_FROM_ISR();
}

static bool options_valid(const spi_device_capture_options_t *options)
{
    return options && options->mode <= 3 &&
           options->frames && options->frames <= SPI_DEVICE_CAPTURE_MAX_FRAMES &&
           options->capacity && options->capacity <= SPI_DEVICE_CAPTURE_MAX_BYTES &&
           options->timeout_ms && options->timeout_ms <= SPI_DEVICE_CAPTURE_MAX_TIMEOUT_MS &&
           GPIO_IS_VALID_GPIO(options->sck) && GPIO_IS_VALID_GPIO(options->mosi) &&
           GPIO_IS_VALID_GPIO(options->cs) && options->sck != options->mosi &&
           options->sck != options->cs && options->mosi != options->cs &&
           options->reply_len <= options->capacity && (!options->reply_len || options->reply) &&
           (options->miso == GPIO_NUM_NC ? !options->reply_len :
            (GPIO_IS_VALID_OUTPUT_GPIO(options->miso) && options->miso != options->sck &&
             options->miso != options->mosi && options->miso != options->cs));
}

static TickType_t remaining_ticks(TickType_t started, TickType_t timeout)
{
    const TickType_t elapsed = xTaskGetTickCount() - started;
    return elapsed < timeout ? timeout - elapsed : 0;
}

static esp_err_t release_spi(void)
{
    portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
    portENTER_CRITICAL(&lock);
    // End the current selection and reset the shifter before resetting the
    // queue. The forced CS edge is outside the returned capture window.
    esp_rom_gpio_connect_in_signal(GPIO_MATRIX_CONST_ONE_INPUT, FSPICS0_IN_IDX, false);
    spi_ll_slave_reset(SPI_LL_GET_HW(SPI2_HOST));
    const esp_err_t queue_result = spi_slave_queue_reset(SPI2_HOST);
    portEXIT_CRITICAL(&lock);

    // Queue reset alone doesn't disarm the active hardware DMA descriptor.
    // This is the IDF 6.1 ESP32-C3 GDMA path. Stop both channels while the
    // descriptors and all application buffers are still live.
    spi_dma_ctx_t *dma = spi_bus_get_dma_ctx(SPI2_HOST);
    const esp_err_t rx_result = gdma_reset(dma->rx_dma_chan);
    const esp_err_t tx_result = gdma_reset(dma->tx_dma_chan);
    const esp_err_t free_result = spi_slave_free(SPI2_HOST);
    if (queue_result != ESP_OK) return queue_result;
    if (rx_result != ESP_OK) return rx_result;
    if (tx_result != ESP_OK) return tx_result;
    return free_result;
}

esp_err_t spi_device_capture_run(const spi_device_capture_options_t *options,
                                 spi_device_capture_result_t *result)
{
    if (!result) return ESP_ERR_INVALID_ARG;
    memset(result, 0, sizeof(*result));
    if (!options_valid(options)) return ESP_ERR_INVALID_ARG;

    // DMA uses word-aligned buffers and hardware caps trans_len at its own
    // limit. Leave guard space to detect overflow of the requested capacity.
    const size_t dma_bytes = (options->capacity + (size_t)4) & ~(size_t)3;
    const size_t dma_bits = dma_bytes * 8;
    const bool duplex = options->miso != GPIO_NUM_NC;
    const spi_bus_config_t bus = {
        .mosi_io_num = GPIO_NUM_NC,
        .miso_io_num = options->miso,
        .sclk_io_num = GPIO_NUM_NC,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .data4_io_num = GPIO_NUM_NC,
        .data5_io_num = GPIO_NUM_NC,
        .data6_io_num = GPIO_NUM_NC,
        .data7_io_num = GPIO_NUM_NC,
        .max_transfer_sz = dma_bytes,
        .flags = SPICOMMON_BUSFLAG_GPIO_PINS,
    };
    const spi_slave_interface_config_t device = {
        .spics_io_num = GPIO_NUM_NC,
        .flags = options->lsb_first ? SPI_SLAVE_BIT_LSBFIRST : 0,
        .queue_size = options->frames,
        .mode = options->mode,
        .post_setup_cb = first_frame_ready,
    };

    // The common driver also attaches output functions to MOSI and CS.
    // Route these as input signals only, even when a reply is configured.
    esp_err_t err = spi_slave_initialize(SPI2_HOST, &bus, &device, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) return err;
    esp_rom_gpio_connect_in_signal(GPIO_MATRIX_CONST_ONE_INPUT, FSPICS0_IN_IDX, false);

    spi_slave_transaction_t transactions[SPI_DEVICE_CAPTURE_MAX_FRAMES] = {0};
    SemaphoreHandle_t ready = xSemaphoreCreateBinary();
    uint8_t *rx = spi_bus_dma_memory_alloc(SPI2_HOST, dma_bytes * options->frames, MALLOC_CAP_INTERNAL);
    uint8_t *tx = duplex ? spi_bus_dma_memory_alloc(SPI2_HOST, dma_bytes, MALLOC_CAP_INTERNAL) : NULL;
    if (!ready || !rx || (duplex && !tx)) {
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    if (options->reply_len) memcpy(tx, options->reply, options->reply_len);

    err = gpio_set_direction(options->sck, GPIO_MODE_INPUT);
    if (err == ESP_OK) err = gpio_set_direction(options->mosi, GPIO_MODE_INPUT);
    if (err == ESP_OK) err = gpio_set_direction(options->cs, GPIO_MODE_INPUT);
    if (err != ESP_OK) goto cleanup;
    esp_rom_gpio_connect_in_signal(options->sck, FSPICLK_IN_IDX, false);
    esp_rom_gpio_connect_in_signal(options->mosi, FSPID_IN_IDX, false);

    for (size_t i = 0; i < options->frames; ++i) {
        transactions[i].length = dma_bits;
        transactions[i].rx_buffer = rx + i * dma_bytes;
        transactions[i].tx_buffer = tx;
        transactions[i].user = i == 0 ? ready : NULL;
        err = spi_slave_queue_trans(SPI2_HOST, &transactions[i], 0);
        if (err != ESP_OK) goto cleanup;
    }

    const TickType_t timeout = pdMS_TO_TICKS(options->timeout_ms) + 1;
    const TickType_t started = xTaskGetTickCount();
    if (xSemaphoreTake(ready, remaining_ticks(started, timeout)) != pdTRUE) {
        result->timed_out = true;
        goto cleanup;
    }
    esp_rom_gpio_connect_in_signal(options->cs, FSPICS0_IN_IDX, false);
    if (options->armed) options->armed(options->armed_context);

    while (result->frame_count < options->frames) {
        spi_slave_transaction_t *done;
        err = spi_slave_get_trans_result(SPI2_HOST, &done, remaining_ticks(started, timeout));
        if (err == ESP_ERR_TIMEOUT) {
            result->timed_out = true;
            err = ESP_OK;
            break;
        }
        if (err != ESP_OK) break;

        spi_device_capture_frame_t *frame = &result->frame[result->frame_count++];
        frame->bits = done->trans_len;
        frame->bytes = (frame->bits + 7) / 8;
        frame->length_saturated = frame->bits >= dma_bits;
        frame->truncated = frame->bytes > options->capacity;
        if (frame->truncated) frame->bytes = options->capacity;
        memcpy(frame->data, done->rx_buffer, frame->bytes);
    }

cleanup:
   {
        const esp_err_t released = release_spi();
        if (err == ESP_OK) err = released;
    }
    if (ready) vSemaphoreDelete(ready);
    free(rx);
    free(tx);
    return err;
}

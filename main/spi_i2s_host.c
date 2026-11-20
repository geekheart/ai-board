#include "spi_i2s_host.h"

#include <stdbool.h>
#include <sys/queue.h>

#include "driver/i2s_std.h"
#include "esp_attr.h"
#include "esp_rom_gpio.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "i2s_private.h"
#include "soc/gpio_pins.h"
#include "soc/gpio_sig_map.h"

enum {
    SLOT_BITS = 32,
    SLOTS_PER_FRAME = 2,
    DMA_FRAMES = 1,
    DMA_DESCRIPTORS = 2,
    BCLK_DIV = I2S_MCLK_MULTIPLE_256 / (SLOTS_PER_FRAME * SLOT_BITS),
    RX_TIMEOUT_MS = 100,
};

typedef struct {
    SemaphoreHandle_t done;
    uint32_t word;
    bool captured;
} reply_latch_t;

static bool IRAM_ATTR latch_reply(i2s_chan_handle_t handle, i2s_event_data_t *event, void *context)
{
    (void)handle;
    reply_latch_t *latch = context;
    if (latch->captured || event->size < sizeof(uint32_t)) return false;
    const uint32_t *sample = event->dma_buf;
    latch->word = *sample;
    latch->captured = true;
    BaseType_t wake = pdFALSE;
    xSemaphoreGiveFromISR(latch->done, &wake);
    return wake == pdTRUE;
}

static void release_pin(gpio_num_t pin)
{
    gpio_reset_pin(pin);
    gpio_set_direction(pin, GPIO_MODE_INPUT);
    gpio_set_pull_mode(pin, GPIO_FLOATING);
}

static void close_channel(i2s_chan_handle_t channel, bool enabled, esp_err_t *result)
{
    if (!channel) return;
    if (enabled) {
        const esp_err_t status = i2s_channel_disable(channel);
        if (*result == ESP_OK) *result = status;
    }
    const esp_err_t status = i2s_del_channel(channel);
    if (*result == ESP_OK) *result = status;
}

esp_err_t spi_i2s_host_transfer(gpio_num_t sck, gpio_num_t mosi, gpio_num_t miso,
                                gpio_num_t cs, unsigned hz,
                                const uint8_t tx[SPI_I2S_HOST_BYTES],
                                uint8_t rx[SPI_I2S_HOST_BYTES],
                                spi_stimulus_ready_fn ready, void *ready_arg,
                                unsigned *actual_hz)
{
    if (!tx || hz < SPI_I2S_HOST_MIN_HZ || hz > SPI_I2S_HOST_MAX_HZ ||
        !GPIO_IS_VALID_OUTPUT_GPIO(sck) || !GPIO_IS_VALID_OUTPUT_GPIO(mosi) ||
        !GPIO_IS_VALID_OUTPUT_GPIO(cs) || sck == mosi || sck == cs || mosi == cs ||
        (rx && (!GPIO_IS_VALID_GPIO(miso) || miso == sck || miso == mosi || miso == cs)) ||
        (!rx && miso != GPIO_NUM_NC)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (actual_hz) *actual_hz = 0;

    // WS idles high before I2S starts. Use its low (left) slot as CS active.
    // Keep CS on the GPIO output until the SPI receiver has armed.
    gpio_set_level(cs, 1);
    esp_err_t result = gpio_set_direction(cs, GPIO_MODE_OUTPUT);
    if (result != ESP_OK) return result;
    i2s_chan_config_t channel = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    channel.dma_desc_num = DMA_DESCRIPTORS;
    channel.dma_frame_num = DMA_FRAMES;
    const i2s_std_config_t config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(hz / (SLOTS_PER_FRAME * SLOT_BITS)),
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = sck,
            .ws = cs,
            .dout = mosi,
            .din = miso,
        },
    };

    i2s_chan_handle_t transmitter = NULL, receiver = NULL;
    bool tx_enabled = false, rx_enabled = false;
    reply_latch_t latch = {0};
    result = i2s_new_channel(&channel, &transmitter, rx ? &receiver : NULL);
    if (result != ESP_OK) goto done;
    result = i2s_channel_init_std_mode(transmitter, &config);
    if (result != ESP_OK) goto done;
    if (receiver) {
        i2s_std_config_t receive_config = config;
        receive_config.slot_cfg.slot_mode = I2S_SLOT_MODE_MONO;
        receive_config.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
        result = i2s_channel_init_std_mode(receiver, &receive_config);
        if (result != ESP_OK) goto done;
        latch.done = xSemaphoreCreateBinary();
        if (!latch.done) {
            result = ESP_ERR_NO_MEM;
            goto done;
        }
        const i2s_event_callbacks_t callbacks = {.on_recv = latch_reply};
        result = i2s_channel_register_event_callback(receiver, &callbacks, &latch);
        if (result != ESP_OK) goto done;
    }
    esp_rom_gpio_connect_out_signal(cs, SIG_GPIO_OUT_IDX, false, false);

    uint32_t payload = 0;
    for (size_t i = 0; i < SPI_I2S_HOST_BYTES; ++i) {
        payload = (payload << 8) | tx[i];
    }
    // A single-word TX can leave the serializer at its idle zero on the
    // first edge. Choose its idle pad level to match the requested MSB.
    const bool invert_data = (payload >> 31) != 0;
    if (invert_data) payload = ~payload;
    esp_rom_gpio_connect_out_signal(mosi, I2SO_SD_OUT_IDX, invert_data, false);
    size_t loaded = 0;
    result = i2s_channel_preload_data(transmitter, &payload, sizeof(payload), &loaded);
    if (result != ESP_OK) goto done;
    if (loaded != sizeof(payload)) {
        result = ESP_ERR_INVALID_SIZE;
        goto done;
    }
    // The IDF 6.1 driver starts from descriptor zero. Clock only the left
    // word so FIFO stop returns WS directly to its deselected high level.
    // Clocking the right word can briefly select the next left slot again.
    lldesc_t *first = transmitter->dma.desc[0];
    first->length = sizeof(payload);
    STAILQ_NEXT(first, qe) = NULL;
    transmitter->controller->hal.dev->tx_conf.tx_stop_en = 1;

    i2s_chan_info_t info;
    result = i2s_channel_get_info(transmitter, &info);
    if (result != ESP_OK) goto done;
    const unsigned measured_clock = info.mclk_hz / BCLK_DIV;
    if (receiver) {
        result = i2s_channel_enable(receiver);
        if (result != ESP_OK) goto done;
        rx_enabled = true;
    }

    if (ready && !ready(ready_arg)) {
        result = ESP_ERR_INVALID_STATE;
        goto done;
    }

    esp_rom_gpio_connect_out_signal(cs, I2SO_WS_OUT_IDX, false, false);
    result = i2s_channel_enable(transmitter);
    if (result != ESP_OK) goto done;
    tx_enabled = true;
    if (actual_hz) *actual_hz = measured_clock;
    if (receiver) {
        const TickType_t timeout = pdMS_TO_TICKS(RX_TIMEOUT_MS);
        if (xSemaphoreTake(latch.done, timeout) != pdTRUE) {
            result = ESP_ERR_TIMEOUT;
            goto done;
        }
        for (size_t i = 0; i < SPI_I2S_HOST_BYTES; ++i) {
            rx[i] = latch.word >> (8 * (SPI_I2S_HOST_BYTES - 1 - i));
        }
    }
    // Allow the I2S clock to stop and WS to return high before restoring GPIO CS.
    esp_rom_delay_us((SLOTS_PER_FRAME * SLOT_BITS * 1000000 + measured_clock - 1) / measured_clock + 5);

done:
    esp_rom_gpio_connect_out_signal(cs, SIG_GPIO_OUT_IDX, false, false);
    close_channel(transmitter, tx_enabled, &result);
    close_channel(receiver, rx_enabled, &result);
    if (latch.done) vSemaphoreDelete(latch.done);
    if (receiver) {
        esp_rom_gpio_connect_in_signal(GPIO_MATRIX_CONST_ZERO_INPUT, I2SI_BCK_IN_IDX, false);
        esp_rom_gpio_connect_in_signal(GPIO_MATRIX_CONST_ZERO_INPUT, I2SI_WS_IN_IDX, false);
        esp_rom_gpio_connect_in_signal(GPIO_MATRIX_CONST_ZERO_INPUT, I2SI_SD_IN_IDX, false);
    }
    release_pin(mosi);
    release_pin(sck);
    release_pin(cs);
    return result;
}

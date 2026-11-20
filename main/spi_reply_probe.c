#include "spi_reply_probe.h"

#include "driver/i2s_pdm.h"
#include "driver/i2s_std.h"
#include "esp_rom_gpio.h"
#include "soc/gpio_pins.h"
#include "soc/gpio_sig_map.h"

static i2s_chan_handle_t rx;
static bool enabled;
static bool raw_clock_mode;

enum { PDM_HALFWORDS_PER_FRAME = 4 };

esp_err_t spi_reply_probe_begin(gpio_num_t sck, gpio_num_t cs, gpio_num_t miso, bool raw_clock)
{
    if (!GPIO_IS_VALID_GPIO(sck) || !GPIO_IS_VALID_GPIO(cs) || !GPIO_IS_VALID_GPIO(miso) ||
        sck == cs || sck == miso || cs == miso) {
        return ESP_ERR_INVALID_ARG;
    }
    if (rx) return ESP_ERR_INVALID_STATE;

    i2s_chan_config_t channel = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_SLAVE);
    channel.dma_frame_num = raw_clock ? PDM_HALFWORDS_PER_FRAME : 1;
    channel.dma_desc_num = raw_clock ? 6 : 4;
    esp_err_t result = i2s_new_channel(&channel, NULL, &rx);
    if (result != ESP_OK) return result;

    if (raw_clock) {
        // Raw PDM packs successive clock samples without a WS framing dependency.
        const i2s_pdm_rx_config_t config = {
            .clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(2500000),
            .slot_cfg = I2S_PDM_RX_SLOT_RAW_FMT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
            .gpio_cfg = {
                .clk = sck,
                .din = miso,
                .invert_flags.clk_inv = false,
            },
        };
        result = i2s_channel_init_pdm_rx_mode(rx, &config);
    } else {
        // CS low selects the left slot. Sample MISO on SCK with no I2S one-bit shift.
        i2s_std_config_t config = {
            .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(150000),
            .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
            .gpio_cfg = {
                .mclk = I2S_GPIO_UNUSED,
                .bclk = sck,
                .ws = cs,
                .dout = I2S_GPIO_UNUSED,
                .din = miso,
            },
        };
        config.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
        result = i2s_channel_init_std_mode(rx, &config);
    }
    if (result == ESP_OK) result = i2s_channel_enable(rx);
    if (result == ESP_OK) {
        enabled = true;
        raw_clock_mode = raw_clock;
    } else {
        spi_reply_probe_end();
    }
    return result;
}

esp_err_t spi_reply_probe_read(uint8_t reply[SPI_REPLY_PROBE_BYTES], uint32_t timeout_ms)
{
    if (!reply) return ESP_ERR_INVALID_ARG;
    if (!enabled) return ESP_ERR_INVALID_STATE;

    union {
        uint32_t word;
        uint16_t halfwords[PDM_HALFWORDS_PER_FRAME];
    } frame;
    const size_t frame_size = raw_clock_mode ? sizeof(frame.halfwords) : sizeof(frame.word);
    size_t bytes_read = 0;
    esp_err_t result = i2s_channel_read(rx, &frame, frame_size, &bytes_read, timeout_ms);
    if (result != ESP_OK) return result;
    if (bytes_read != frame_size) return ESP_ERR_INVALID_SIZE;

    if (raw_clock_mode) {
        // C3 raw PDM copies each selected 16-bit sample to both slots.
        for (size_t i = 0; i < PDM_HALFWORDS_PER_FRAME; i += 2) {
            const uint16_t word = frame.halfwords[i];
            if (word != frame.halfwords[i + 1]) return ESP_ERR_INVALID_RESPONSE;
            reply[i] = word >> 8;
            reply[i + 1] = word;
        }
    } else {
        for (size_t i = 0; i < SPI_REPLY_PROBE_BYTES; ++i) {
            reply[i] = frame.word >> (8 * (SPI_REPLY_PROBE_BYTES - 1 - i));
        }
    }
    return ESP_OK;
}

esp_err_t spi_reply_probe_end(void)
{
    if (!rx) return ESP_OK;
    if (enabled) {
        esp_err_t result = i2s_channel_disable(rx);
        if (result != ESP_OK) return result;
        enabled = false;
    }

    esp_err_t result = i2s_del_channel(rx);
    if (result == ESP_OK) {
        rx = NULL;
        esp_rom_gpio_connect_in_signal(GPIO_MATRIX_CONST_ZERO_INPUT, I2SI_BCK_IN_IDX, false);
        esp_rom_gpio_connect_in_signal(GPIO_MATRIX_CONST_ZERO_INPUT, I2SI_WS_IN_IDX, false);
        esp_rom_gpio_connect_in_signal(GPIO_MATRIX_CONST_ZERO_INPUT, I2SI_SD_IN_IDX, false);
    }
    return result;
}

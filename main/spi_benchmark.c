#include "spi_benchmark.h"

#include <string.h>

#include "driver/spi_master.h"
#include "esp_rom_sys.h"

static void release_pin(gpio_num_t pin)
{
    gpio_reset_pin(pin);
    gpio_set_pull_mode(pin, GPIO_FLOATING);
    gpio_set_direction(pin, GPIO_MODE_INPUT);
}

esp_err_t spi_benchmark_transfer(gpio_num_t sck, gpio_num_t mosi, gpio_num_t miso,
                                 gpio_num_t cs, unsigned hz, const uint8_t *tx,
                                 uint8_t *rx, size_t length)
{
    const gpio_num_t pins[] = {sck, mosi, miso, cs};
    if (!tx || !rx || length == 0 || length > 4 || hz < SPI_BENCHMARK_MIN_HZ ||
        hz > SPI_BENCHMARK_MAX_HZ) {
        return ESP_ERR_INVALID_ARG;
    }
    uint64_t assigned = 0;
    for (size_t i = 0; i < sizeof(pins) / sizeof(pins[0]); ++i) {
        if (!GPIO_IS_VALID_OUTPUT_GPIO(pins[i])) return ESP_ERR_INVALID_ARG;
        uint64_t bit = UINT64_C(1) << pins[i];
        if (assigned & bit) return ESP_ERR_INVALID_ARG;
        assigned |= bit;
    }

    const spi_bus_config_t bus = {
        .mosi_io_num = mosi,
        .miso_io_num = miso,
        .sclk_io_num = sck,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .data4_io_num = -1,
        .data5_io_num = -1,
        .data6_io_num = -1,
        .data7_io_num = -1,
    };
    spi_device_interface_config_t config = {
        .mode = 0,
        .clock_source = SPI_CLK_SRC_XTAL,
        .clock_speed_hz = hz,
        .spics_io_num = -1,
        .queue_size = 1,
    };

    gpio_set_level(cs, 1);
    gpio_set_direction(cs, GPIO_MODE_OUTPUT);
    esp_err_t result = spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_DISABLED);
    if (result != ESP_OK) {
        release_pin(cs);
        return result;
    }
    spi_device_handle_t device;
    result = spi_bus_add_device(SPI2_HOST, &config, &device);
    if (result == ESP_OK) {
        spi_transaction_t transaction = {
            .flags = SPI_TRANS_USE_TXDATA | SPI_TRANS_USE_RXDATA,
            .length = length * 8,
            .rxlength = length * 8,
        };
        memcpy(transaction.tx_data, tx, length);
        // Delimit CS distinctly from SCK at the analyzer's microsecond resolution.
        gpio_set_level(cs, 0);
        esp_rom_delay_us(10);
        result = spi_device_polling_transmit(device, &transaction);
        esp_rom_delay_us(10);
        gpio_set_level(cs, 1);
        esp_rom_delay_us(10);
        if (result == ESP_OK) memcpy(rx, transaction.rx_data, length);
        esp_err_t removed = spi_bus_remove_device(device);
        if (result == ESP_OK) result = removed;
    }
    esp_err_t freed = spi_bus_free(SPI2_HOST);
    if (result == ESP_OK) result = freed;

    for (size_t i = 0; i < sizeof(pins) / sizeof(pins[0]); ++i) release_pin(pins[i]);
    return result;
}

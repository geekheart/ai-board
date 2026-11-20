#include "protocol_receive_fixture.h"

#include <stdlib.h>
#include "driver/rmt_tx.h"
#include "esp_cpu.h"
#include "esp_private/esp_clk.h"
#include "hal/gpio_ll.h"
#include "soc/soc_caps.h"

enum {
    RMT_HZ = 80000000,
    UART_BITS = 10,
    UART_TX_WAIT_MS = 100,
    UART_RAM_BLOCKS = 4,
    I2C_LINE_TIMEOUT_US = 1000,
    I2C_MAX_BYTES = 64,
};

struct uart_receive_fixture {
    rmt_channel_handle_t channel;
    rmt_encoder_handle_t encoder;
    bool enabled;
    size_t symbol_count;
    rmt_symbol_word_t symbols[RECEIVE_FIXTURE_MAX_BYTES * UART_BITS / 2];
};

static esp_err_t uart_emit(const uart_receive_fixture_t *fixture,
                          const rmt_symbol_word_t *symbols, size_t count)
{
    const rmt_transmit_config_t config = {.flags.eot_level = 1};
    esp_err_t status = rmt_transmit(fixture->channel, fixture->encoder, symbols,
                                    count * sizeof(*symbols), &config);
    return status == ESP_OK ? rmt_tx_wait_all_done(fixture->channel, UART_TX_WAIT_MS) : status;
}

void receive_fixture_uart_close(uart_receive_fixture_t *fixture)
{
    if (!fixture) return;
    if (fixture->enabled) rmt_disable(fixture->channel);
    if (fixture->encoder) rmt_del_encoder(fixture->encoder);
    if (fixture->channel) rmt_del_channel(fixture->channel);
    free(fixture);
}

esp_err_t receive_fixture_uart_open(gpio_num_t tx, unsigned baud, const uint8_t *data, size_t size,
                                    uart_receive_fixture_t **output, unsigned *actual_baud)
{
    if (!output || !actual_baud || !data || !size || size > RECEIVE_FIXTURE_MAX_BYTES ||
        baud < 9600 || baud > 5000000 || !GPIO_IS_VALID_OUTPUT_GPIO(tx)) return ESP_ERR_INVALID_ARG;
    *output = NULL;
    *actual_baud = 0;
    uart_receive_fixture_t *fixture = calloc(1, sizeof(*fixture));
    if (!fixture) return ESP_ERR_NO_MEM;
    // At 5 Mbaud each bit is 16 APB ticks, above the C3 RMT minimum of 12.
    const uint16_t ticks = (RMT_HZ + baud / 2) / baud;
    fixture->symbol_count = size * UART_BITS / 2;
    for (size_t i = 0; i < size; ++i) {
        const unsigned word = ((unsigned)data[i] << 1) | (1U << (UART_BITS - 1));
        for (unsigned bit = 0; bit < UART_BITS; bit += 2) {
            fixture->symbols[(i * UART_BITS + bit) / 2] = (rmt_symbol_word_t){
                .level0 = (word >> bit) & 1, .duration0 = ticks,
                .level1 = (word >> (bit + 1)) & 1, .duration1 = ticks,
            };
        }
    }

    const rmt_tx_channel_config_t config = {
        .gpio_num = tx, .clk_src = RMT_CLK_SRC_APB, .resolution_hz = RMT_HZ,
        .mem_block_symbols = UART_RAM_BLOCKS * SOC_RMT_MEM_WORDS_PER_CHANNEL,
        .trans_queue_depth = 1, .flags.init_level = 1,
    };
    const rmt_copy_encoder_config_t encoder_config = {};
    esp_err_t status = rmt_new_tx_channel(&config, &fixture->channel);
    if (status == ESP_OK) status = rmt_new_copy_encoder(&encoder_config, &fixture->encoder);
    if (status == ESP_OK) {
        status = rmt_enable(fixture->channel);
        fixture->enabled = status == ESP_OK;
    }
    if (status == ESP_OK) {
        // The enabled C3 channel applies its idle level on first TX.
        const rmt_symbol_word_t idle = {.level0 = 1, .duration0 = 160, .level1 = 1, .duration1 = 160};
        status = uart_emit(fixture, &idle, 1);
    }
    if (status != ESP_OK) {
        receive_fixture_uart_close(fixture);
        return status;
    }
    *output = fixture;
    *actual_baud = (RMT_HZ + ticks / 2) / ticks;
    return ESP_OK;
}

esp_err_t receive_fixture_uart_send(uart_receive_fixture_t *fixture)
{
    return uart_emit(fixture, fixture->symbols, fixture->symbol_count);
}

typedef struct {
    gpio_num_t sda;
    gpio_num_t scl;
    uint32_t period;
    uint32_t minimum_low;
    uint32_t minimum_high;
    uint32_t read_turnaround_low;
    uint32_t data_hold;
    uint32_t data_rise_setup;
    uint32_t low_since;
    uint32_t line_timeout;
    uint32_t first_rise;
    uint32_t last_rise;
    unsigned rising_edges;
} i2c_fixture_bus_t;

static void wait_since(uint32_t start, uint32_t ticks)
{
    while (esp_cpu_get_cycle_count() - start < ticks) {}
}

static void wait_cycles(uint32_t ticks)
{
    wait_since(esp_cpu_get_cycle_count(), ticks);
}

static void scl_low(i2c_fixture_bus_t *bus)
{
    gpio_ll_set_level(&GPIO, bus->scl, 0);
    bus->low_since = esp_cpu_get_cycle_count();
}

static bool i2c_wait_high(const i2c_fixture_bus_t *bus, gpio_num_t pin)
{
    const uint32_t start = esp_cpu_get_cycle_count();
    while (!gpio_ll_get_level(&GPIO, pin)) {
        if (esp_cpu_get_cycle_count() - start >= bus->line_timeout) return false;
    }
    return true;
}

static bool i2c_scl_release(const i2c_fixture_bus_t *bus)
{
    gpio_ll_set_level(&GPIO, bus->scl, 1);
    return i2c_wait_high(bus, bus->scl);
}

static void i2c_set_data(const i2c_fixture_bus_t *bus, bool released)
{
    // Let SCL finish falling at the receiver before transitioning SDA.
    wait_since(bus->low_since, bus->data_hold);
    gpio_ll_set_level(&GPIO, bus->sda, released);
}

static bool i2c_wait_data_high(const i2c_fixture_bus_t *bus)
{
    if (!i2c_wait_high(bus, bus->sda)) return false;
    // Allow the SDA rise to reach the receiver across the analog switch.
    wait_cycles(bus->data_rise_setup);
    return true;
}

static esp_err_t i2c_clock(i2c_fixture_bus_t *bus, bool released, bool *sample)
{
    i2c_set_data(bus, released);
    if (released && !sample && !i2c_wait_data_high(bus)) return ESP_ERR_TIMEOUT;
    wait_since(bus->low_since, bus->minimum_low);
    if (!i2c_scl_release(bus)) return ESP_ERR_TIMEOUT;
    bus->last_rise = esp_cpu_get_cycle_count();
    if (!bus->rising_edges++) bus->first_rise = bus->last_rise;
    // Include GPIO and rise-time overhead in the SCL period while still
    // honoring the minimum high time after the line actually rises.
    wait_since(bus->last_rise, bus->minimum_high);
    wait_since(bus->low_since, bus->period);
    if (sample) *sample = gpio_ll_get_level(&GPIO, bus->sda);
    scl_low(bus);
    return ESP_OK;
}

static esp_err_t i2c_write_byte(i2c_fixture_bus_t *bus, uint8_t value)
{
    for (int bit = 7; bit >= 0; --bit) {
        const esp_err_t status = i2c_clock(bus, (value >> bit) & 1, NULL);
        if (status != ESP_OK) return status;
    }
    bool nack;
    const esp_err_t status = i2c_clock(bus, true, &nack);
    return status == ESP_OK && nack ? ESP_ERR_INVALID_RESPONSE : status;
}

static esp_err_t i2c_read_byte(i2c_fixture_bus_t *bus, uint8_t *value, bool last)
{
    // Release the host's ACK while SCL is still low. The pull-ups and
    // CH446Q path need extra time to settle if the next MSB is high.
    i2c_set_data(bus, true);
    wait_since(bus->low_since, bus->read_turnaround_low);
    uint8_t data = 0;
    for (unsigned bit = 0; bit < 8; ++bit) {
        bool high;
        const esp_err_t status = i2c_clock(bus, true, &high);
        if (status != ESP_OK) return status;
        data = (data << 1) | high;
    }
    const esp_err_t status = i2c_clock(bus, last, NULL);
    if (status == ESP_OK) *value = data;
    return status;
}

static esp_err_t i2c_repeated_start(i2c_fixture_bus_t *bus)
{
    // Release SDA while SCL is low, and verify it rose before releasing
    // SCL so that the intervening interval cannot be seen as a STOP.
    i2c_set_data(bus, true);
    if (!i2c_wait_data_high(bus)) return ESP_ERR_TIMEOUT;
    wait_since(bus->low_since, bus->minimum_low);
    if (!i2c_scl_release(bus)) return ESP_ERR_TIMEOUT;
    wait_cycles(bus->period / 2);
    gpio_ll_set_level(&GPIO, bus->sda, 0);
    wait_cycles(bus->period / 2);
    scl_low(bus);
    return ESP_OK;
}

static esp_err_t i2c_stop(i2c_fixture_bus_t *bus)
{
    // Also use this on failure, when SCL may have been left released.
    scl_low(bus);
    i2c_set_data(bus, false);
    wait_since(bus->low_since, bus->minimum_low);
    const bool clock_high = i2c_scl_release(bus);
    wait_cycles(bus->period / 2);
    gpio_ll_set_level(&GPIO, bus->sda, 1);
    if (!clock_high || !i2c_wait_high(bus, bus->sda)) return ESP_ERR_TIMEOUT;
    return ESP_OK;
}

static esp_err_t i2c_transfer(gpio_num_t sda, gpio_num_t scl, uint8_t address, unsigned hz,
                              const uint8_t *write_data, size_t write_size,
                              uint8_t *read_data, size_t read_size,
                              size_t *received, unsigned *actual_hz)
{
    if (actual_hz) *actual_hz = 0;
    if (received) *received = 0;
    if (!write_data || !write_size || write_size > I2C_MAX_BYTES || !actual_hz ||
        read_size > I2C_MAX_BYTES || (read_size && (!read_data || !received)) ||
        hz < 10000 || hz > 400000 || address < 8 || address > 119 ||
        !GPIO_IS_VALID_OUTPUT_GPIO(sda) || !GPIO_IS_VALID_OUTPUT_GPIO(scl) || sda == scl) {
        return ESP_ERR_INVALID_ARG;
    }
    const unsigned cpu_hz = esp_clk_cpu_freq();
    const unsigned cycles_us = cpu_hz / 1000000;
    const bool fast = hz > 100000;
    i2c_fixture_bus_t bus = {
        .sda = sda, .scl = scl, .period = (cpu_hz + hz - 1) / hz,
        .minimum_low = (cycles_us * (fast ? 13 : 47) + 9) / 10,
        .minimum_high = (cycles_us * (fast ? 6 : 40) + 9) / 10,
        .read_turnaround_low = (cycles_us * (fast ? 28 : 47) + 9) / 10,
        .data_hold = (cycles_us * 3 + 9) / 10,
        .data_rise_setup = (cycles_us * 6 + 9) / 10,
        .line_timeout = cycles_us * I2C_LINE_TIMEOUT_US,
    };
    if (!i2c_scl_release(&bus)) return ESP_ERR_TIMEOUT;
    wait_cycles(bus.period / 2);
    if (!gpio_ll_get_level(&GPIO, sda)) return ESP_ERR_INVALID_STATE;

    gpio_ll_set_level(&GPIO, sda, 0);
    wait_cycles(bus.period / 2);
    scl_low(&bus);
    esp_err_t status = i2c_write_byte(&bus, address << 1);
    for (size_t i = 0; i < write_size && status == ESP_OK; ++i) {
        status = i2c_write_byte(&bus, write_data[i]);
    }
    if (read_size && status == ESP_OK) {
        status = i2c_repeated_start(&bus);
        if (status == ESP_OK) status = i2c_write_byte(&bus, (address << 1) | 1);
        for (size_t i = 0; i < read_size && status == ESP_OK; ++i) {
            status = i2c_read_byte(&bus, &read_data[i], i + 1 == read_size);
            if (status == ESP_OK) ++*received;
        }
    }
    const esp_err_t stop_status = i2c_stop(&bus);
    if (bus.rising_edges > 1) {
        *actual_hz = (uint64_t)(bus.rising_edges - 1) * cpu_hz / (bus.last_rise - bus.first_rise);
    }
    return status == ESP_OK ? stop_status : status;
}

esp_err_t receive_fixture_i2c_write(gpio_num_t sda, gpio_num_t scl, uint8_t address, unsigned hz,
                                    const uint8_t *data, size_t size, unsigned *actual_hz)
{
    return i2c_transfer(sda, scl, address, hz, data, size, NULL, 0, NULL, actual_hz);
}

esp_err_t receive_fixture_i2c_write_read(gpio_num_t sda, gpio_num_t scl, uint8_t address, unsigned hz,
                                         const uint8_t *write_data, size_t write_size,
                                         uint8_t *read_data, size_t read_size,
                                         size_t *received, unsigned *actual_hz)
{
    if (!read_size) return ESP_ERR_INVALID_ARG;
    return i2c_transfer(sda, scl, address, hz, write_data, write_size, read_data, read_size,
                        received, actual_hz);
}

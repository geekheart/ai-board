#include "spi_stimulus.h"

#include <stdbool.h>

#include "driver/rmt_tx.h"
#include "esp_rom_sys.h"
#include "soc/soc_caps.h"

enum {
    CLOCK_CHANNEL,
    DATA_CHANNEL,
    CHANNELS,
    MAX_BITS = 32,
    RESOLUTION_HZ = 80000000,
    // ESP32-C3 TRM equation 33.1. Each phase must exceed 5 APB cycles
    // plus 6 RMT source cycles. Both clocks run at 80 MHz here.
    MIN_PHASE_TICKS = 12,
    GAP_STEP_NS = 25,
    GAP_STEP_TICKS = RESOLUTION_HZ / (1000000000 / GAP_STEP_NS),
    BENCH_IDLE_TICKS = 4 * RESOLUTION_HZ / 1000000,
    PAIR_CS_SETUP_TICKS = RESOLUTION_HZ / 1000000,
    PAIR_CS_HOLD_TICKS = RESOLUTION_HZ / 1000000,
    RMT_DURATION_MAX = 32767,
    CS_SETUP_US = 50,
    CS_HOLD_US = 10,
    TX_TIMEOUT_MS = 100,
};

typedef struct {
    gpio_num_t pin;
    const rmt_symbol_word_t *symbols;
    size_t symbol_count;
    size_t memory_symbols;
    bool idle_high;
    rmt_channel_handle_t channel;
    rmt_encoder_handle_t encoder;
    bool enabled;
} stimulus_output_t;

static void release_pin(gpio_num_t pin)
{
    gpio_reset_pin(pin);
    gpio_set_pull_mode(pin, GPIO_FLOATING);
    gpio_set_direction(pin, GPIO_MODE_INPUT);
}

static void tx_symbols(const uint8_t *bytes, size_t bit_count, uint16_t half_period,
                       rmt_symbol_word_t *clock, rmt_symbol_word_t *data)
{
    for (size_t index = 0; index < bit_count; ++index) {
        const uint8_t bit = (bytes[index / 8] >> (7 - index % 8)) & 1;
        clock[index] = (rmt_symbol_word_t){
            .duration0 = half_period, .level0 = 0,
            .duration1 = half_period, .level1 = 1,
        };
        data[index] = (rmt_symbol_word_t){
            .duration0 = half_period, .level0 = bit,
            .duration1 = half_period, .level1 = bit,
        };
    }
}

static void propagate_status(esp_err_t *result, esp_err_t cleanup)
{
    if (*result == ESP_OK) *result = cleanup;
}

static bool valid_pins(gpio_num_t sck, gpio_num_t mosi, gpio_num_t cs)
{
    return GPIO_IS_VALID_OUTPUT_GPIO(sck) && GPIO_IS_VALID_OUTPUT_GPIO(mosi) &&
           GPIO_IS_VALID_OUTPUT_GPIO(cs) && sck != mosi && sck != cs && mosi != cs;
}

static uint16_t half_clock_period(unsigned hz)
{
    if (hz < SPI_STIMULUS_MIN_HZ || hz > SPI_STIMULUS_MAX_HZ) return 0;
    // Quantize upward and keep each half-period within the hardware limit.
    unsigned ticks = (RESOLUTION_HZ / 2 + hz - 1) / hz;
    return ticks < MIN_PHASE_TICKS ? MIN_PHASE_TICKS : ticks;
}

static esp_err_t configure_outputs(stimulus_output_t outputs[CHANNELS], rmt_sync_manager_handle_t *sync)
{
    rmt_channel_handle_t channels[CHANNELS];
    for (size_t i = 0; i < CHANNELS; ++i) {
        stimulus_output_t *output = &outputs[i];
        const rmt_tx_channel_config_t config = {
            .gpio_num = output->pin,
            .clk_src = RMT_CLK_SRC_APB,
            .resolution_hz = RESOLUTION_HZ,
            .mem_block_symbols = output->memory_symbols,
            .trans_queue_depth = 1,
            .flags.init_level = output->idle_high,
        };
        const rmt_copy_encoder_config_t encoder_config = {};
        esp_err_t result = rmt_new_tx_channel(&config, &output->channel);
        if (result != ESP_OK) return result;
        result = rmt_new_copy_encoder(&encoder_config, &output->encoder);
        if (result != ESP_OK) return result;
        result = rmt_enable(output->channel);
        if (result != ESP_OK) return result;
        output->enabled = true;
        channels[i] = output->channel;
    }

    const rmt_sync_manager_config_t group = {.tx_channel_array = channels, .array_size = CHANNELS};
    return rmt_new_sync_manager(&group, sync);
}

static esp_err_t transmit_outputs(stimulus_output_t outputs[CHANNELS])
{
    for (size_t i = 0; i < CHANNELS; ++i) {
        stimulus_output_t *output = &outputs[i];
        const rmt_transmit_config_t config = {.flags.eot_level = output->idle_high};
        esp_err_t result = rmt_transmit(output->channel, output->encoder, output->symbols,
                                       output->symbol_count * sizeof(*output->symbols), &config);
        if (result != ESP_OK) return result;
    }
    for (size_t i = 0; i < CHANNELS; ++i) {
        esp_err_t result = rmt_tx_wait_all_done(outputs[i].channel, TX_TIMEOUT_MS);
        if (result != ESP_OK) return result;
    }
    return ESP_OK;
}

static esp_err_t prime_outputs(const stimulus_output_t outputs[CHANNELS], rmt_sync_manager_handle_t sync)
{
    // The C3 RMT applies the idle-output configuration at TX start. Drive
    // an idle-only transfer before the receiving peripherals are armed.
    rmt_symbol_word_t symbols[CHANNELS];
    stimulus_output_t idle[CHANNELS];
    for (size_t i = 0; i < CHANNELS; ++i) {
        symbols[i] = (rmt_symbol_word_t){
            .level0 = outputs[i].idle_high, .duration0 = BENCH_IDLE_TICKS / 2,
            .level1 = outputs[i].idle_high, .duration1 = BENCH_IDLE_TICKS / 2,
        };
        idle[i] = outputs[i];
        idle[i].symbols = &symbols[i];
        idle[i].symbol_count = 1;
    }
    esp_err_t result = transmit_outputs(idle);
    return result == ESP_OK ? rmt_sync_reset(sync) : result;
}

static esp_err_t release_outputs(stimulus_output_t outputs[CHANNELS], rmt_sync_manager_handle_t sync,
                                 esp_err_t result)
{
    for (size_t i = 0; i < CHANNELS; ++i) {
        if (outputs[i].enabled) propagate_status(&result, rmt_disable(outputs[i].channel));
    }
    if (sync) propagate_status(&result, rmt_del_sync_manager(sync));
    for (size_t i = 0; i < CHANNELS; ++i) {
        if (outputs[i].encoder) propagate_status(&result, rmt_del_encoder(outputs[i].encoder));
        if (outputs[i].channel) {
            propagate_status(&result, rmt_del_channel(outputs[i].channel));
            release_pin(outputs[i].pin);
        }
    }
    return result;
}

static esp_err_t transfer_single(gpio_num_t sck, gpio_num_t mosi, gpio_num_t cs,
                                 unsigned hz, const uint8_t *tx, size_t length, bool bench,
                                 spi_stimulus_ready_fn ready, void *ready_arg, unsigned *actual_hz)
{
    const uint16_t half_period = half_clock_period(hz);
    if (!tx || !length || length > MAX_BITS / 8 || !half_period || !valid_pins(sck, mosi, cs)) {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t prefix = bench ? 1 : 0;
    const size_t bit_count = length * 8;
    const bool initial_data = bench && (tx[0] & 0x80);
    rmt_symbol_word_t symbols[CHANNELS][MAX_BITS + 1];
    if (bench) {
        symbols[CLOCK_CHANNEL][0] = (rmt_symbol_word_t){
            .duration0 = BENCH_IDLE_TICKS / 2, .duration1 = BENCH_IDLE_TICKS / 2,
        };
        symbols[DATA_CHANNEL][0] = (rmt_symbol_word_t){
            .duration0 = BENCH_IDLE_TICKS / 2, .level0 = initial_data,
            .duration1 = BENCH_IDLE_TICKS / 2, .level1 = initial_data,
        };
    }
    tx_symbols(tx, bit_count, half_period, symbols[CLOCK_CHANNEL] + prefix,
               symbols[DATA_CHANNEL] + prefix);

    // Keep the legacy allocation order. The benchmark starts data on TX0
    // first, with the clock on TX1 as in the two-frame fixture.
    const size_t clock_output = bench ? 1 : 0;
    stimulus_output_t outputs[CHANNELS];
    outputs[clock_output] = (stimulus_output_t){
        .pin = sck, .symbols = symbols[CLOCK_CHANNEL], .symbol_count = bit_count + prefix,
        .memory_symbols = SOC_RMT_MEM_WORDS_PER_CHANNEL,
    };
    outputs[1 - clock_output] = (stimulus_output_t){
        .pin = mosi, .symbols = symbols[DATA_CHANNEL], .symbol_count = bit_count + prefix,
        .memory_symbols = SOC_RMT_MEM_WORDS_PER_CHANNEL, .idle_high = initial_data,
    };
    rmt_sync_manager_handle_t sync = NULL;

    gpio_set_level(cs, 1);
    gpio_set_direction(cs, GPIO_MODE_OUTPUT);
    esp_err_t result = configure_outputs(outputs, &sync);
    if (result == ESP_OK && bench) result = prime_outputs(outputs, sync);
    if (result == ESP_OK && ready && !ready(ready_arg)) result = ESP_ERR_INVALID_STATE;
    if (result == ESP_OK) {
        gpio_set_level(cs, 0);
        esp_rom_delay_us(CS_SETUP_US);
        result = transmit_outputs(outputs);
        esp_rom_delay_us(CS_HOLD_US);
        gpio_set_level(cs, 1);
    }
    result = release_outputs(outputs, sync, result);
    release_pin(cs);
    if (result == ESP_OK && actual_hz) *actual_hz = RESOLUTION_HZ / (2u * half_period);
    return result;
}

esp_err_t spi_stimulus_transfer(gpio_num_t sck, gpio_num_t mosi, gpio_num_t cs,
                                unsigned hz, const uint8_t *tx, size_t length)
{
    return transfer_single(sck, mosi, cs, hz, tx, length, false, NULL, NULL, NULL);
}

esp_err_t spi_stimulus_transfer_bench(gpio_num_t sck, gpio_num_t mosi, gpio_num_t cs,
                                      unsigned hz, const uint8_t *tx, size_t length,
                                      spi_stimulus_ready_fn ready, void *ready_arg,
                                      unsigned *actual_hz)
{
    return transfer_single(sck, mosi, cs, hz, tx, length, true, ready, ready_arg, actual_hz);
}

typedef struct {
    rmt_symbol_word_t *symbols;
    size_t phases;
} stimulus_waveform_t;

static void append_level(stimulus_waveform_t *wave, bool level, uint32_t ticks)
{
    while (ticks) {
        uint16_t duration = ticks > RMT_DURATION_MAX ? RMT_DURATION_MAX : ticks;
        if (ticks > duration && ticks - duration < MIN_PHASE_TICKS) {
            duration = ticks - MIN_PHASE_TICKS;
        }
        rmt_symbol_word_t *symbol = &wave->symbols[wave->phases / 2];
        if (wave->phases % 2) {
            symbol->level1 = level;
            symbol->duration1 = duration;
        } else {
            *symbol = (rmt_symbol_word_t){.level0 = level, .duration0 = duration};
        }
        wave->phases++;
        ticks -= duration;
    }
}

static void append_clocks(stimulus_waveform_t *wave, uint16_t half_period)
{
    for (size_t bit = 0; bit < MAX_BITS; ++bit) {
        append_level(wave, false, half_period);
        append_level(wave, true, half_period);
    }
}

esp_err_t spi_stimulus_transfer_pair(gpio_num_t sck, gpio_num_t mosi, gpio_num_t cs,
                                     unsigned hz, uint32_t gap_ns, bool mosi_high,
                                     unsigned *actual_hz, uint32_t *actual_gap_ns,
                                     spi_stimulus_ready_fn ready, void *ready_arg)
{
    const uint16_t half_period = half_clock_period(hz);
    if (!half_period || !valid_pins(sck, mosi, cs) ||
        gap_ns < SPI_STIMULUS_MIN_GAP_NS || gap_ns > SPI_STIMULUS_MAX_GAP_NS) {
        return ESP_ERR_INVALID_ARG;
    }

    const uint32_t gap_steps = (gap_ns + GAP_STEP_NS - 1) / GAP_STEP_NS;
    const uint32_t gap_ticks = gap_steps * GAP_STEP_TICKS;
    const uint32_t frame_ticks = PAIR_CS_SETUP_TICKS + 2 * MAX_BITS * half_period + PAIR_CS_HOLD_TICKS;
    rmt_symbol_word_t cs_symbols[SOC_RMT_MEM_WORDS_PER_CHANNEL - 1];
    rmt_symbol_word_t clock_symbols[2 * SOC_RMT_MEM_WORDS_PER_CHANNEL - 1];
    stimulus_waveform_t select = {.symbols = cs_symbols};
    stimulus_waveform_t clock = {.symbols = clock_symbols};

    append_level(&select, true, BENCH_IDLE_TICKS);
    append_level(&select, false, frame_ticks);
    append_level(&select, true, gap_ticks);
    append_level(&select, false, frame_ticks);
    append_level(&select, true, MIN_PHASE_TICKS);

    append_level(&clock, false, BENCH_IDLE_TICKS + PAIR_CS_SETUP_TICKS);
    append_clocks(&clock, half_period);
    append_level(&clock, false, PAIR_CS_HOLD_TICKS + gap_ticks + PAIR_CS_SETUP_TICKS);
    append_clocks(&clock, half_period);
    append_level(&clock, false, PAIR_CS_HOLD_TICKS);

    // The C3 has two TX channels. Reserve the short CS waveform on TX0 first,
    // then let SCK on TX1 borrow the adjacent RX block. Both complete using
    // preloaded symbols, so interrupt latency can't change the CS interval.
    stimulus_output_t outputs[CHANNELS] = {
        {.pin = cs, .symbols = cs_symbols, .symbol_count = (select.phases + 1) / 2,
         .memory_symbols = SOC_RMT_MEM_WORDS_PER_CHANNEL, .idle_high = true},
        {.pin = sck, .symbols = clock_symbols, .symbol_count = (clock.phases + 1) / 2,
         .memory_symbols = 2 * SOC_RMT_MEM_WORDS_PER_CHANNEL},
    };
    rmt_sync_manager_handle_t sync = NULL;
    gpio_set_level(cs, 1);
    gpio_set_direction(cs, GPIO_MODE_OUTPUT);
    gpio_set_level(mosi, mosi_high);
    gpio_set_direction(mosi, GPIO_MODE_OUTPUT);
    esp_err_t result = configure_outputs(outputs, &sync);
    if (result == ESP_OK) result = prime_outputs(outputs, sync);
    if (result == ESP_OK && ready && !ready(ready_arg)) result = ESP_ERR_INVALID_STATE;
    if (result == ESP_OK) result = transmit_outputs(outputs);
    result = release_outputs(outputs, sync, result);
    release_pin(cs);
    release_pin(mosi);
    if (result == ESP_OK) {
        if (actual_hz) *actual_hz = RESOLUTION_HZ / (2u * half_period);
        if (actual_gap_ns) *actual_gap_ns = gap_steps * GAP_STEP_NS;
    }
    return result;
}

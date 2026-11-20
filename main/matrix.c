#include <stdbool.h>
#include <stdio.h>
#include "matrix.h"
#include "driver/ledc.h"
#include "esp_rom_sys.h"

enum { CLK = 9, DATA = 7, STB = 2, RESET = 10 };
static const gpio_num_t links[MATRIX_Y_COUNT] = {18, 8, 6, 5, 19, 4, 3, 1};
static uint16_t active_routes[MATRIX_Y_COUNT];
static matrix_mode_t modes[MATRIX_Y_COUNT];
static int pwm_link = -1;

static void output(gpio_num_t pin, int level)
{
    ESP_ERROR_CHECK(gpio_set_level(pin, level));
    ESP_ERROR_CHECK(gpio_set_direction(pin, GPIO_MODE_OUTPUT));
}

static void reset_matrix(void)
{
    gpio_set_level(RESET, 1);
    esp_rom_delay_us(10);
    gpio_set_level(RESET, 0);
    esp_rom_delay_us(10);
}

static void route(int x, int y, bool closed)
{
    unsigned address = (y << 4) | x;
    for (int bit = 6; bit >= 0; --bit) {
        gpio_set_level(DATA, (address >> bit) & 1);
        esp_rom_delay_us(3);
        gpio_set_level(CLK, 1);
        esp_rom_delay_us(3);
        gpio_set_level(CLK, 0);
    }
    gpio_set_level(DATA, closed);
    esp_rom_delay_us(3);
    gpio_set_level(STB, 1);
    esp_rom_delay_us(5);
    gpio_set_level(STB, 0);
    esp_rom_delay_us(5);
}

static void link_input(unsigned y)
{
    if (pwm_link == (int)y) {
        ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
        gpio_reset_pin(links[y]);
        pwm_link = -1;
    }
    gpio_set_direction(links[y], GPIO_MODE_INPUT);
    gpio_set_pull_mode(links[y], GPIO_FLOATING);
    modes[y] = MATRIX_INPUT;
}

void matrix_boot_reset(void)
{
    for (unsigned y = 0; y < MATRIX_Y_COUNT; ++y) {
        gpio_reset_pin(links[y]);
        link_input(y);
    }
    output(STB, 0);
    output(RESET, 0);
    reset_matrix();
}

bool matrix_has_routes(void)
{
    for (unsigned y = 0; y < MATRIX_Y_COUNT; ++y) {
        if (active_routes[y]) return true;
    }
    return false;
}

matrix_net_t matrix_link_net(unsigned y)
{
    matrix_net_t net = {.ports = active_routes[y], .links = 1U << y};
    uint8_t previous;
    // A port shared by two Y channels joins their entire electrical nets.
    do {
        previous = net.links;
        for (unsigned link = 0; link < MATRIX_Y_COUNT; ++link) {
            if (net.ports & active_routes[link]) {
                net.links |= 1U << link;
                net.ports |= active_routes[link];
            }
        }
    } while (net.links != previous);
    return net;
}

bool matrix_links_have_output(uint8_t link_mask)
{
    for (unsigned link = 0; link < MATRIX_Y_COUNT; ++link) {
        if ((link_mask & (1U << link)) && modes[link] >= MATRIX_LOW) return true;
    }
    return false;
}

void matrix_apply(const uint16_t routes[MATRIX_Y_COUNT])
{
    for (unsigned y = 0; y < MATRIX_Y_COUNT; ++y) link_input(y);
    reset_matrix();
    output(DATA, 0);
    output(CLK, 0);
    for (unsigned y = 0; y < MATRIX_Y_COUNT; ++y) {
        active_routes[y] = routes[y];
        for (unsigned x = 0; x < MATRIX_X_COUNT; ++x) {
            if (routes[y] & (1U << x)) route(x, y, true);
        }
    }
    gpio_set_direction(CLK, GPIO_MODE_INPUT);
}

uint16_t matrix_route_mask(unsigned y) { return active_routes[y]; }
gpio_num_t matrix_link_gpio(unsigned y) { return links[y]; }
matrix_mode_t matrix_link_mode(unsigned y) { return modes[y]; }
int matrix_link_level(unsigned y) { return gpio_get_level(links[y]); }

const char *matrix_mode_name(matrix_mode_t mode)
{
    static const char *names[] = {"input", "pullup", "pulldown", "low", "high", "pwm"};
    return names[mode];
}

static esp_err_t configure_pwm(unsigned y, unsigned hz, unsigned duty)
{
    const ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = hz,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    esp_err_t result = ledc_timer_config(&timer);
    if (result != ESP_OK) return result;
    const ledc_channel_config_t channel = {
        .gpio_num = links[y],
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .duty = duty * 256 / 100,
    };
    result = ledc_channel_config(&channel);
    if (result == ESP_OK) {
        pwm_link = y;
        gpio_input_enable(links[y]);
    } else {
        gpio_reset_pin(links[y]);
        link_input(y);
    }
    return result;
}

esp_err_t matrix_configure_link(unsigned y, matrix_mode_t mode, unsigned hz, unsigned duty)
{
    if (mode >= MATRIX_LOW) {
        const matrix_net_t net = matrix_link_net(y);
        const uint8_t peers = net.links & ~(1U << y);
        if (!net.ports || (net.ports & (net.ports - 1)) || matrix_links_have_output(peers)) {
            return ESP_ERR_INVALID_STATE;
        }
    }
    if (mode == MATRIX_PWM && pwm_link >= 0 && pwm_link != (int)y) return ESP_ERR_INVALID_STATE;

    link_input(y);
    esp_err_t result = ESP_OK;
    switch (mode) {
    case MATRIX_INPUT: break;
    case MATRIX_PULLUP: result = gpio_set_pull_mode(links[y], GPIO_PULLUP_ONLY); break;
    case MATRIX_PULLDOWN: result = gpio_set_pull_mode(links[y], GPIO_PULLDOWN_ONLY); break;
    case MATRIX_LOW:
    case MATRIX_HIGH:
        result = gpio_set_level(links[y], mode == MATRIX_HIGH);
        if (result == ESP_OK) result = gpio_set_direction(links[y], GPIO_MODE_INPUT_OUTPUT);
        break;
    case MATRIX_PWM: result = configure_pwm(y, hz, duty); break;
    }
    if (result == ESP_OK) modes[y] = mode;
    return result;
}

static int sample(gpio_num_t pin)
{
    esp_rom_delay_us(50);
    return gpio_get_level(pin);
}

void matrix_test_routes(const uint8_t link_masks[MATRIX_X_COUNT])
{
    for (unsigned y = 0; y < MATRIX_Y_COUNT; ++y) link_input(y);
    reset_matrix();
    output(DATA, 0);
    output(CLK, 0);
    for (unsigned x = 0; x < MATRIX_X_COUNT; ++x) {
        for (unsigned y = 0; y < MATRIX_Y_COUNT; ++y) {
            if (link_masks[x] & (1U << y)) route(x, y, true);
        }
    }
    gpio_set_direction(CLK, GPIO_MODE_INPUT);
}

void matrix_test_links(unsigned x, uint8_t link_mask)
{
    uint8_t fixture[MATRIX_X_COUNT] = {0};
    fixture[x] = link_mask;
    matrix_test_routes(fixture);
}

int matrix_selftest(void)
{
    for (unsigned y = 0; y < MATRIX_Y_COUNT; ++y) link_input(y);
    output(STB, 0);
    output(RESET, 0);
    output(DATA, 0);
    output(CLK, 0);
    reset_matrix();
    int failures = 0;
    for (int x = 0; x < 16; ++x) {
        int passes = 0;
        for (int a = 0; a < 8; ++a) {
            for (int b = a + 1; b < 8; ++b) {
                gpio_num_t tx = links[a], rx = links[b];
                gpio_set_direction(rx, GPIO_MODE_INPUT);
                gpio_set_pull_mode(rx, GPIO_PULLUP_ONLY);
                output(tx, 0);
                int isolated = sample(rx);
                route(x, a, true);
                route(x, b, true);
                int low = sample(rx);
                gpio_set_level(tx, 1);
                int high = sample(rx);
                gpio_set_level(tx, 0);
                route(x, b, false);
                int opened = sample(rx);
                route(x, b, true);
                int reclosed = sample(rx);
                reset_matrix();
                int reset = sample(rx);
                gpio_set_direction(tx, GPIO_MODE_INPUT);
                gpio_set_pull_mode(rx, GPIO_FLOATING);
                if (isolated == 1 && low == 0 && high == 1 && opened == 1 && reclosed == 0 && reset == 1) {
                    ++passes;
                } else {
                    ++failures;
                    printf("FAIL X%02d Y%d/Y%d idle=%d low=%d high=%d open=%d reclose=%d reset=%d\n",
                           x, a, b, isolated, low, high, opened, reclosed, reset);
                }
            }
        }
        printf("X%02d: %d/28 passed\n", x, passes);
    }
    printf("RESULT: %d/448 passed; %d failed\n", 448 - failures, failures);
    fflush(stdout);
    gpio_set_direction(CLK, GPIO_MODE_INPUT);
    for (unsigned y = 0; y < MATRIX_Y_COUNT; ++y) link_input(y);
    return failures;
}


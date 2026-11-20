#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "driver/gpio.h"
#include "esp_err.h"

enum { MATRIX_Y_COUNT = 8, MATRIX_X_COUNT = 16 };

typedef enum {
    MATRIX_INPUT,
    MATRIX_PULLUP,
    MATRIX_PULLDOWN,
    MATRIX_LOW,
    MATRIX_HIGH,
    MATRIX_PWM,
} matrix_mode_t;

typedef struct {
    uint16_t ports;
    uint8_t links;
} matrix_net_t;

void matrix_boot_reset(void);
bool matrix_has_routes(void);
void matrix_apply(const uint16_t routes[MATRIX_Y_COUNT]);
uint16_t matrix_route_mask(unsigned y);
matrix_net_t matrix_link_net(unsigned y);
bool matrix_links_have_output(uint8_t link_mask);
gpio_num_t matrix_link_gpio(unsigned y);
matrix_mode_t matrix_link_mode(unsigned y);
const char *matrix_mode_name(matrix_mode_t mode);
int matrix_link_level(unsigned y);
esp_err_t matrix_configure_link(unsigned y, matrix_mode_t mode, unsigned hz, unsigned duty);
void matrix_test_links(unsigned x, uint8_t link_mask);
void matrix_test_routes(const uint8_t link_masks[MATRIX_X_COUNT]);
int matrix_selftest(void);

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "sdkconfig.h"

esp_err_t board_save_wifi(const char *ssid, const char *password);
void board_schedule_reboot(void);
bool board_reboot_is_pending(void);
void board_status_json(char *response, size_t capacity);
esp_err_t board_scan_json(char *response, size_t capacity);
void board_matrix_arm(void);
bool board_matrix_is_armed(void);
bool board_matrix_acquire(void);
void board_matrix_release(void);
esp_err_t board_matrix_run(int *failures);

#ifdef CONFIG_AI_BOARD_OUT_CONTROL
bool board_out_grounded(void);
esp_err_t board_set_out_grounded(bool grounded);
#endif

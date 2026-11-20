#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "board.h"
#include "console.h"
#include "control_lease.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "workbench.h"

typedef enum {
    LINE_OK,
    LINE_CANCELLED,
    LINE_TOO_LONG,
} line_result_t;

static line_result_t read_line(char *line, size_t capacity, bool echo)
{
    static bool skip_lf;
    size_t length = 0;
    size_t excess = 0;
    while (true) {
        uint8_t byte;
        if (uart_read_bytes(UART_NUM_0, &byte, 1, portMAX_DELAY) != 1) {
            continue;
        }
        bool drop = skip_lf && byte == '\n';
        skip_lf = false;
        if (drop) continue;

        if (byte == '\r' || byte == '\n') {
            skip_lf = byte == '\r';
            line[length] = '\0';
            putchar('\n');
            return excess ? LINE_TOO_LONG : LINE_OK;
        }
        if (byte == 3) {
            putchar('\n');
            return LINE_CANCELLED;
        }
        if (byte == '\b' || byte == 127) {
            if (excess > 0) {
                --excess;
            } else if (length > 0) {
                --length;
                if (echo) fputs("\b \b", stdout);
            }
        } else if (byte >= 32) {
            if (length < capacity - 1) {
                line[length++] = (char)byte;
                if (echo) putchar(byte);
            } else {
                ++excess;
            }
        }
        fflush(stdout);
    }
}

static bool accept_line(line_result_t result)
{
    if (result == LINE_OK) return true;
    puts(result == LINE_CANCELLED ? "CANCELLED" : "ERROR input too long");
    return false;
}

static bool begin_local_mutation(void)
{
    const control_lease_result_t permit = control_lease_begin_mutation(NULL);
    if (permit == CONTROL_LEASE_OK) {
        if (!board_reboot_is_pending()) return true;
        control_lease_end_mutation();
    }
    puts(permit == CONTROL_LEASE_DENIED ? "ERROR AI controls the board" : "ERROR board busy");
    return false;
}

static void configure_wifi(void)
{
    if (control_lease_status().active) {
        puts("ERROR AI controls the board");
        return;
    }
    char ssid[33];
    puts("WIFI SSID:");
    fflush(stdout);
    if (!accept_line(read_line(ssid, sizeof(ssid), true))) return;

    char password[64];
    puts("WIFI PASSWORD:");
    fflush(stdout);
    line_result_t line = read_line(password, sizeof(password), false);
    const bool allowed = line == LINE_OK && begin_local_mutation();
    esp_err_t saved = allowed ? board_save_wifi(ssid, password) : ESP_ERR_INVALID_ARG;
    for (size_t i = 0; i < sizeof(password); ++i) {
        ((volatile char *)password)[i] = '\0';
    }
    if (!accept_line(line) || !allowed) return;
    if (saved != ESP_OK) {
        control_lease_end_mutation();
        printf("WIFI ERROR %s\n", esp_err_to_name(saved));
        return;
    }
    puts("WIFI SAVED");
    fflush(stdout);
    board_schedule_reboot();
    control_lease_end_mutation();
}

static void run_selftest(void)
{
    int failures;
    esp_err_t result = board_matrix_run(&failures);
    if (result == ESP_OK) {
        printf("SELFTEST passed=%d failed=%d total=448\n", 448 - failures, failures);
    } else {
        printf("SELFTEST ERROR %s\n", esp_err_to_name(result));
    }
}

static void run_capture(const char *form)
{
    char *json = NULL;
    const char *diagnostic = NULL;
    esp_err_t result = workbench_serial_capture(form, &json, &diagnostic);
    if (result == ESP_OK && json) {
        printf("CAPTURE JSON %s\n", json);
    } else {
        printf("CAPTURE ERROR %s\n", diagnostic ? diagnostic : esp_err_to_name(result));
    }
    fflush(stdout);
    free(json);
}

static void console_task(void *context)
{
    puts("UART CONSOLE READY. Type help for commands.");
    while (true) {
        char command[256];
        fputs("ai-board> ", stdout);
        fflush(stdout);
        if (!accept_line(read_line(command, sizeof(command), true))) continue;
        if (strcmp(command, "help") == 0) {
            puts("status | scan | wifi | arm dtr-disconnected | selftest | capture <form>");
            puts("capture a=0&b=1&c=2&d=3&ms=150&trigger=manual");
        } else if (strcmp(command, "status") == 0) {
            char response[512];
            board_status_json(response, sizeof(response));
            puts(response);
        } else if (strcmp(command, "scan") == 0) {
            if (!begin_local_mutation()) continue;
            static char response[3072];
            esp_err_t result = board_scan_json(response, sizeof(response));
            control_lease_end_mutation();
            if (result == ESP_OK) puts(response);
            else printf("SCAN ERROR %s\n", esp_err_to_name(result));
        } else if (strcmp(command, "wifi") == 0) {
            configure_wifi();
        } else if (strcmp(command, "arm dtr-disconnected") == 0) {
            if (!begin_local_mutation()) continue;
            board_matrix_arm();
            control_lease_end_mutation();
            puts("MATRIX ARMED. Remove all BBX loads before selftest.");
        } else if (strcmp(command, "selftest") == 0) {
            if (!begin_local_mutation()) continue;
            run_selftest();
            control_lease_end_mutation();
        } else if (strncmp(command, "capture ", 8) == 0) {
            if (!begin_local_mutation()) continue;
            run_capture(command + 8);
            control_lease_end_mutation();
        } else if (command[0] != '\0') {
            puts("ERROR unknown command. Type help.");
        }
    }
}

esp_err_t board_console_start(void)
{
    esp_err_t result = uart_driver_install(UART_NUM_0, 512, 0, 0, NULL, 0);
    if (result != ESP_OK) return result;
    if (xTaskCreate(console_task, "uart_console", 6144, NULL, 3, NULL) != pdPASS) {
        uart_driver_delete(UART_NUM_0);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

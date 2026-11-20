#include "i2c_device_capture.h"

#include <string.h>

#include "driver/i2c_slave.h"
#include "esp_attr.h"
#include "esp_intr_alloc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "hal/i2c_ll.h"
#include "hal/i2c_periph.h"

enum {
    RX_BUFFER_BYTES = 256,
    TX_BUFFER_BYTES = 32,
    READ_FILL_BYTES = 16,
};

typedef enum {
    CAPTURE_COMPLETE,
    MASTER_REQUESTED_READ,
    REGISTER_BATCH_READY,
} capture_event_t;

typedef struct {
    QueueHandle_t events;
    i2c_device_capture_result_t *result;
    size_t frames;
    size_t capacity;
    size_t register_count;
    i2c_device_register_t registers[I2C_DEVICE_CAPTURE_MAX_REGISTERS];
    i2c_device_capture_frame_t pending_write;
    uint8_t pending_reply[I2C_DEVICE_CAPTURE_MAX_REPLY];
    uint8_t pending_reply_len;
    uint8_t pending_span_count;
    uint8_t pending_span_lengths[I2C_DEVICE_CAPTURE_MAX_REGISTERS];
    bool has_pending_write;
    bool pending_write_applied;
    bool single_reply;
    bool auto_increment;
} capture_context_t;

_Static_assert(I2C_DEVICE_CAPTURE_MAX_REPLY == I2C_LL_GET(FIFO_LEN),
               "Register reply must fit the ESP32-C3 I2C TX FIFO");

static void IRAM_ATTR save_write(i2c_device_capture_frame_t *frame, size_t capacity,
                                const i2c_slave_rx_done_event_data_t *event)
{
    frame->total_bytes = event->length;
    frame->truncated = event->length > capacity;
    frame->bytes = frame->truncated ? capacity : event->length;
    // The driver reuses its callback buffer at the next address phase.
    memcpy(frame->data, event->buffer, frame->bytes);
}

static void IRAM_ATTR finish_previous_read(i2c_device_capture_result_t *result)
{
    if (result->transaction_count) {
        result->transaction[result->transaction_count - 1].read_completed = true;
        result->read_completed = true;
    }
}

static i2c_device_register_t *IRAM_ATTR find_register(capture_context_t *context, uint8_t address)
{
    for (size_t i = 0; i < context->register_count; ++i) {
        if (context->registers[i].address == address) return &context->registers[i];
    }
    return NULL;
}

static void IRAM_ATTR select_register_span(capture_context_t *context, uint8_t start,
                                          const uint8_t *payload, size_t payload_size)
{
    if (payload_size > I2C_DEVICE_CAPTURE_MAX_REPLY) return;
    const size_t limit = payload_size ? payload_size : I2C_DEVICE_CAPTURE_MAX_REPLY;
    i2c_device_register_t *entries[I2C_DEVICE_CAPTURE_MAX_REGISTERS];
    size_t count = 0;
    size_t total = 0;
    for (unsigned address = start; address <= UINT8_MAX && total < limit && count < context->register_count; ++address) {
        i2c_device_register_t *entry = find_register(context, address);
        if (!entry || entry->reply_len > limit - total) break;
        entries[count++] = entry;
        total += entry->reply_len;
    }
    if (payload_size && total != payload_size) return;

    // The full address range and payload lengths are known before any value
    // changes. A read-only selection can use all complete values that fit.
    size_t offset = 0;
    for (size_t i = 0; i < count; ++i) {
        i2c_device_register_t *entry = entries[i];
        const size_t width = entry->reply_len;
        if (payload_size) memcpy(entry->reply, payload + offset, width);
        memcpy(context->pending_reply + offset, entry->reply, width);
        context->pending_span_lengths[i] = width;
        offset += width;
    }
    context->pending_reply_len = total;
    context->pending_span_count = count;
    context->pending_write_applied = payload_size != 0;
}

static void IRAM_ATTR receive_register_write(capture_context_t *context,
                                             const i2c_slave_rx_done_event_data_t *event)
{
    save_write(&context->pending_write, context->capacity, event);
    context->has_pending_write = true;
    context->pending_write_applied = false;
    context->pending_reply_len = 0;
    context->pending_span_count = 0;
    const size_t payload_size = event->length - 1;
    if (context->auto_increment) {
        select_register_span(context, event->buffer[0], event->buffer + 1, payload_size);
        return;
    }

    if (context->single_reply && payload_size) return;
    i2c_device_register_t *entry = find_register(context, event->buffer[0]);
    if (!entry) return;
    if (payload_size && payload_size <= I2C_DEVICE_CAPTURE_MAX_REPLY) {
        // Use the hardware event directly; the display limit is separate.
        memcpy(entry->reply, event->buffer + 1, payload_size);
        entry->reply_len = payload_size;
        context->pending_write_applied = true;
    }
    memcpy(context->pending_reply, entry->reply, entry->reply_len);
    context->pending_reply_len = entry->reply_len;
}

static bool IRAM_ATTR write_received(i2c_slave_dev_handle_t handle,
                                    const i2c_slave_rx_done_event_data_t *event, void *argument)
{
    (void)handle;
    capture_context_t *context = argument;
    if (!context || !event->length) return false;

    i2c_device_capture_result_t *result = context->result;
    if (context->register_count) {
        // A subsequent write phase proves the preceding read phase ended,
        // even when the task never got to see an idle bus between exchanges.
        finish_previous_read(result);
        if (result->transaction_count < context->frames) receive_register_write(context, event);
        return false;
    }
    if (result->frame_count >= context->frames) return false;
    save_write(&result->frame[result->frame_count++], context->capacity, event);
    if (result->frame_count != context->frames) return false;
    const capture_event_t complete = CAPTURE_COMPLETE;
    BaseType_t wake = pdFALSE;
    xQueueSendFromISR(context->events, &complete, &wake);
    return wake == pdTRUE;
}

static bool IRAM_ATTR serve_register_read(capture_context_t *context)
{
    i2c_device_capture_result_t *result = context->result;
    finish_previous_read(result);
    ++result->read_requests;

    // IDF delivers the pending RX callback before the matching read address
    // callback and keeps SCL stretched while TX is loaded.
    const size_t loaded = context->has_pending_write ? context->pending_reply_len : 0;
    i2c_dev_t *hardware = I2C_LL_GET_HW(I2C_NUM_0);
    i2c_ll_txfifo_rst(hardware);   // Preserve the active I2C FSM.
    for (size_t i = 0; i < I2C_DEVICE_CAPTURE_MAX_REPLY; ++i) {
        const uint8_t value = i < loaded ? context->pending_reply[i] : 0xff;
        i2c_ll_write_txfifo(hardware, &value, 1);
    }
    i2c_ll_slave_clear_stretch(hardware);

    if (result->transaction_count >= context->frames) {
        context->has_pending_write = false;
        return false;
    }
    if (context->single_reply && !loaded) return false;
    i2c_device_capture_transaction_t *transaction = &result->transaction[result->transaction_count++];
    transaction->frame_index = SIZE_MAX;
    transaction->matched = loaded != 0;
    transaction->reply_loaded = loaded;
    if (loaded) memcpy(transaction->reply, context->pending_reply, loaded);
    if (context->has_pending_write) {
        transaction->frame_index = result->frame_count;
        result->frame[result->frame_count++] = context->pending_write;
        transaction->write_applied = context->pending_write_applied;
        transaction->span_count = context->pending_span_count;
        memcpy(transaction->span_lengths, context->pending_span_lengths, transaction->span_count);
        context->has_pending_write = false;
    }
    result->reply_loaded += loaded;
    result->read_completed = false;
    if (result->transaction_count != context->frames) return false;
    const capture_event_t ready = REGISTER_BATCH_READY;
    BaseType_t wake = pdFALSE;
    xQueueSendFromISR(context->events, &ready, &wake);
    return wake == pdTRUE;
}

static bool IRAM_ATTR read_requested(i2c_slave_dev_handle_t handle,
                                    const i2c_slave_request_event_data_t *event, void *argument)
{
    (void)handle;
    (void)event;
    capture_context_t *context = argument;
    if (!context) return false;
    if (context->register_count) return serve_register_read(context);
    const capture_event_t read = MASTER_REQUESTED_READ;
    BaseType_t wake = pdFALSE;
    xQueueSendFromISR(context->events, &read, &wake);
    return wake == pdTRUE;
}

static void IRAM_ATTR discard_unread_reply(void *argument)
{
    i2c_dev_t *hardware = argument;
    i2c_ll_clear_intr_mask(hardware, I2C_NACK_INT_ENA_M);
    // The master ended its read with NACK. Clear leftover bytes and rearm
    // the transmitter before the next read address can prefetch one.
    i2c_ll_txfifo_rst(hardware);
    i2c_ll_start_trans(hardware);
}

static bool valid_registers(const i2c_device_capture_options_t *options)
{
    if (options->auto_increment && (!options->register_count || options->reply_len)) return false;
    if (options->reply_len) {
        return !options->register_count && options->reply && options->frames == 1 &&
               options->reply_len <= I2C_DEVICE_CAPTURE_MAX_REPLY;
    }
    if (!options->register_count) return true;
    if (!options->registers || options->register_count > I2C_DEVICE_CAPTURE_MAX_REGISTERS) return false;
    for (size_t i = 0; i < options->register_count; ++i) {
        const i2c_device_register_t *entry = &options->registers[i];
        if (!entry->reply_len || entry->reply_len > I2C_DEVICE_CAPTURE_MAX_REPLY) return false;
        for (size_t previous = 0; previous < i; ++previous) {
            if (options->registers[previous].address == entry->address) return false;
        }
    }
    return true;
}

static bool valid_options(const i2c_device_capture_options_t *options)
{
    return options && GPIO_IS_VALID_OUTPUT_GPIO(options->sda) &&
           GPIO_IS_VALID_OUTPUT_GPIO(options->scl) && options->sda != options->scl &&
           options->address >= 0x08 && options->address <= 0x77 &&
           options->frames && options->frames <= I2C_DEVICE_CAPTURE_MAX_FRAMES &&
           options->capacity && options->capacity <= I2C_DEVICE_CAPTURE_MAX_BYTES &&
           options->timeout_ms && options->timeout_ms <= I2C_DEVICE_CAPTURE_MAX_TIMEOUT_MS &&
           valid_registers(options);
}

static void release_pins(const i2c_device_capture_options_t *options)
{
    gpio_set_direction(options->sda, GPIO_MODE_INPUT);
    gpio_set_direction(options->scl, GPIO_MODE_INPUT);
    gpio_set_pull_mode(options->sda, GPIO_FLOATING);
    gpio_set_pull_mode(options->scl, GPIO_FLOATING);
}

esp_err_t i2c_device_capture_run(const i2c_device_capture_options_t *options,
                                 i2c_device_capture_result_t *result)
{
    if (!result) return ESP_ERR_INVALID_ARG;
    memset(result, 0, sizeof(*result));
    if (!valid_options(options)) return ESP_ERR_INVALID_ARG;

    capture_context_t context = {
        .events = xQueueCreate(4, sizeof(capture_event_t)),
        .result = result,
        .frames = options->frames,
        .capacity = options->capacity,
        .register_count = options->register_count,
        .single_reply = options->reply_len != 0,
        .auto_increment = options->auto_increment,
    };
    if (!context.events) return ESP_ERR_NO_MEM;
    if (context.single_reply) {
        context.register_count = 1;
        context.registers[0].address = options->register_address;
        context.registers[0].reply_len = options->reply_len;
        memcpy(context.registers[0].reply, options->reply, options->reply_len);
    } else if (context.register_count) {
        memcpy(context.registers, options->registers, context.register_count * sizeof(context.registers[0]));
    }

    const i2c_slave_config_t config = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = options->sda,
        .scl_io_num = options->scl,
        .clk_source = I2C_CLK_SRC_XTAL,
        .send_buf_depth = TX_BUFFER_BYTES,
        .receive_buf_depth = RX_BUFFER_BYTES,
        .slave_addr = options->address,
        .addr_bit_len = I2C_ADDR_BIT_LEN_7,
        .intr_priority = 2,
        .flags.enable_internal_pullup = options->internal_pullup,
    };
    i2c_slave_dev_handle_t slave = NULL;
    intr_handle_t read_end_irq = NULL;
    i2c_dev_t *hardware = I2C_LL_GET_HW(I2C_NUM_0);
    esp_err_t err = i2c_new_slave_device(&config, &slave);
    if (err != ESP_OK) goto cleanup;

    if (context.register_count) {
        int flags = ESP_INTR_FLAG_SHARED | ESP_INTR_FLAG_LOWMED | ESP_INTR_FLAG_LEVEL2;
#if CONFIG_I2C_ISR_IRAM_SAFE
        flags |= ESP_INTR_FLAG_IRAM;
#endif
        // IDF 6.1 prepends shared handlers. Install after the slave so we
        // service NACK before its ISR can clear a simultaneous STOP status.
        err = esp_intr_alloc_intrstatus(i2c_periph_signal[I2C_NUM_0].irq, flags,
                                       (uint32_t)i2c_ll_get_interrupt_status_reg(hardware),
                                       I2C_NACK_INT_ENA_M, discard_unread_reply,
                                       (void *)hardware, &read_end_irq);
        if (err != ESP_OK) goto cleanup;
        i2c_ll_clear_intr_mask(hardware, I2C_NACK_INT_ENA_M);
        i2c_ll_enable_intr_mask(hardware, I2C_NACK_INT_ENA_M);
    }

    const i2c_slave_event_callbacks_t callbacks = {
        .on_receive = write_received,
        .on_request = read_requested,
    };
    err = i2c_slave_register_event_callbacks(slave, &callbacks, &context);
    if (err != ESP_OK) goto cleanup;

    const TickType_t duration = pdMS_TO_TICKS(options->timeout_ms) + 1;
    const TickType_t start = xTaskGetTickCount();
    if (options->armed) options->armed(options->armed_context);

    for (;;) {
        const TickType_t elapsed = xTaskGetTickCount() - start;
        capture_event_t event;
        if (elapsed >= duration ||
            xQueueReceive(context.events, &event, duration - elapsed) != pdTRUE ||
            event == CAPTURE_COMPLETE) {
            break;
        }

        if (event == REGISTER_BATCH_READY) {
            // Serve successive exchanges in the ISR. Only the final read
            // needs the task to wait for bus idle before releasing the pins.
            while (i2c_ll_is_bus_busy(hardware) && xTaskGetTickCount() - start < duration) {
                vTaskDelay(1);
            }
            break;
        }

        // The driver stretches SCL at a master read request. Release it
        // with an all-high filler while continuing to capture only writes.
        uint8_t filler[READ_FILL_BYTES];
        memset(filler, 0xff, sizeof(filler));
        uint32_t written = 0;
        err = i2c_slave_write(slave, filler, sizeof(filler), &written, 0);
        if (err != ESP_OK) break;
    }

cleanup:
    if (read_end_irq) {
        i2c_ll_disable_intr_mask(hardware, I2C_NACK_INT_ENA_M);
        i2c_ll_clear_intr_mask(hardware, I2C_NACK_INT_ENA_M);
        const esp_err_t released = esp_intr_free(read_end_irq);
        if (err == ESP_OK) err = released;
    }
    if (slave) {
        // Detach callbacks before reclaiming their context, including on
        // a driver teardown error.
        const i2c_slave_event_callbacks_t disabled = {0};
        i2c_slave_register_event_callbacks(slave, &disabled, NULL);
        if (context.register_count && !i2c_ll_is_bus_busy(hardware)) {
            finish_previous_read(result);
        }
        const esp_err_t released = i2c_del_slave_device(slave);
        if (err == ESP_OK) err = released;
    }
    release_pins(options);
    vQueueDelete(context.events);
    if (context.register_count) {
        if (result->transaction_count < options->frames && context.has_pending_write &&
            result->frame_count < options->frames) {
            result->frame[result->frame_count++] = context.pending_write;
        }
        result->timed_out = err == ESP_OK && (result->transaction_count < options->frames || !result->read_completed);
    } else {
        result->timed_out = err == ESP_OK && result->frame_count < options->frames;
    }
    return err;
}

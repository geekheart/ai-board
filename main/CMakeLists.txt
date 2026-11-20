idf_component_register(SRCS "main.c" "matrix.c" "console.c" "capture.c" "protocol.c" "workbench.c" "web.c" "spi_benchmark.c" "spi_stimulus.c" "capture_dma.c"
    "spi_device_capture.c" "spi_reply_probe.c" "spi_i2s_host.c"
    "i2c_device_capture.c" "uart_batch_capture.c" "protocol_receive_fixture.c" "control_lease.c" "board_lua.c" "script_store.c" "web_store.c" "board_events.c"
    INCLUDE_DIRS "."
    REQUIRES freertos esp_driver_uart esp_driver_gpio esp_driver_i2c esp_driver_ledc esp_driver_spi esp_driver_rmt esp_driver_i2s
             esp_rom esp_wifi esp_event esp_netif esp_http_server app_update nvs_flash
             esp_hw_support esp_timer
    PRIV_REQUIRES espressif__lua joltwallet__littlefs esp_partition mbedtls)

# Leave room on the Lua task's native stack for deep parses and driver calls.
idf_component_get_property(lua_runtime_component espressif__lua COMPONENT_LIB)
target_compile_definitions(${lua_runtime_component} PRIVATE LUAI_MAXCCALLS=64)

# The ESP-IDF 6.1 I2S driver owns the DMA buffers. The fixture terminates its
# first TX descriptor so it emits one SPI transaction.
idf_build_get_property(idf_path IDF_PATH)
target_include_directories(${COMPONENT_LIB} PRIVATE "${idf_path}/components/esp_driver_i2s")

idf_build_get_property(web_python PYTHON)
foreach(asset IN ITEMS index.html style.css app.js)
    set(source "${CMAKE_CURRENT_SOURCE_DIR}/../web/${asset}")
    set(packed "${CMAKE_CURRENT_BINARY_DIR}/${asset}.gz")
    add_custom_command(
        OUTPUT "${packed}"
        COMMAND "${web_python}" "${CMAKE_CURRENT_SOURCE_DIR}/pack_web.py" "${source}" "${packed}"
        DEPENDS "${source}" "${CMAKE_CURRENT_SOURCE_DIR}/pack_web.py"
        VERBATIM)
    target_add_binary_data(${COMPONENT_LIB} "${packed}" BINARY)
endforeach()

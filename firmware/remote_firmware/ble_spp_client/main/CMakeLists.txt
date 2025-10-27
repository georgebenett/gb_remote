file(GLOB_RECURSE UI_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/ui/*.c"
)

idf_component_register(
    SRCS
        "src/button.c"
        "src/sleep.c"
        "src/ble_spp_client.c"
        "src/main.c"
        "src/adc.c"
        "src/lcd.c"
        "src/vesc_config.c"
        "src/ui_updater.c"
        "src/battery.c"
        "src/usb_serial_handler.c"
        "src/level_assistant.c"
        ${UI_SOURCES}
    INCLUDE_DIRS
        "."
        "ui"
        "include"
    REQUIRES driver nvs_flash bt esp_adc spi_flash esp_lcd lvgl
)

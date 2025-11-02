# Select UI folder based on target configuration
if(CONFIG_TARGET_DUAL_THROTTLE)
    set(UI_DIR "ui_dual_throttle")
elseif(CONFIG_TARGET_LITE)
    set(UI_DIR "ui_lite")
else()
    # Fallback: read sdkconfig if CONFIG variables not available yet
    set(SDKCONFIG_PATH "${CMAKE_SOURCE_DIR}/sdkconfig")
    if(EXISTS "${SDKCONFIG_PATH}")
        file(READ "${SDKCONFIG_PATH}" SDKCONFIG_CONTENT)
        if(SDKCONFIG_CONTENT MATCHES "(^|[\r\n])[^#]*CONFIG_TARGET_DUAL_THROTTLE=y")
            set(UI_DIR "ui_dual_throttle")
        elseif(SDKCONFIG_CONTENT MATCHES "(^|[\r\n])[^#]*CONFIG_TARGET_LITE=y")
            set(UI_DIR "ui_lite")
        endif()
    endif()
    
    if(UI_DIR STREQUAL "")
        message(FATAL_ERROR "No target configuration selected. Please set CONFIG_TARGET_DUAL_THROTTLE or CONFIG_TARGET_LITE in sdkconfig.\nRun: idf.py reconfigure or use ./dual_throttle_build.sh / ./lite_build.sh")
    endif()
endif()

file(GLOB_RECURSE UI_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/${UI_DIR}/*.c"
)

idf_component_register(
    SRCS
        "button.c"
        "power.c"
        "ble.c"
        "main.c"
        "throttle.c"
        "lcd.c"
        "vesc_config.c"
        "ui_updater.c"
        "battery.c"
        "usb_serial_handler.c"
        "level_assistant.c"
        "viber.c"
        ${UI_SOURCES}
    INCLUDE_DIRS
        "."
        "${UI_DIR}"
        "ui_dual_throttle"
        "ui_lite"
    REQUIRES driver nvs_flash bt esp_adc spi_flash esp_lcd lvgl
)

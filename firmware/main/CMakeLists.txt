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

    # IDF's early requirements pass runs this file without CONFIG_* values.
    if(NOT UI_DIR AND NOT CMAKE_BUILD_EARLY_EXPANSION)
        message(FATAL_ERROR "No target configuration selected. Please set CONFIG_TARGET_DUAL_THROTTLE or CONFIG_TARGET_LITE in sdkconfig.\nUse ./build.sh <lite|dual_throttle>")
    endif()
endif()

# CONFIGURE_DEPENDS re-globs when EEZ adds or drops a file; it is not allowed
# in the early (script mode) pass, which only needs the requirements.
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    file(GLOB_RECURSE UI_SOURCES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/${UI_DIR}/*.c"
    )
endif()

idf_component_register(
    SRCS
        "app_init.c"
        "battery.c"
        "ble.c"
        "button.c"
        "lcd.c"
        "main.c"
        "power.c"
        "rx_ota.c"
        "snake.c"
        "throttle.c"
        "throttle_math.c"
        "ui_updater.c"
        "usb_serial.c"
        "settings.c"
        "settings_rules.c"
        "viber.c"
        ${UI_SOURCES}
    INCLUDE_DIRS
        "."
        "${UI_DIR}"
    REQUIRES driver nvs_flash bt esp_adc spi_flash esp_lcd lvgl espcoredump
             esp_partition esp_app_format bootloader_support
)

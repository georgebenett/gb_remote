#ifndef USB_SERIAL_HANDLER_H
#define USB_SERIAL_HANDLER_H

#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Target-specific USB CDC configurations
    // ESP32-S3 specific USB CDC settings
    #define USB_CDC_ENABLED 1
    #define USB_CDC_USE_PRIMARY_CONSOLE 1
    #define USB_CDC_USE_SECONDARY_CONSOLE 0
    #define USB_CDC_INIT_DELAY_MS 100
    #define USB_CDC_TASK_DELAY_MS 50
    #define USB_CDC_BUFFER_SIZE 1024
#else
    // Default settings for other targets
    #define USB_CDC_ENABLED 0
    #define USB_CDC_USE_PRIMARY_CONSOLE 0
    #define USB_CDC_USE_SECONDARY_CONSOLE 0
    #define USB_CDC_INIT_DELAY_MS 100
    #define USB_CDC_TASK_DELAY_MS 50
    #define USB_CDC_BUFFER_SIZE 1024
#endif

// Command types that can be received via USB
typedef enum {
    CMD_RESET_ODOMETER = 0,
    CMD_SET_MOTOR_PULLEY,
    CMD_SET_WHEEL_PULLEY,
    CMD_SET_WHEEL_SIZE,
    CMD_SET_MOTOR_POLES,
    CMD_GET_CONFIG,
    CMD_CALIBRATE_THROTTLE,
    CMD_GET_CALIBRATION,
    CMD_GET_FIRMWARE_VERSION,
    CMD_SET_SPEED_UNIT_KMH,
    CMD_SET_SPEED_UNIT_MPH,
    CMD_HELP,
    CMD_UNKNOWN
} usb_command_t;

// Function prototypes
void usb_serial_init(void);
void usb_serial_start_task(void);
void usb_serial_process_command(const char* command);

// External variables and functions that will be called from USB handler
extern bool is_connect;
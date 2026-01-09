# GB Remote

<div align="center">
  <img src="https://github.com/georgebenett/gb_remote/blob/main/gb_remotes.jpg" width="50%" alt="GB Controller">
</div>

A firmware open-source hand controller for electric skateboards built with the ESP32S3. This project provides a complete wireless control solution with real-time telemetry monitoring, featuring a custom BLE protocol that communicates with VESC motor controllers and Jiabaida/Kaly BMS systems.

This repository contains the **remote controller firmware**. The complete system also includes:
- **Receiver Firmware** (`gb_receiver`): The BLE server firmware that runs on the receiver

## Key Features

### Intuitive Control Interface
- **Analog Throttle Control**: High-precision ADC-based throttle input with automatic calibration
- **Dual Throttle Support**: Separate throttle and brake inputs on dual throttle variant with neutral position
- **Smart Button Interface**: Multi-function button with long-press and double-press detection
- **Haptic Feedback**: Vibration patterns for connection status, alerts, and user feedback
- **Sleep Management**: Automatic power management with inactivity detection and power modes
- **Power Management**: Soft latching hardware where power consumption while "sleeping is 1uA"

### Modern UI
- **LVGL Graphics Library**: Beautiful, responsive user interface built with EEZ Studio (open-source)
- **Real-time Speed Display**: Large, easy-to-read speedometer with km/h and mi/h display
- **Dual Battery Monitoring**: Separate battery indicators for controller and skateboard
- **Connection Status**: Visual feedback for BLE connection quality
- **Trip Distance Tracking**: Built-in odometer with persistent storage
- **Adjustable Backlight**: Configurable display brightness (1-100%)

### Comprehensive Telemetry
- **VESC Integration**: Real-time data from VESC
- **BMS Support**: Full battery management system monitoring with cell-level voltage tracking
  - **Compatible BMS Systems**: Jiabaida and Kaly BMS telemetry support
  - **Cell Voltage Monitoring**: Individual cell voltage tracking for up to 16 cells
  - **Capacity Tracking**: Real-time remaining and nominal capacity monitoring
- **Custom BLE Protocol**: Optimized data transmission for low latency and high reliability
- **Automatic Parameter Configuration**: Motor pulley, wheel diameter, and throttle settings are automatically retrieved by the receiver from the vesc


## Hardware Variants

The firmware supports two hardware variants:

### GB Remote Lite
- **Display**: 240×320 pixel TFT LCD
- **Throttle**: Single analog throttle with inversion support
- **Use Case**: Standard single-throttle control

### GB Remote Dual Throttle
- **Display**: 172×320 pixel TFT LCD
- **Throttle**: Dual analog inputs (throttle + brake)
- **Use Case**: Separate throttle and brake control with neutral position

Both variants share the same core firmware with target-specific configurations.

## Technical Specifications

### Hardware Platform
- **MCU**: ESP32 S3
- **Display**: TFT LCD ST7789 (resolution varies by variant)
- **Input**: Analog throttle with ADC calibration (dual inputs on dual throttle variant)
- **Connectivity**: Bluetooth Low Energy (BLE) SPP client
- **Power**: Built-in battery monitoring and power management
- **Haptic Feedback**: Vibration motor for user notifications

### Software Architecture
- **RTOS**: FreeRTOS for real-time task management
- **Graphics**: LVGL 8.3.6 with EEZ Studio UI
- **Communication**: Custom BLE SPP protocol for VESC/BMS data
- **Storage**: NVS flash for configuration and trip data
- **Power Management**: Advanced power modes and soft latching circuit

## Communication Protocol

### BLE SPP Protocol
- **Service UUID**: `0xABF0`
- **Data Characteristic**: `0xABF2`
- **Protocol**: Custom BLE SPP for low-latency communication with receiver
- **Data Exchange**: Throttle/brake commands from remote, VESC and BMS telemetry from receiver

### USB Serial Binary Protocol
The remote communicates with the configuration tool via a binary protocol over USB Serial (USB CDC):

- **Packet-based**: Binary protocol with CRC-16-CCITT error checking
- **Real-time Streaming**: Configurable data streaming (1-100 Hz) for monitoring and debugging
- **Commands**: Firmware version, configuration management, calibration, odometer control, BLE trim adjustment
- **Documentation**: See [`firmware/docs/USB_SERIAL_PROTOCOL.md`](firmware/docs/USB_SERIAL_PROTOCOL.md) for complete protocol specification


## Project Structure

This repository contains the **remote controller firmware**. The complete GB Remote system consists of:

1. **Remote Controller** (this repository): ESP32S3-based hand controller with display and throttle
2. **Receiver Firmware** (`gb_receiver`): BLE server firmware for the skateboard/receiver side that interfaces with VESC and BMS

## Getting Started

### Prerequisites
- ESP-IDF development environment

### Build & Flash

The project includes build scripts for easy compilation:

- **Lite variant**: `./lite_build.sh` - Builds for 240×320 display with single throttle
- **Dual Throttle variant**: `./dual_throttle_build.sh` - Builds for 172×320 display with dual throttle/brake
- **Release build**: `./build_and_release.sh` - Builds both variants for release

After building, flash using ESP-IDF:
```bash
idf.py flash monitor
```

Or use the [Configuration Tool](https://gbengineering.se/config-tool/) for easy firmware updates.

## Configuration Tool

**Easy Configuration via Web Interface**

Configure your GB Remote controller easily using our online configuration tool hosted on the GB Engineering website:

**[GB Remote Configuration Tool](https://gbengineering.se/config-tool/)**

The configuration tool provides a comprehensive web-based interface for setting up and managing your GB Remote system.

### Features:
- **USB Serial Connection**: Connect directly via USB cable
- **Real-time Configuration**: Adjust settings without recompiling firmware
- **Automatic Firmware Updates**: The configuration tool automatically checks for firmware updates every time the remote is connected. Update with a simple button confirmation
- **Throttle Calibration**: Calibrate throttle and brake inputs with guided calibration process
- **Speed Unit Toggle**: Switch between km/h and mph display units
- **Backlight Control**: Adjustable display brightness (0-100%)
- **BLE Trim Adjustment**: Fine-tune BLE output offset for precise throttle control
- **Odometer Management**: Reset trip distance counter
- **Real-time Data Streaming**: Monitor live telemetry data at configurable rates (1-100 Hz)
- **Status Monitoring**: Real-time device status and logs

### Quick Setup:
1. Flash the firmware to your Remote (and Receiver if needed)
2. Connect your controller via USB cable
3. Open the [Configuration Tool](https://gbengineering.se/config-tool/)
4. Click "Connect to Remote" to establish USB serial connection
5. Configure your settings and calibrate throttle


## BMS Compatibility

This controller is specifically designed to work with popular electric skateboard BMS systems:

- **Jaiabaida BMS**: Full telemetry support including cell voltages and capacity data
- **Kaly BMS**: Complete compatibility with all monitoring features

The receiver firmware (`gb_receiver`) handles the BMS communication and forwards the telemetry data to the remote controller via BLE.


## Contributing

This project is open source and welcomes contributions! Whether you're fixing bugs, adding features, or improving documentation, your help is appreciated.

## License

**Creative Commons Attribution-NonCommercial 4.0 International (CC BY-NC 4.0)**

This work is licensed under the Creative Commons Attribution-NonCommercial 4.0 International License. To view a copy of this license, visit [http://creativecommons.org/licenses/by-nc-nd/4.0/](http://creativecommons.org/licenses/by-nc-nd/4.0/).

### What this means:

**You are free to:**
- Share - copy and redistribute the material in any medium or format
- Adapt - remix, transform, and build upon the material
- Use for personal, educational, and research purposes

**You may NOT:**
- Use for commercial purposes (selling, commercial distribution, etc.)
- Create derivative works (modify and redistribute)
- Use without proper attribution to the original author

### Commercial Use:
For commercial use, licensing, or commercial distribution of this project, please contact the author for licensing terms and fees.


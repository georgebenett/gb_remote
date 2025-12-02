# GB Remote

<div align="center">
  <img src="https://github.com/georgebenett/gb_remote/blob/main/gb_remotes.jpg" width="50%" alt="GB Controller">
</div>

A sophisticated, open-source hand controller for electric skateboards built with the ESP32S3. This project provides a complete wireless control solution with real-time telemetry monitoring, featuring a custom BLE SPP client that communicates with VESC motor controllers and BMS systems.

## Key Features

### Intuitive Control Interface
- **Analog Throttle Control**: High-precision ADC-based throttle input with automatic calibration
- **Smart Button Interface**: Multi-function button with long-press and double-press detection
- **Sleep Management**: Automatic power management with inactivity detection and power modes
- **Power Management**: Soft latching hardware where power consumption while "sleeping is 1uA"

### Modern UI
- **LVGL Graphics Library**: Beautiful, responsive user interface built with SquareLine Studio
- **Real-time Speed Display**: Large, easy-to-read speedometer with km/h and mi/h display
- **Dual Battery Monitoring**: Separate battery indicators for controller and skateboard
- **Connection Status**: Visual feedback for BLE connection quality
- **Trip Distance Tracking**: Built-in odometer with persistent storage
- **Adjustable Backlight**: Configurable display brightness (1-100%) with fade effects

### Comprehensive Telemetry
- **VESC Integration**: Real-time motor data including RPM, current, temperature, and voltage
- **BMS Support**: Full battery management system monitoring with cell-level voltage tracking
  - **Compatible BMS Systems**: Jaiabaida and Kaly BMS telemetry support
  - **Cell Voltage Monitoring**: Individual cell voltage tracking for up to 16 cells
  - **Capacity Tracking**: Real-time remaining and nominal capacity monitoring
- **Custom BLE Protocol**: Optimized data transmission for low latency and high reliability
- **Configurable Parameters**: Adjustable motor pulley, wheel diameter, and throttle settings

### Advanced Configuration
- **VESC Parameter Tuning**: Configurable motor poles, pulley ratios, and wheel diameter
- **Throttle Calibration**: Automatic ADC calibration for precise control
- **Persistent Settings**: NVS storage for configuration persistence across reboots
- **Invertible Controls**: Support for inverted throttle direction

## Technical Specifications

### Hardware Platform
- **MCU**: ESP32 S3
- **Display**: TFT LCD ST7789
- **Input**: Analog throttle with ADC calibration
- **Connectivity**: Bluetooth Low Energy (BLE) SPP client
- **Power**: Built-in battery monitoring and power management

### Software Architecture
- **RTOS**: FreeRTOS for real-time task management
- **Graphics**: LVGL 8.3.6 with EEZ Studio UI
- **Communication**: Custom BLE SPP protocol for VESC/BMS data
- **Storage**: NVS flash for configuration and trip data
- **Power Management**: Advanced power modes and soft latching circuit

## Communication Protocol

### BLE SPP Service
- **Service UUID**: `0xABF0`
- **Data Characteristic**: `0xABF2`
- **VESC Data Packet**: 14 bytes (temperature, current, RPM, voltage)
- **BMS Data Packet**: 41 bytes (voltage, current, capacity, cell voltages)
  - **Compatible BMS**: Jaiabaida and Kaly BMS systems
  - **Cell Monitoring**: Up to 16 individual cell voltages
  - **Capacity Data**: Remaining and nominal capacity tracking

## Getting Started

### Prerequisites
- ESP-IDF development environment

### Build & Flash
- Use the build scripts to build for different target (lite or dual trigger)

## Configuration Tool

**Easy Configuration via Web Interface**

Configure your GB Remote Lite controller easily using our online configuration tool:

**[GB Remote Lite Config Tool](https://georgebenett.github.io/gb_config_tool/)**

### Features:
- **USB Serial Connection**: Connect directly via USB cable
- **Real-time Configuration**: Adjust settings without recompiling firmware
- **Firmware Updates**: Easy firmware flashing and updates
- **Throttle Calibration**: Can calibrate throttle and brake
- **Parameter Management**: Configure motor poles, pulley ratios, wheel diameter
- **Status Monitoring**: Real-time device status and logs

### Quick Setup:
1. Flash the firmware to your Remote
2. Connect your controller via USB cable
3. Open the [Config Tool](https://georgebenett.github.io/gb_remote_tool/)
4. Click "Connect to Remote" to establish connection
5. Configure your settings and calibrate throttle
6. Pair with your electric skateboard's BLE server

### Configuration
1. Flash the firmware to your remote
2. Use the [Flashing Tool](https://georgebenett.github.io/gb_flashing_tool/) for easy flashing
3. Calibrate the throttle input on first use
4. Configure VESC parameters in the settings
5. Pair with your electric skateboard's BLE server

## Use Cases

- **Electric Skateboards**: Primary use case with VESC motor controllers
- **Electric Scooters**: Compatible with similar motor control systems
- **DIY Electric Vehicles**: Customizable for various electric vehicle applications
- **Research & Development**: Open-source platform for electric vehicle control research

## BMS Compatibility

This controller is specifically designed to work with popular electric skateboard BMS systems:

- **Jaiabaida BMS**: Full telemetry support including cell voltages and capacity data
- **Kaly BMS**: Complete compatibility with all monitoring features
- **Custom BMS**: Extensible protocol for other BMS systems


## Contributing

This project is open source and welcomes contributions! Whether you're fixing bugs, adding features, or improving documentation, your help is appreciated.

## License

**Creative Commons Attribution-NonCommercial 4.0 International (CC BY-NC 4.0)**

This work is licensed under the Creative Commons Attribution-NonCommercial 4.0 International License. To view a copy of this license, visit [http://creativecommons.org/licenses/by-nc-nd/4.0/](http://creativecommons.org/licenses/by-nc-nd/4.0/) or send a letter to Creative Commons, PO Box 1866, Mountain View, CA 94042, USA.

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


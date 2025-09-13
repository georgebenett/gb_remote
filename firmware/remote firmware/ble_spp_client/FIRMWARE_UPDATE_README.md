# Firmware Update Functionality

This document describes the firmware update functionality that has been added to the ESP32 Hand Controller system.

## Overview

The firmware update feature allows users to check for, download, and flash firmware updates directly from the ESP32 Controller GUI tool. The system compares the current firmware version running on the device with the latest available version from a remote repository, and provides one-click flashing functionality.

## Components

### 1. ESP32 Firmware Changes

#### New Command: `get_firmware_version`
- **Location**: `main/usb_serial_handler.c`
- **Function**: Returns current firmware version and build information
- **Output**:
  ```
  Firmware version: 1.0.0
  Build date: Dec 15 2024 14:30:25
  Target: esp32c3
  IDF version: v5.1.2
  ```

#### Configuration Display Update
- Added firmware version to the `get_config` command output
- Version is displayed in the "Current Configuration" section

### 2. Python GUI Changes

#### New Button: "Check Firmware Update"
- **Location**: Action buttons section in the main GUI
- **Function**: Initiates firmware version check and update process

#### New Features:
1. **Version Detection**: Automatically queries the connected ESP32 for current firmware version
2. **Online Update Check**: Compares current version with latest available version
3. **Update Notification**: Shows dialog when updates are available
4. **Firmware Download**: Downloads latest firmware binary from GitHub releases
5. **One-Click Flashing**: Flashes firmware directly to ESP32 using esptool
6. **Progress Monitoring**: Real-time flashing progress display

#### Dependencies Added:
- `requests`: For HTTP requests to check for updates
- `packaging`: For semantic version comparison
- `esptool`: For ESP32 firmware flashing (installed via apt)

### 3. Firmware Flashing Section

#### New GUI Section: "Firmware Flashing"
- **ESP-IDF Path**: Auto-detects esptool installation or ESP-IDF path
- **Download Latest Firmware**: Downloads latest release from GitHub
- **Flash Firmware**: Flashes downloaded firmware to ESP32
- **Select Firmware File**: Choose custom firmware file
- **Firmware File Path**: Shows selected firmware file

#### Flashing Features:
1. **Auto-Detection**: Automatically finds esptool in PATH or ESP-IDF installation
2. **Download Integration**: Downloads firmware directly from GitHub releases
3. **Correct Flash Offset**: Uses 0x10000 (64KB) offset for ESP32 application firmware
4. **Progress Display**: Real-time flashing progress in response area
5. **Error Handling**: Comprehensive error handling and user feedback
6. **Safety Checks**: Confirmation dialogs before flashing
7. **Port Management**: Automatically disconnects serial port during flashing

## Usage

### 1. Check for Updates
1. Connect to your ESP32 device using the GUI
2. Click the "Check Firmware Update" button
3. The system will:
   - Query the device for current firmware version
   - Check online for the latest version
   - Display comparison results

### 2. Download and Flash Firmware
When an update is available or you want to flash firmware:

#### Option A: One-Click Update
1. Click "Download Latest Firmware" to get the latest release
2. Click "Flash Firmware" to flash the downloaded firmware
3. Confirm the flashing operation in the dialog
4. Wait for the flashing to complete

#### Option B: Custom Firmware
1. Download firmware manually from GitHub releases
2. Click "Select Firmware File" to choose the .bin file
3. Click "Flash Firmware" to flash the selected firmware
4. Confirm the flashing operation in the dialog

#### Option C: Check for Updates First
1. Click "Check Firmware Update" to compare versions
2. If an update is available, follow Option A
3. If up to date, you can still flash the current version

## Configuration

### GitHub Repository Setup
The firmware update system is configured to use your repository:

1. **Repository URL**: `https://api.github.com/repos/georgebenett/gb_remote/releases/latest`
   - Already configured in `esp32_controller.py`
   - Points to your [gb_remote releases](https://github.com/georgebenett/gb_remote/releases/)

2. **Current Release**: Version 1.0 is available
   - Release date: September 13, 2024
   - The system will compare the ESP32's current version with this release
   - Future releases will be automatically detected

3. **Creating New Releases**:
   - Tag your firmware releases (e.g., `v1.1.0`, `v1.2.0`)
   - Upload firmware binary files as release assets
   - The system will automatically detect the latest release

### Version Format
- Use semantic versioning (e.g., `1.0.0`, `1.1.0`, `2.0.0`)
- The system uses the `packaging` library for proper version comparison

### ESP32 Flashing Requirements
The flashing functionality requires esptool to be installed:

#### Option 1: Install esptool via apt (Recommended)
```bash
sudo apt install esptool
```

#### Option 2: Install esptool via pip
```bash
pip install esptool
```

#### Option 3: Use ESP-IDF installation
- Install ESP-IDF framework
- The system will automatically detect esptool in the ESP-IDF installation

The GUI will auto-detect which method is available and configure accordingly.

## Implementation Details

### ESP32 Side
- Uses `CONFIG_APP_PROJECT_VER` for version information
- Includes build date, target, and IDF version
- Integrates with existing USB serial command system

### Flashing Technical Details
- **Flash Offset**: 0x10000 (64KB) - standard for ESP32 application firmware
- **Chip Type**: ESP32-C3 (configurable in code)
- **Baud Rate**: 921600 for fast flashing
- **Reset Behavior**: Hard reset after flashing to ensure clean boot

### Python Side
- Threaded update checking to avoid blocking the GUI
- Proper error handling for network issues
- Extensible framework for different update sources

## Future Enhancements

### Planned Features:
1. **Automatic Download**: Download firmware binaries from GitHub releases
2. **Integrity Verification**: Verify downloaded firmware using checksums
3. **Flashing Integration**: Integrate with ESP-IDF tools for automatic flashing
4. **Rollback Support**: Keep previous firmware version for rollback
5. **Update Scheduling**: Schedule updates for convenient times

### Customization Options:
1. **Multiple Update Sources**: Support for different repositories or update servers
2. **Update Channels**: Beta, stable, and development channels
3. **User Preferences**: Allow users to configure update behavior
4. **Offline Mode**: Support for local firmware files

## Troubleshooting

### Common Issues:
1. **"Could not determine current firmware version"**
   - Ensure the ESP32 is connected and responding
   - Check that the `get_firmware_version` command is working

2. **"Update check failed"**
   - Check internet connection
   - Verify GitHub repository URL is correct
   - Ensure the repository has public releases

3. **Version comparison errors**
   - Ensure firmware versions follow semantic versioning
   - Check that the `packaging` library is installed

### Debug Information:
- All update-related messages are logged in the response text area
- Look for `[INFO]`, `[ERROR]`, and `[UPDATE AVAILABLE]` prefixes
- Check the console output for detailed error messages

## Security Considerations

### Current Implementation:
- Uses HTTPS for all network requests
- Validates version strings before comparison
- No automatic flashing (user confirmation required)

### Future Security Enhancements:
1. **Digital Signatures**: Verify firmware authenticity
2. **Encrypted Downloads**: Encrypt firmware binaries
3. **Secure Boot**: Integrate with ESP32 secure boot features
4. **Update Verification**: Verify firmware integrity before installation

## Installation

### Python Dependencies:
```bash
pip install -r requirements.txt
```

### ESP32 Firmware:
- The firmware changes are already integrated
- No additional configuration required
- Version information comes from ESP-IDF build system

## Testing

### Manual Testing:
1. Connect to ESP32 and run `get_firmware_version` command
2. Use the GUI to check for updates
3. Verify version comparison logic
4. Test error handling with invalid versions

### Automated Testing:
- Unit tests for version comparison logic
- Integration tests for GUI functionality
- Mock tests for network operations

## Support

For issues or questions regarding the firmware update functionality:
1. Check the troubleshooting section above
2. Review the console output for error messages
3. Ensure all dependencies are properly installed
4. Verify the ESP32 firmware is up to date

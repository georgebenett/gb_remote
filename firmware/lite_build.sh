#!/bin/bash
# Build script for lite target

set -e

echo "Building for lite target ..."

# Create target-specific defaults if it doesn't exist
if [ ! -f sdkconfig.defaults.lite ]; then
    echo "# CONFIG_TARGET_DUAL_THROTTLE is not set" > sdkconfig.defaults.lite
    echo "CONFIG_TARGET_LITE=y" >> sdkconfig.defaults.lite
    echo "CONFIG_LCD_HOR_RES=240" >> sdkconfig.defaults.lite
    echo "CONFIG_LCD_VER_RES=320" >> sdkconfig.defaults.lite
    echo "CONFIG_LCD_OFFSET_X=0" >> sdkconfig.defaults.lite
    echo "CONFIG_LCD_OFFSET_Y=0" >> sdkconfig.defaults.lite
fi

# Copy target defaults
cp sdkconfig.defaults.lite sdkconfig.defaults

# Directly update sdkconfig to ensure target selection is correct
# This is necessary because reconfigure doesn't always override choice options
if [ -f sdkconfig ]; then
    # Replace target configuration lines
    sed -i 's/^CONFIG_TARGET_DUAL_THROTTLE=y/# CONFIG_TARGET_DUAL_THROTTLE is not set/' sdkconfig
    sed -i 's/^# CONFIG_TARGET_LITE is not set$/CONFIG_TARGET_LITE=y/' sdkconfig
    sed -i 's/^CONFIG_LCD_HOR_RES=.*/CONFIG_LCD_HOR_RES=240/' sdkconfig
    sed -i 's/^CONFIG_LCD_VER_RES=.*/CONFIG_LCD_VER_RES=320/' sdkconfig
    sed -i 's/^CONFIG_LCD_OFFSET_X=.*/CONFIG_LCD_OFFSET_X=0/' sdkconfig
    sed -i 's/^CONFIG_LCD_OFFSET_Y=.*/CONFIG_LCD_OFFSET_Y=0/' sdkconfig

    # Ensure CONFIG_TARGET_LITE=y exists (add if missing)
    if ! grep -q "^CONFIG_TARGET_LITE=y" sdkconfig; then
        sed -i '/^# Hardware Target Configuration$/a\
#\
# CONFIG_TARGET_DUAL_THROTTLE is not set\
CONFIG_TARGET_LITE=y' sdkconfig
    fi
fi

# Reconfigure to apply settings
idf.py reconfigure

# Build
idf.py build

# Flash function that tries multiple ports
flash_firmware() {
    echo "Flashing firmware to device..."

    # Priority ports to try first
    PRIORITY_PORTS=("/dev/ttyACM0" "/dev/ttyACM1")

    # Function to try flashing to a specific port
    try_flash_port() {
        local port=$1
        echo "Trying to flash to $port..."
        if [ -c "$port" ]; then
            if idf.py -p "$port" flash; then
                echo "Successfully flashed to $port"
                return 0
            else
                echo "Failed to flash to $port"
                return 1
            fi
        else
            echo "Port $port not available"
            return 1
        fi
    }

    # Try priority ports first
    for port in "${PRIORITY_PORTS[@]}"; do
        if try_flash_port "$port"; then
            return 0
        fi
    done

    # If priority ports failed, try other available ttyACM and ttyUSB ports
    echo "Priority ports failed, scanning for other available ports..."
    OTHER_PORTS=$(ls /dev/ttyACM* /dev/ttyUSB* 2>/dev/null | grep -v -E "(ttyACM0|ttyACM1)$" || true)

    if [ -n "$OTHER_PORTS" ]; then
        for port in $OTHER_PORTS; do
            if try_flash_port "$port"; then
                return 0
            fi
        done
    fi

    echo "ERROR: Failed to flash to any available port!"
    echo "Available ports:"
    ls -la /dev/ttyACM* /dev/ttyUSB* 2>/dev/null || echo "No ttyACM or ttyUSB ports found"
    return 1
}

# Call the flash function
if flash_firmware; then
    echo "Build and flash complete for lite target!"
else
    echo "Build completed but flash failed!"
    exit 1
fi
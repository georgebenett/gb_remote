#!/bin/bash
# Build script for lite target

set -e

# Detect OS for sed compatibility (macOS requires backup extension, Linux doesn't)
if [[ "$OSTYPE" == "darwin"* ]]; then
    SED_INPLACE="sed -i ''"
else
    SED_INPLACE="sed -i"
fi

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

# Update sdkconfig if target changed
if [ ! -f sdkconfig ]; then
    # No sdkconfig exists, use defaults
    cp sdkconfig.defaults.lite sdkconfig.defaults
elif ! grep -q "^CONFIG_TARGET_LITE=y" sdkconfig 2>/dev/null; then
    # Target is not set to LITE, update it
    echo "Updating target configuration to LITE..."
    cp sdkconfig.defaults.lite sdkconfig.defaults

    # Update sdkconfig directly
    $SED_INPLACE 's/^CONFIG_TARGET_DUAL_THROTTLE=y/# CONFIG_TARGET_DUAL_THROTTLE is not set/' sdkconfig
    $SED_INPLACE 's/^# CONFIG_TARGET_LITE is not set$/CONFIG_TARGET_LITE=y/' sdkconfig
    $SED_INPLACE 's/^CONFIG_LCD_HOR_RES=.*/CONFIG_LCD_HOR_RES=240/' sdkconfig
    $SED_INPLACE 's/^CONFIG_LCD_VER_RES=.*/CONFIG_LCD_VER_RES=320/' sdkconfig
    $SED_INPLACE 's/^CONFIG_LCD_OFFSET_X=.*/CONFIG_LCD_OFFSET_X=0/' sdkconfig
    $SED_INPLACE 's/^CONFIG_LCD_OFFSET_Y=.*/CONFIG_LCD_OFFSET_Y=0/' sdkconfig

    # Ensure CONFIG_TARGET_LITE=y exists (add if missing)
    if ! grep -q "^CONFIG_TARGET_LITE=y" sdkconfig; then
        $SED_INPLACE '/^# Hardware Target Configuration$/a\
#\
# CONFIG_TARGET_DUAL_THROTTLE is not set\
CONFIG_TARGET_LITE=y' sdkconfig
    fi
else
    echo "Target already configured for LITE..."
fi

# Build (will auto-reconfigure if sdkconfig changed)
echo "Starting build..."
BUILD_START=$(date +%s)
idf.py build
BUILD_END=$(date +%s)
BUILD_TIME=$((BUILD_END - BUILD_START))
echo "Build completed in ${BUILD_TIME} seconds"

# Flash function that tries multiple ports
flash_firmware() {
    echo "Flashing firmware to device..."

    # Detect OS and set priority ports accordingly
    if [[ "$OSTYPE" == "darwin"* ]]; then
        # macOS ports
        PRIORITY_PORTS=("/dev/tty.usbmodem101" "/dev/tty.usbmodem*" "/dev/tty.usbserial*")
        SCAN_PATTERNS=("/dev/tty.usbmodem*" "/dev/tty.usbserial*")
    else
        # Linux ports
        PRIORITY_PORTS=("/dev/ttyACM0" "/dev/ttyACM1")
        SCAN_PATTERNS=("/dev/ttyACM*" "/dev/ttyUSB*")
    fi

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

    # Try priority ports first (expand globs for macOS)
    for port_pattern in "${PRIORITY_PORTS[@]}"; do
        # Expand glob pattern if it contains wildcards
        if [[ "$port_pattern" == *"*"* ]]; then
            # Expand glob and try each matching port
            for port in $port_pattern; do
                # Check if port actually exists (not the literal pattern)
                if [ "$port" != "$port_pattern" ] && [ -e "$port" ] && try_flash_port "$port"; then
                    return 0
                fi
            done
        else
            if try_flash_port "$port_pattern"; then
                return 0
            fi
        fi
    done

    # If priority ports failed, try other available ports
    echo "Priority ports failed, scanning for other available ports..."
    OTHER_PORTS=""
    for pattern in "${SCAN_PATTERNS[@]}"; do
        if [[ "$OSTYPE" == "darwin"* ]]; then
            # For macOS, exclude already tried ports
            FOUND=$(ls $pattern 2>/dev/null | grep -v -E "(usbmodem101)$" || true)
        else
            # For Linux, exclude priority ports
            FOUND=$(ls $pattern 2>/dev/null | grep -v -E "(ttyACM0|ttyACM1)$" || true)
        fi
        if [ -n "$FOUND" ]; then
            OTHER_PORTS="$OTHER_PORTS $FOUND"
        fi
    done

    if [ -n "$OTHER_PORTS" ]; then
        for port in $OTHER_PORTS; do
            if try_flash_port "$port"; then
                return 0
            fi
        done
    fi

    echo "ERROR: Failed to flash to any available port!"
    echo "Available ports:"
    if [[ "$OSTYPE" == "darwin"* ]]; then
        ls -la /dev/tty.usbmodem* /dev/tty.usbserial* 2>/dev/null || echo "No macOS USB serial ports found"
    else
        ls -la /dev/ttyACM* /dev/ttyUSB* 2>/dev/null || echo "No ttyACM or ttyUSB ports found"
    fi
    return 1
}

# Call the flash function
if flash_firmware; then
    echo "Build and flash complete for lite target!"
else
    echo "Build completed but flash failed!"
    exit 1
fi
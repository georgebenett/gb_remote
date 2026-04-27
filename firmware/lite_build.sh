#!/bin/bash
# Build and flash script for lite target
set -e

BUILD_DIR="build_lite"
SDKCONFIG_FILE="sdkconfig.lite"
SDKCONFIG_DEFAULTS_FILE="sdkconfig.defaults.lite"

echo "Building for lite target..."

if command -v ccache &> /dev/null; then
    export IDF_CCACHE_ENABLE=1
    echo "ccache enabled"
fi

BUILD_START=$(date +%s)
idf.py -B "$BUILD_DIR" \
    -D SDKCONFIG="$SDKCONFIG_FILE" \
    -D SDKCONFIG_DEFAULTS="$SDKCONFIG_DEFAULTS_FILE" \
    build
BUILD_END=$(date +%s)
echo "Build completed in $((BUILD_END - BUILD_START)) seconds"

flash_firmware() {
    echo "Flashing firmware to device..."

    if [[ "$OSTYPE" == "darwin"* ]]; then
        PRIORITY_PORTS=("/dev/tty.usbmodem101" "/dev/tty.usbmodem*" "/dev/tty.usbserial*")
        SCAN_PATTERNS=("/dev/tty.usbmodem*" "/dev/tty.usbserial*")
    else
        PRIORITY_PORTS=("/dev/ttyACM0" "/dev/ttyACM1")
        SCAN_PATTERNS=("/dev/ttyACM*" "/dev/ttyUSB*")
    fi

    try_flash_port() {
        local port=$1
        echo "Trying to flash to $port..."
        if [ -c "$port" ]; then
            if idf.py -B "$BUILD_DIR" -p "$port" flash; then
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

    for port_pattern in "${PRIORITY_PORTS[@]}"; do
        if [[ "$port_pattern" == *"*"* ]]; then
            for port in $port_pattern; do
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

    echo "Priority ports failed, scanning for other available ports..."
    OTHER_PORTS=""
    for pattern in "${SCAN_PATTERNS[@]}"; do
        if [[ "$OSTYPE" == "darwin"* ]]; then
            FOUND=$(ls $pattern 2>/dev/null | grep -v -E "(usbmodem101)$" || true)
        else
            FOUND=$(ls $pattern 2>/dev/null | grep -v -E "(ttyACM0|ttyACM1)$" || true)
        fi
        [ -n "$FOUND" ] && OTHER_PORTS="$OTHER_PORTS $FOUND"
    done

    if [ -n "$OTHER_PORTS" ]; then
        for port in $OTHER_PORTS; do
            try_flash_port "$port" && return 0
        done
    fi

    echo "ERROR: Failed to flash to any available port!"
    if [[ "$OSTYPE" == "darwin"* ]]; then
        ls -la /dev/tty.usbmodem* /dev/tty.usbserial* 2>/dev/null || echo "No macOS USB serial ports found"
    else
        ls -la /dev/ttyACM* /dev/ttyUSB* 2>/dev/null || echo "No ttyACM or ttyUSB ports found"
    fi
    return 1
}

if flash_firmware; then
    echo "Build and flash complete for lite target!"
else
    echo "Build completed but flash failed!"
    exit 1
fi

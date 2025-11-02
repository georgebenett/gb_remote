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

# Flash
echo "Flashing firmware to device..."
idf.py flash

echo "Build and flash complete for lite target!"


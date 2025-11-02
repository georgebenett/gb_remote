#!/bin/bash
# Build and flash script for dual_throttle target
set -e

echo "Building for dual_throttle target ..."

# Create target-specific defaults if it doesn't exist
if [ ! -f sdkconfig.defaults.dual_throttle ]; then
    echo "CONFIG_TARGET_DUAL_THROTTLE=y" > sdkconfig.defaults.dual_throttle
    echo "# CONFIG_TARGET_LITE is not set" >> sdkconfig.defaults.dual_throttle
    echo "CONFIG_LCD_HOR_RES=172" >> sdkconfig.defaults.dual_throttle
    echo "CONFIG_LCD_VER_RES=320" >> sdkconfig.defaults.dual_throttle
    echo "CONFIG_LCD_OFFSET_X=34" >> sdkconfig.defaults.dual_throttle
    echo "CONFIG_LCD_OFFSET_Y=0" >> sdkconfig.defaults.dual_throttle
fi

# Copy target defaults
cp sdkconfig.defaults.dual_throttle sdkconfig.defaults

# Directly update sdkconfig to ensure target selection is correct
# This is necessary because reconfigure doesn't always override choice options
if [ -f sdkconfig ]; then
    # Replace target configuration lines
    sed -i 's/^# CONFIG_TARGET_DUAL_THROTTLE is not set$/CONFIG_TARGET_DUAL_THROTTLE=y/' sdkconfig
    sed -i 's/^CONFIG_TARGET_LITE=y/# CONFIG_TARGET_LITE is not set/' sdkconfig
    sed -i 's/^CONFIG_LCD_HOR_RES=.*/CONFIG_LCD_HOR_RES=172/' sdkconfig
    sed -i 's/^CONFIG_LCD_VER_RES=.*/CONFIG_LCD_VER_RES=320/' sdkconfig
    sed -i 's/^CONFIG_LCD_OFFSET_X=.*/CONFIG_LCD_OFFSET_X=34/' sdkconfig
    sed -i 's/^CONFIG_LCD_OFFSET_Y=.*/CONFIG_LCD_OFFSET_Y=0/' sdkconfig

    # Ensure CONFIG_TARGET_DUAL_THROTTLE=y exists (add if missing)
    if ! grep -q "^CONFIG_TARGET_DUAL_THROTTLE=y" sdkconfig; then
        sed -i '/^# Hardware Target Configuration$/a\
#\
CONFIG_TARGET_DUAL_THROTTLE=y\
# CONFIG_TARGET_LITE is not set' sdkconfig
    fi
fi

# Reconfigure to apply settings
idf.py reconfigure

# Build
idf.py build

# Flash
echo "Flashing firmware to device..."
idf.py flash

echo "Build and flash complete for dual_throttle target!"


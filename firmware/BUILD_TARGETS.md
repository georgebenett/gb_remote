# Building for Different Hardware Targets

This firmware supports building for two different hardware targets with different screen sizes and throttle logic configurations.

## Supported Targets

1. **dual_throttle (172x320 Screen)**
   - Screen resolution: 172x320 pixels
   - LCD X offset: 34

2. **lite (240x320 Screen)**
   - Screen resolution: 240x320 pixels
   - LCD X offset: 0

## Building Methods

### Method 1: Using menuconfig (Recommended)

1. Configure the target using menuconfig:
   ```bash
   idf.py menuconfig
   ```

2. Navigate to: **Hardware Target Configuration** → **Select Hardware Target**
   - Choose either "dual_throttle (172x320 Screen)" or "lite (240x320 Screen)"

3. Save the configuration and build:
   ```bash
   idf.py build
   ```

### Method 2: Using sdkconfig.defaults

Create target-specific `sdkconfig.defaults` files:

**For dual_throttle target:**
```bash
echo "CONFIG_TARGET_DUAL_THROTTLE=y" > sdkconfig.defaults.dual_throttle
echo "CONFIG_LCD_HOR_RES=172" >> sdkconfig.defaults.dual_throttle
echo "CONFIG_LCD_VER_RES=320" >> sdkconfig.defaults.dual_throttle
echo "CONFIG_LCD_OFFSET_X=34" >> sdkconfig.defaults.dual_throttle
echo "CONFIG_LCD_OFFSET_Y=0" >> sdkconfig.defaults.dual_throttle
```

**For lite target:**
```bash
echo "CONFIG_TARGET_LITE=y" > sdkconfig.defaults.lite
echo "CONFIG_LCD_HOR_RES=240" >> sdkconfig.defaults.lite
echo "CONFIG_LCD_VER_RES=320" >> sdkconfig.defaults.lite
echo "CONFIG_LCD_OFFSET_X=0" >> sdkconfig.defaults.lite
echo "CONFIG_LCD_OFFSET_Y=0" >> sdkconfig.defaults.lite
```

Then copy the appropriate file before building:
```bash
cp sdkconfig.defaults.dual_throttle sdkconfig.defaults
idf.py build
```

### Method 3: Using Build Scripts

Use the provided build scripts (see `build_*.sh` files in the project root):
```bash
./build_dual_throttle.sh    # Build and flash for dual_throttle target (172x320)
./build_lite.sh             # Build and flash for lite target (240x320)
```

These scripts will automatically build and flash the firmware to your connected device.

## Configuration Files

- `main/Kconfig.projbuild` - Kconfig definitions for target selection
- `main/target_config.h` - Header file with target-specific configuration macros
- `sdkconfig` - Generated configuration file (do not edit manually)
- `sdkconfig.defaults` - Default configuration values

## Adding Target-Specific Throttle Logic

To add different throttle logic for different targets:

1. Add throttle configuration options to `main/Kconfig.projbuild`:
   ```
   config THROTTLE_LOGIC_TYPE
       int "Throttle Logic Type"
       default 1 if TARGET_DUAL_THROTTLE
       default 2 if TARGET_LITE
   ```

2. Use conditional compilation in throttle code:
   ```c
   #include "target_config.h"
   
   #ifdef CONFIG_TARGET_DUAL_THROTTLE
       // dual_throttle throttle logic
   #elif defined(CONFIG_TARGET_LITE)
       // lite throttle logic
   #endif
   ```

## Flashing

### Using Build Scripts (Recommended)

The build scripts automatically flash the firmware after building:
```bash
./build_dual_throttle.sh    # Builds and flashes dual_throttle target
./build_lite.sh             # Builds and flashes lite target
```

### Manual Flashing

If you've already built the firmware and just want to flash:
```bash
idf.py flash
```

### Monitor Serial Output

To monitor the serial output after flashing:
```bash
idf.py monitor
```

Or combine flash and monitor:
```bash
idf.py flash monitor
```

Press `Ctrl+]` to exit the monitor.

### Specifying Serial Port

If you have multiple serial devices, specify the port:
```bash
idf.py -p /dev/ttyUSB0 flash monitor
```

Or set it once:
```bash
export ESPPORT=/dev/ttyUSB0
idf.py flash
```

## Verifying Target Configuration

After building, you can verify the target configuration by checking:
- The build log will show the selected target
- `sdkconfig` file contains `CONFIG_TARGET_DUAL_THROTTLE=y` or `CONFIG_TARGET_LITE=y`
- Screen resolution constants in `target_config.h` match your selection


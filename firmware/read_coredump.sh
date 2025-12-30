#!/bin/bash
# Script to read and decode crash dump from ESP32 flash

set -e

# Configuration
SERIAL_PORT="${1:-/dev/tty.usbmodem1101}"
COREDUMP_OFFSET=0x2F0000
COREDUMP_SIZE=0x010000
ELF_FILE="build/gb_controller_lite.elf"
RAW_DUMP_FILE="coredump_raw.bin"
DECODED_DUMP_FILE="coredump_decoded.txt"

echo "Reading crash dump from flash..."
echo "Serial port: $SERIAL_PORT"
echo "Coredump partition offset: $COREDUMP_OFFSET"
echo "Coredump partition size: $COREDUMP_SIZE"
echo ""

# Check if ELF file exists
if [ ! -f "$ELF_FILE" ]; then
    echo "Error: ELF file not found at $ELF_FILE"
    exit 1
fi

# Try to find esptool
ESPTOOL=""
if command -v esptool.py &> /dev/null; then
    ESPTOOL="esptool.py"
elif python3 -m esptool --help &> /dev/null; then
    ESPTOOL="python3 -m esptool"
elif [ -f "$HOME/esp/v5.3/esp-idf/components/esptool_py/esptool/esptool.py" ]; then
    ESPTOOL="python3 $HOME/esp/v5.3/esp-idf/components/esptool_py/esptool/esptool.py"
elif [ -f "$HOME/esp/esp-idf/components/esptool_py/esptool/esptool.py" ]; then
    ESPTOOL="python3 $HOME/esp/esp-idf/components/esptool_py/esptool/esptool.py"
elif [ -f "$HOME/.espressif/python_env/idf5.3_py3.14_env/bin/esptool.py" ]; then
    ESPTOOL="$HOME/.espressif/python_env/idf5.3_py3.14_env/bin/esptool.py"
elif [ -f "$HOME/.espressif/python_env/idf5.3_py3.9_env/bin/esptool.py" ]; then
    ESPTOOL="$HOME/.espressif/python_env/idf5.3_py3.9_env/bin/esptool.py"
else
    echo "Error: esptool not found. Please install ESP-IDF or esptool."
    echo ""
    echo "You can install esptool with:"
    echo "  pip3 install esptool"
    echo ""
    echo "Or set up ESP-IDF environment:"
    echo "  source \$HOME/esp/esp-idf/export.sh"
    exit 1
fi

# Read coredump partition from flash
echo "Reading coredump partition from flash..."
$ESPTOOL --port "$SERIAL_PORT" read_flash $COREDUMP_OFFSET $COREDUMP_SIZE "$RAW_DUMP_FILE"

if [ ! -f "$RAW_DUMP_FILE" ]; then
    echo "Error: Failed to read coredump from flash"
    exit 1
fi

echo "Successfully read coredump data to $RAW_DUMP_FILE"
echo ""

# Try to decode the coredump
echo "Decoding crash dump..."
ESPCOREDUMP=""
if command -v espcoredump.py &> /dev/null; then
    ESPCOREDUMP="espcoredump.py"
elif [ -f "$HOME/esp/v5.3/esp-idf/components/espcoredump/espcoredump.py" ]; then
    ESPCOREDUMP="python3 $HOME/esp/v5.3/esp-idf/components/espcoredump/espcoredump.py"
elif [ -f "$HOME/esp/esp-idf/components/espcoredump/espcoredump.py" ]; then
    ESPCOREDUMP="python3 $HOME/esp/esp-idf/components/espcoredump/espcoredump.py"
elif [ -f "$HOME/.espressif/python_env/idf5.3_py3.14_env/bin/espcoredump.py" ]; then
    ESPCOREDUMP="$HOME/.espressif/python_env/idf5.3_py3.14_env/bin/espcoredump.py"
elif [ -f "$HOME/.espressif/python_env/idf5.3_py3.9_env/bin/espcoredump.py" ]; then
    ESPCOREDUMP="$HOME/.espressif/python_env/idf5.3_py3.9_env/bin/espcoredump.py"
else
    echo "Warning: espcoredump.py not found. Raw dump saved to $RAW_DUMP_FILE"
    echo ""
    echo "To decode the dump, install ESP-IDF and run:"
    echo "  espcoredump.py info_corefile -t elf -c $RAW_DUMP_FILE $ELF_FILE"
    echo "  (or try -t raw if ELF format doesn't work)"
    exit 0
fi

# Decode the coredump
# Try ELF format first (default in ESP-IDF), then fall back to raw
if $ESPCOREDUMP info_corefile -t elf -c "$RAW_DUMP_FILE" "$ELF_FILE" > "$DECODED_DUMP_FILE" 2>&1; then
    echo "Decoded as ELF format"
elif $ESPCOREDUMP info_corefile -t raw -c "$RAW_DUMP_FILE" "$ELF_FILE" > "$DECODED_DUMP_FILE" 2>&1; then
    echo "Decoded as raw format"
else
    echo "Warning: Failed to decode coredump with both ELF and raw formats"
    echo "Raw dump saved to: $RAW_DUMP_FILE"
    exit 1
fi

if [ -f "$DECODED_DUMP_FILE" ]; then
    echo "Crash dump decoded successfully!"
    echo "Decoded output saved to: $DECODED_DUMP_FILE"
    echo ""
    echo "=== CRASH DUMP SUMMARY ==="
    head -50 "$DECODED_DUMP_FILE"
    echo ""
    echo "Full dump available in: $DECODED_DUMP_FILE"
else
    echo "Warning: Failed to decode coredump, but raw data is in $RAW_DUMP_FILE"
fi


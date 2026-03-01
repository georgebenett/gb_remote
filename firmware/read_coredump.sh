#!/bin/bash
# Script to read and decode crash dump from ESP32 flash or from a coredump file.
#
# Usage:
#   ./read_coredump.sh <coredump_name> [serial_port]
#
# The coredump name is used to detect product (Lite or Dual_Throttle) and version.
# The script downloads the matching ELF from the GitHub release (via gh) and uses it
# to decode. It does not build firmware locally, to avoid SHA/build-date mismatch.
#
# Examples:
#   ./read_coredump.sh coredump_2026-03-01T19-07-08-246Z_Dual_Throttle_v2.0.4
#   ./read_coredump.sh coredump_2026-03-01T19-07-08-246Z_Dual_Throttle_v2.0.4 /dev/tty.usbmodem1101
#
# If the coredump .bin file exists (e.g. coredump_..._v2.0.4.bin), it will be used.
# Otherwise the script reads the coredump partition from the device via serial_port.

set -e

# Configuration
COREDUMP_OFFSET=0x2F0000
COREDUMP_SIZE=0x010000
ELF_FILE="build/gb_controller_lite.elf"
RAW_DUMP_FILE="coredump_raw.bin"
DECODED_DUMP_FILE="coredump_decoded.txt"

# --- Parse usage ---
if [ $# -lt 1 ]; then
    echo "Usage: $0 <coredump_name> [serial_port]"
    echo ""
    echo "  coredump_name  e.g. coredump_2026-03-01T19-07-08-246Z_Dual_Throttle_v2.0.4 (with or without .bin)"
    echo "  serial_port   optional, for reading from device if coredump file not found (default: /dev/tty.usbmodem1101)"
    echo ""
    echo "The script parses product (Lite / Dual_Throttle) and version from the filename,"
    echo "then downloads the ELF from the GitHub release (gh CLI) and decodes."
    exit 1
fi

COREDUMP_INPUT="$1"
SERIAL_PORT="${2:-/dev/tty.usbmodem1101}"

# Script and repo paths
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"
if [ ! -f "CMakeLists.txt" ]; then
    echo "Error: Script must be in firmware directory (CMakeLists.txt not found)"
    exit 1
fi
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# Resolve to absolute path so we can find the file after cd'ing
if [ -f "$COREDUMP_INPUT" ]; then
    COREDUMP_ABS=$(cd "$(dirname "$COREDUMP_INPUT")" && pwd)/$(basename "$COREDUMP_INPUT")
elif [ -f "${COREDUMP_INPUT}.bin" ]; then
    COREDUMP_ABS=$(cd "$(dirname "${COREDUMP_INPUT}.bin")" && pwd)/$(basename "${COREDUMP_INPUT}.bin")
else
    COREDUMP_ABS=""
fi

# Basename without .bin for parsing
BASENAME=$(basename "$COREDUMP_INPUT" .bin)

# --- Parse product and version from filename ---
# Expected patterns: ..._Dual_Throttle_v2.0.4 or ..._Lite_v2.0.4
if [[ "$BASENAME" == *"_Dual_Throttle_"* ]]; then
    TARGET="dual_throttle"
elif [[ "$BASENAME" == *"_Lite_"* ]]; then
    TARGET="lite"
else
    echo "Error: Could not determine product from filename '$BASENAME'"
    echo "Expected filename to contain _Dual_Throttle_ or _Lite_ (e.g. coredump_*_Dual_Throttle_v2.0.4)"
    exit 1
fi

# Extract version (last _vX.Y.Z part)
VERSION=$(echo "$BASENAME" | sed -n 's/.*_v\([0-9][0-9.]*\)$/\1/p')
if [ -z "$VERSION" ]; then
    echo "Error: Could not extract version from filename '$BASENAME'"
    echo "Expected suffix _vX.Y.Z (e.g. _v2.0.4)"
    exit 1
fi

# --- Require GitHub CLI and get ELF from release (no local build; avoids SHA mismatch) ---
if ! command -v gh &>/dev/null; then
    echo "Error: GitHub CLI (gh) is required to download the release ELF. Install from https://cli.github.com/"
    exit 1
fi
REPO=$(cd "$REPO_ROOT" && gh repo view --json nameWithOwner -q .nameWithOwner 2>/dev/null) || true
if [ -z "$REPO" ]; then
    echo "Error: Could not detect GitHub repo. Run from a git clone and ensure 'gh auth login' has been done."
    exit 1
fi

echo "Coredump: $BASENAME"
echo "Product:  $TARGET"
echo "Version:  $VERSION"
echo ""

# --- Try to get ELF from GitHub release (avoids SHA mismatch vs. rebuilding) ---
ELF_CACHE="$SCRIPT_DIR/.coredump_elf_cache"
RELEASE_ELF="$ELF_CACHE/gb_controller_${TARGET}_v${VERSION}.elf"
ELF_FILE="$RELEASE_ELF"

if [ -f "$RELEASE_ELF" ]; then
    echo "Using cached ELF from release: $RELEASE_ELF"
else
    echo "Downloading ELF from GitHub release v$VERSION..."
    mkdir -p "$ELF_CACHE"
    TMP_ZIP=$(mktemp)
    trap "rm -f $TMP_ZIP" EXIT
    if ! (cd "$REPO_ROOT" && gh release download "v$VERSION" --repo "$REPO" --pattern "${TARGET}_v${VERSION}.zip" -O "$TMP_ZIP" --clobber); then
        echo "Error: Could not download release v$VERSION asset '${TARGET}_v${VERSION}.zip'."
        echo "Check that release v$VERSION exists: https://github.com/$REPO/releases/tag/v$VERSION"
        exit 1
    fi
    if ! unzip -j -o "$TMP_ZIP" "gb_controller_${TARGET}.elf" -d "$ELF_CACHE"; then
        echo "Error: Release zip does not contain gb_controller_${TARGET}.elf."
        exit 1
    fi
    rm -f "$TMP_ZIP"
    trap - EXIT
    if [ ! -f "$ELF_CACHE/gb_controller_${TARGET}.elf" ]; then
        echo "Error: ELF not found after unzip."
        exit 1
    fi
    mv "$ELF_CACHE/gb_controller_${TARGET}.elf" "$RELEASE_ELF"
    echo "Using ELF from release v$VERSION"
fi
echo ""

# --- Obtain coredump: from file or from flash ---
if [ -n "$COREDUMP_ABS" ] && [ -f "$COREDUMP_ABS" ]; then
    echo "Using existing coredump file: $COREDUMP_ABS"
    cp "$COREDUMP_ABS" "$RAW_DUMP_FILE"
else
    echo "Coredump file not found, reading from flash..."
    echo "Serial port: $SERIAL_PORT"
    ESPTOOL=""
    if command -v esptool.py &>/dev/null; then
        ESPTOOL="esptool.py"
    elif python3 -m esptool --help &>/dev/null; then
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
        echo "Error: esptool not found. Install ESP-IDF or: pip3 install esptool"
        exit 1
    fi
    $ESPTOOL --port "$SERIAL_PORT" read_flash $COREDUMP_OFFSET $COREDUMP_SIZE "$RAW_DUMP_FILE"
fi

if [ ! -f "$RAW_DUMP_FILE" ]; then
    echo "Error: No coredump data (file missing or read failed)"
    exit 1
fi
echo ""

# --- Decode coredump ---
if [ ! -f "$ELF_FILE" ]; then
    echo "Error: ELF file not found at $ELF_FILE"
    exit 1
fi

ESPCOREDUMP=""
if command -v espcoredump.py &>/dev/null; then
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
    echo "To decode: espcoredump.py info_corefile -t elf -c $RAW_DUMP_FILE $ELF_FILE"
    exit 0
fi

echo "Decoding crash dump..."
if $ESPCOREDUMP info_corefile -t elf -c "$RAW_DUMP_FILE" "$ELF_FILE" > "$DECODED_DUMP_FILE" 2>&1; then
    echo "Decoded as ELF format"
elif $ESPCOREDUMP info_corefile -t raw -c "$RAW_DUMP_FILE" "$ELF_FILE" > "$DECODED_DUMP_FILE" 2>&1; then
    echo "Decoded as raw format"
else
    echo "Warning: Failed to decode with both ELF and raw formats"
    echo "Raw dump saved to: $RAW_DUMP_FILE"
    exit 1
fi

echo ""
echo "Crash dump decoded successfully!"
echo "Decoded output saved to: $DECODED_DUMP_FILE"
echo ""
echo "=== CRASH DUMP SUMMARY ==="
head -50 "$DECODED_DUMP_FILE"
echo ""
echo "Full dump available in: $DECODED_DUMP_FILE"

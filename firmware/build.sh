#!/bin/bash
# Build one variant, optionally flash it.
#   ./build.sh <lite|dual_throttle> [--flash] [--port PORT]
# Builds into build_<variant>/ from sdkconfig.defaults + sdkconfig.defaults.<variant>.
set -e
cd "$(dirname "$0")"

VARIANT=$1
shift || true
case "$VARIANT" in
    lite|dual_throttle) ;;
    *) echo "Usage: $0 <lite|dual_throttle> [--flash] [--port PORT]"; exit 1 ;;
esac

FLASH=0
PORT=""
while [ $# -gt 0 ]; do
    case "$1" in
        --flash) FLASH=1 ;;
        --port) PORT=$2; shift ;;
        *) echo "Unknown option: $1"; exit 1 ;;
    esac
    shift
done

IDF=(idf.py -B "build_$VARIANT" -D SDKCONFIG="sdkconfig.$VARIANT"
     -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.$VARIANT")
export IDF_TARGET=esp32s3
command -v ccache >/dev/null && export IDF_CCACHE_ENABLE=1

# Build the sibling gb_receiver and bundle it into receiver_fw/ (rx_fw).
RX_BUNDLE="../../gb_receiver/bundle_to_remotes.sh"
if [ -f "$RX_BUNDLE" ] && [ -z "$CI" ]; then
    bash "$RX_BUNDLE"
else
    echo "Keeping receiver_fw/ as is"
fi

"${IDF[@]}" build
[ "$FLASH" = 1 ] || exit 0

if [ -z "$PORT" ]; then
    if [[ "$OSTYPE" == darwin* ]]; then
        PORT=$(ls /dev/tty.usbmodem* /dev/tty.usbserial* 2>/dev/null | head -1)
    else
        PORT=$(ls /dev/ttyACM* /dev/ttyUSB* 2>/dev/null | head -1)
    fi
fi
if [ -z "$PORT" ]; then
    echo "No serial port found; pass --port"
    exit 1
fi
"${IDF[@]}" -p "$PORT" flash

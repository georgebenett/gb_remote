#!/bin/bash
# Remove macOS-generated cruft files from the firmware folder.
# macOS sometimes creates duplicate files with trailing '' or " in the filename
# (e.g. battery.c'' or usb_serial.c")
#
# Usage: ./remove_macos_cruft.sh [--dry-run]
#   --dry-run  List files that would be removed without deleting them

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DRY_RUN=0

for arg in "$@"; do
    case "$arg" in
        --dry-run) DRY_RUN=1 ;;
    esac
done

count=0
while IFS= read -r -d '' file; do
    if [[ $DRY_RUN -eq 1 ]]; then
        echo "Would remove: $file"
    else
        rm -f "$file"
        echo "Removed: $file"
    fi
    ((count++)) || true
done < <(find "$SCRIPT_DIR" -type f \( -name "*''" -o -name '*"' \) ! -path "*/build/*" ! -path "*/managed_components/*" -print0 2>/dev/null)

if [[ $count -eq 0 ]]; then
    echo "No macOS cruft files found."
else
    echo ""
    [[ $DRY_RUN -eq 1 ]] && echo "Total: $count file(s) would be removed." || echo "Total: $count file(s) removed."
fi

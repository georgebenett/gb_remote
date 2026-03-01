#!/bin/bash
# Build and release script for lite and dual_throttle targets
# Creates zip packages and uploads them to GitHub releases

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Detect OS for sed compatibility
if [[ "$OSTYPE" == "darwin"* ]]; then
    sed_inplace() { sed -i '' "$@"; }
else
    sed_inplace() { sed -i "$@"; }
fi

# Function to print colored messages
print_info() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

print_warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

print_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

# Check if we're in the right directory
if [ ! -f "CMakeLists.txt" ] || [ ! -f "main/version.h" ]; then
    print_error "Must be run from firmware root directory"
    exit 1
fi

# Extract version from version.h
VERSION=$(grep -E '^#define FW_VERSION' main/version.h | sed -E 's/.*"([^"]+)".*/\1/')
if [ -z "$VERSION" ]; then
    print_error "Could not extract version from main/version.h"
    exit 1
fi

print_info "Building firmware version: $VERSION"

# Check for required tools
if ! command -v gh &> /dev/null; then
    print_error "GitHub CLI (gh) is not installed. Install it from https://cli.github.com/"
    exit 1
fi

if ! command -v zip &> /dev/null; then
    print_error "zip command is not available. Please install it."
    exit 1
fi

# Check if gh is authenticated
if ! gh auth status &> /dev/null; then
    print_error "GitHub CLI is not authenticated. Run 'gh auth login'"
    exit 1
fi

# Get repository info
REPO=$(gh repo view --json nameWithOwner -q .nameWithOwner 2>/dev/null || echo "")
if [ -z "$REPO" ]; then
    print_warn "Could not detect GitHub repository. Will prompt for repository name."
    read -p "Enter repository (owner/repo): " REPO
fi

# Create temporary directory for artifacts
ARTIFACTS_DIR=$(mktemp -d)
trap "rm -rf $ARTIFACTS_DIR" EXIT

print_info "Artifacts will be stored in: $ARTIFACTS_DIR"

# Function to build a target
build_target() {
    local target=$1
    local config_file="sdkconfig.defaults.$target"

    print_info "Building for $target target..."

    # Create target-specific defaults if it doesn't exist
    if [ ! -f "$config_file" ]; then
        print_warn "$config_file not found, creating it..."
        if [ "$target" == "lite" ]; then
            echo "# CONFIG_TARGET_DUAL_THROTTLE is not set" > "$config_file"
            echo "CONFIG_TARGET_LITE=y" >> "$config_file"
            echo "CONFIG_LCD_HOR_RES=240" >> "$config_file"
            echo "CONFIG_LCD_VER_RES=320" >> "$config_file"
            echo "CONFIG_LCD_OFFSET_X=0" >> "$config_file"
            echo "CONFIG_LCD_OFFSET_Y=0" >> "$config_file"
        else
            echo "CONFIG_TARGET_DUAL_THROTTLE=y" > "$config_file"
            echo "# CONFIG_TARGET_LITE is not set" >> "$config_file"
            echo "CONFIG_LCD_HOR_RES=172" >> "$config_file"
            echo "CONFIG_LCD_VER_RES=320" >> "$config_file"
            echo "CONFIG_LCD_OFFSET_X=34" >> "$config_file"
            echo "CONFIG_LCD_OFFSET_Y=0" >> "$config_file"
        fi
    fi

    # Update sdkconfig
    if [ ! -f sdkconfig ]; then
        cp "$config_file" sdkconfig.defaults
    else
        cp "$config_file" sdkconfig.defaults
        if [ "$target" == "lite" ]; then
            sed_inplace 's/^CONFIG_TARGET_DUAL_THROTTLE=y/# CONFIG_TARGET_DUAL_THROTTLE is not set/' sdkconfig
            sed_inplace 's/^# CONFIG_TARGET_LITE is not set$/CONFIG_TARGET_LITE=y/' sdkconfig || true
        else
            sed_inplace 's/^# CONFIG_TARGET_DUAL_THROTTLE is not set$/CONFIG_TARGET_DUAL_THROTTLE=y/' sdkconfig || true
            sed_inplace 's/^CONFIG_TARGET_LITE=y/# CONFIG_TARGET_LITE is not set/' sdkconfig
        fi
    fi

    # Enable ccache if available
    if command -v ccache &> /dev/null; then
        export IDF_CCACHE_ENABLE=1
    fi

    # Build
    BUILD_START=$(date +%s)
    idf.py build
    BUILD_END=$(date +%s)
    BUILD_TIME=$((BUILD_END - BUILD_START))
    print_info "Build completed in ${BUILD_TIME} seconds"

    # Find the main app binary (ESP-IDF creates it with the project name)
    # The project name is gb_controller_lite, so the binary should be gb_controller_lite.bin
    APP_BIN="build/gb_controller_lite.bin"

    if [ ! -f "$APP_BIN" ]; then
        # Try to find any .bin file in build root (excluding bootloader and partition table)
        APP_BIN=$(find build -maxdepth 1 -name "*.bin" -type f ! -name "bootloader.bin" ! -name "partition-table.bin" | head -1)
    fi

    if [ -z "$APP_BIN" ] || [ ! -f "$APP_BIN" ]; then
        print_error "Could not find main application binary in build directory"
        print_error "Expected: build/gb_controller_lite.bin"
        exit 1
    fi

    # Verify other required binaries exist
    if [ ! -f "build/bootloader/bootloader.bin" ]; then
        print_error "Could not find bootloader.bin"
        exit 1
    fi

    if [ ! -f "build/partition_table/partition-table.bin" ]; then
        print_error "Could not find partition-table.bin"
        exit 1
    fi

    # Package artifacts
    ZIP_NAME="${target}_v${VERSION}.zip"
    ZIP_PATH="$ARTIFACTS_DIR/$ZIP_NAME"

    print_info "Packaging $target artifacts into $ZIP_NAME..."

    # Create a temporary staging directory for packaging
    STAGING_DIR=$(mktemp -d)

    # Copy and rename files to staging directory with correct names
    # The updater expects: gb_controller_<target>.bin, bootloader.bin, partition-table.bin
    # Include .elf for coredump decoding (must match flashed binary to avoid SHA mismatch)
    cp "$APP_BIN" "$STAGING_DIR/gb_controller_${target}.bin"
    cp "build/bootloader/bootloader.bin" "$STAGING_DIR/bootloader.bin"
    cp "build/partition_table/partition-table.bin" "$STAGING_DIR/partition-table.bin"
    if [ -f "build/gb_controller_lite.elf" ]; then
        cp "build/gb_controller_lite.elf" "$STAGING_DIR/gb_controller_${target}.elf"
    fi

    # Create zip from staging directory (files will be at root level)
    cd "$STAGING_DIR"
    zip -q "$ZIP_PATH" \
        gb_controller_${target}.bin \
        bootloader.bin \
        partition-table.bin
    if [ -f "gb_controller_${target}.elf" ]; then
        zip -q "$ZIP_PATH" "gb_controller_${target}.elf"
    fi
    cd - > /dev/null

    # Clean up staging directory
    rm -rf "$STAGING_DIR"

    # Verify zip was created
    if [ ! -f "$ZIP_PATH" ]; then
        print_error "Failed to create zip file"
        exit 1
    fi

    print_info "Created $ZIP_NAME ($(du -h "$ZIP_PATH" | cut -f1))"

    # Clean build directory for next target
    # Use manual removal instead of fullclean to avoid managed components issues
    print_info "Cleaning build directory..."
    if [ -d "build" ]; then
        rm -rf build
    fi
}

# Build both targets
build_target "lite"
build_target "dual_throttle"

# List created artifacts
print_info "Created artifacts:"
ls -lh "$ARTIFACTS_DIR"/*.zip

# Ask for confirmation before creating release
echo ""
print_warn "Ready to create GitHub release v$VERSION"
read -p "Continue? [y/N]: " confirm
if [[ ! "$confirm" =~ ^[Yy]$ ]]; then
    print_info "Cancelled. Artifacts are in: $ARTIFACTS_DIR"
    print_info "You can manually upload them later."
    exit 0
fi

# Create GitHub release
print_info "Creating GitHub release v$VERSION..."

# Check if release already exists
if gh release view "v$VERSION" --repo "$REPO" &> /dev/null; then
    print_warn "Release v$VERSION already exists. Do you want to:"
    echo "  1) Delete and recreate it"
    echo "  2) Add artifacts to existing release"
    echo "  3) Cancel"
    read -p "Choice [1-3]: " choice

    case $choice in
        1)
            print_info "Deleting existing release..."
            gh release delete "v$VERSION" --repo "$REPO" --yes
            gh release create "v$VERSION" \
                --repo "$REPO" \
                --title "v$VERSION" \
                --notes "Firmware release v$VERSION" \
                "$ARTIFACTS_DIR"/*.zip
            ;;
        2)
            print_info "Uploading artifacts to existing release..."
            gh release upload "v$VERSION" \
                --repo "$REPO" \
                "$ARTIFACTS_DIR"/*.zip \
                --clobber
            ;;
        3)
            print_info "Cancelled"
            exit 0
            ;;
        *)
            print_error "Invalid choice"
            exit 1
            ;;
    esac
else
    # Create new release
    gh release create "v$VERSION" \
        --repo "$REPO" \
        --title "v$VERSION" \
        --notes "Firmware release v$VERSION

## Artifacts
- \`lite_v${VERSION}.zip\` - Lite firmware build
- \`dual_throttle_v${VERSION}.zip\` - Dual throttle firmware build

Each zip contains:
- \`bootloader.bin\` - Bootloader binary
- \`partition-table.bin\` - Partition table
- \`gb_controller_lite.bin\` or \`gb_controller_dual_throttle.bin\` - Application binary (target-specific)
- \`gb_controller_lite.elf\` or \`gb_controller_dual_throttle.elf\` - Application ELF (for coredump decoding)" \
        "$ARTIFACTS_DIR"/*.zip
fi

print_info "Release created successfully!"
print_info "View release at: https://github.com/$REPO/releases/tag/v$VERSION"

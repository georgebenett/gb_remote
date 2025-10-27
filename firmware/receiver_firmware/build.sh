#!/bin/bash
set -e
echo "Setting up ESP-IDF environment..."
. $IDF_PATH/export.sh

echo "ESP-IDF version: $(idf.py --version)"

echo "Building project..."

idf.py build

echo "Build completed successfully!"
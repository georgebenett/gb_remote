#!/usr/bin/env python3
"""
Firmware Packing Script for GB Remote Lite
==========================================

This script collects the firmware files from the build folder:
- bootloader.bin
- partition-table.bin  
- gb_controller_lite.bin

Creates a zip file named 'gb_remote_lite.zip' and moves it to the Downloads folder.
"""

import os
import zipfile
import shutil
from datetime import datetime
from pathlib import Path

def main():
    # Define paths
    script_dir = Path(__file__).parent
    build_dir = script_dir / "build"
    downloads_dir = Path.home() / "Downloads"
    
    # Source files
    source_files = {
        "bootloader.bin": build_dir / "bootloader" / "bootloader.bin",
        "partition-table.bin": build_dir / "partition_table" / "partition-table.bin",
        "gb_controller_lite.bin": build_dir / "gb_controller_lite.bin"
    }
    
    # Check if build directory exists
    if not build_dir.exists():
        print(f"❌ Error: Build directory not found at {build_dir}")
        print("Please run 'idf.py build' first to generate the firmware files.")
        return 1
    
    # Check if all source files exist
    missing_files = []
    for name, path in source_files.items():
        if not path.exists():
            missing_files.append(f"{name} at {path}")
    
    if missing_files:
        print("❌ Error: The following files are missing:")
        for file in missing_files:
            print(f"   - {file}")
        print("\nPlease run 'idf.py build' first to generate all firmware files.")
        return 1
    
    # Fixed filename as requested
    zip_filename = "gb_remote_lite.zip"
    zip_path = downloads_dir / zip_filename
    
    # Create zip file
    print(f"📦 Creating firmware package: {zip_filename}")
    print(f"📁 Destination: {zip_path}")
    
    try:
        with zipfile.ZipFile(zip_path, 'w', zipfile.ZIP_DEFLATED) as zipf:
            for name, source_path in source_files.items():
                print(f"   Adding: {name}")
                zipf.write(source_path, name)
        
        # Get file size
        zip_size = zip_path.stat().st_size
        zip_size_mb = zip_size / (1024 * 1024)
        
        print(f"\n✅ Successfully created firmware package!")
        print(f"📦 File: {zip_filename}")
        print(f"📁 Location: {zip_path}")
        print(f"📊 Size: {zip_size_mb:.2f} MB")
        print(f"\n📋 Contents:")
        print(f"   - bootloader.bin")
        print(f"   - partition-table.bin")
        print(f"   - gb_controller_lite.bin")
        
        return 0
        
    except Exception as e:
        print(f"❌ Error creating zip file: {e}")
        return 1

if __name__ == "__main__":
    exit_code = main()
    exit(exit_code)

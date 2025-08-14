#!/usr/bin/env python3
"""
Build script for creating executable from ESP32 controller script
"""

import subprocess
import sys
import os

def install_pyinstaller():
    """Install PyInstaller if not already installed"""
    try:
        import PyInstaller
        print("PyInstaller is already installed")
    except ImportError:
        print("Installing PyInstaller...")
        subprocess.check_call([sys.executable, "-m", "pip", "install", "pyinstaller"])

def build_executable():
    """Build the executable using PyInstaller"""
    print("Building executable...")
    
    # PyInstaller command with options
    cmd = [
        "pyinstaller",
        "--onefile",                    # Create a single executable file
        "--windowed",                   # Don't show console window on Windows
        "--name=ESP32_Controller",      # Name of the executable
        "--icon=icon.ico",              # Icon file (optional, remove if no icon)
        "esp32_controller.py"
    ]
    
    # Add data files with correct separator for the platform
    if os.path.exists("requirements.txt"):
        if sys.platform.startswith('win'):
            cmd.append("--add-data=requirements.txt;.")
        else:
            cmd.append("--add-data=requirements.txt:.")
    
    # Remove icon option if icon file doesn't exist
    if not os.path.exists("icon.ico"):
        cmd = [arg for arg in cmd if not arg.startswith("--icon")]
    
    # Remove add-data option if requirements.txt doesn't exist
    if not os.path.exists("requirements.txt"):
        cmd = [arg for arg in cmd if not arg.startswith("--add-data")]
    
    try:
        subprocess.check_call(cmd)
        print("Build completed successfully!")
        print("Executable created in: dist/ESP32_Controller.exe")
    except subprocess.CalledProcessError as e:
        print(f"Build failed: {e}")
        return False
    
    return True

def main():
    """Main build process"""
    print("ESP32 Controller Build Script")
    print("=" * 40)
    
    # Install PyInstaller
    install_pyinstaller()
    
    # Build executable
    if build_executable():
        print("\nBuild completed successfully!")
        print("You can find the executable in the 'dist' folder.")
        print("To run on Windows, simply double-click ESP32_Controller.exe")
    else:
        print("\nBuild failed!")
        sys.exit(1)

if __name__ == "__main__":
    main() 
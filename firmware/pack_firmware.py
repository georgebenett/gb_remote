#!/usr/bin/env python3
"""
Firmware Packing Script for GB Remote Lite
==========================================

This script collects the firmware files from the build folder:
- bootloader.bin
- partition-table.bin
- gb_controller_lite.bin

Creates a zip file named 'gb_remote_lite.zip' and moves it to the Downloads folder.
Optionally creates a GitHub release with the firmware package.
"""

import os
import zipfile
import shutil
import re
import argparse
import sys
import subprocess
from datetime import datetime
from pathlib import Path

try:
    import requests
    GITHUB_AVAILABLE = True
except ImportError:
    GITHUB_AVAILABLE = False

def read_version():
    """Read version from version.h file"""
    version_file = Path(__file__).parent / "main" / "version.h"

    if not version_file.exists():
        print("❌ Error: version.h file not found")
        return None

    try:
        with open(version_file, 'r') as f:
            content = f.read()

        # Extract version string
        version_match = re.search(r'#define APP_VERSION_STRING "([^"]+)"', content)
        if version_match:
            return version_match.group(1)
        else:
            print("❌ Error: Could not find APP_VERSION_STRING in version.h")
            return None
    except Exception as e:
        print(f"❌ Error reading version.h: {e}")
        return None

def get_github_repo():
    """Automatically detect GitHub repository from git remote"""
    try:
        # Get the remote URL
        result = subprocess.run(
            ['git', 'remote', 'get-url', 'origin'],
            capture_output=True,
            text=True,
            cwd=Path(__file__).parent
        )

        if result.returncode != 0:
            return None

        remote_url = result.stdout.strip()

        # Parse GitHub URL (supports both HTTPS and SSH formats)
        # HTTPS: https://github.com/owner/repo.git
        # SSH: git@github.com:owner/repo.git
        if 'github.com' in remote_url:
            if remote_url.startswith('https://'):
                # HTTPS format
                match = re.search(r'github\.com/([^/]+)/([^/]+?)(?:\.git)?$', remote_url)
                if match:
                    return f"{match.group(1)}/{match.group(2)}"
            elif remote_url.startswith('git@'):
                # SSH format
                match = re.search(r'github\.com:([^/]+)/([^/]+?)(?:\.git)?$', remote_url)
                if match:
                    return f"{match.group(1)}/{match.group(2)}"

        return None

    except Exception as e:
        print(f"⚠️  Warning: Could not detect GitHub repository: {e}")
        return None

def get_github_token():
    """Get GitHub token from environment variables"""
    # Try multiple common environment variable names
    token_names = ['GITHUB_TOKEN', 'GH_TOKEN', 'GITHUB_PAT']

    for token_name in token_names:
        token = os.getenv(token_name)
        if token:
            return token

    return None

def create_github_release(version, zip_path, github_token, repo_owner, repo_name):
    """Create a GitHub release with the firmware package"""
    if not GITHUB_AVAILABLE:
        print("❌ Error: requests library not available. Install with: pip install requests")
        return False

    if not github_token:
        print("❌ Error: GitHub token not provided")
        return False

    # GitHub API endpoint
    url = f"https://api.github.com/repos/{repo_owner}/{repo_name}/releases"

    # Release data
    release_data = {
        "tag_name": f"v{version}",
        "name": f"GB Remote Lite v{version}",
        "body": f"""## GB Remote Lite Firmware v{version}

### Firmware Package Contents:
- `bootloader.bin` - ESP32 bootloader
- `partition-table.bin` - Partition table
- `gb_controller_lite.bin` - Main application firmware

### Installation:
1. Visit the GB Remote Config Tool: https://georgebenett.github.io/gb_config_tool/
2. Connect your remote device via USB
3. Follow the web-based flashing interface to flash the firmware

### Changes in this version:
- See commit history for detailed changes

---
*Automatically generated release*""",
        "draft": False,
        "prerelease": False
    }

    headers = {
        "Authorization": f"token {github_token}",
        "Accept": "application/vnd.github.v3+json"
    }

    try:
        print(f"🚀 Creating GitHub release v{version}...")

        # Create the release
        response = requests.post(url, json=release_data, headers=headers)
        response.raise_for_status()

        release_info = response.json()
        release_id = release_info["id"]
        upload_url = release_info["upload_url"].replace("{?name,label}", "")

        print(f"✅ Release created: {release_info['html_url']}")

        # Upload the firmware package
        print("📤 Uploading firmware package...")

        with open(zip_path, 'rb') as f:
            upload_data = f.read()

        upload_headers = {
            "Authorization": f"token {github_token}",
            "Content-Type": "application/zip"
        }

        upload_response = requests.post(
            f"{upload_url}?name=gb_remote_lite_v{version}.zip&label=GB Remote Lite Firmware v{version}",
            data=upload_data,
            headers=upload_headers
        )
        upload_response.raise_for_status()

        print(f"✅ Firmware package uploaded successfully!")
        print(f"🔗 Release URL: {release_info['html_url']}")

        return True

    except requests.exceptions.RequestException as e:
        print(f"❌ Error creating GitHub release: {e}")
        if hasattr(e, 'response') and e.response is not None:
            print(f"Response: {e.response.text}")
        return False

def main():
    # Parse command line arguments
    parser = argparse.ArgumentParser(description='Pack GB Remote Lite firmware and optionally create GitHub release')
    parser.add_argument('--github', action='store_true', help='Create GitHub release (requires GITHUB_TOKEN env var)')
    parser.add_argument('--token', help='GitHub personal access token (overrides GITHUB_TOKEN env var)')
    parser.add_argument('--repo', help='GitHub repository in format owner/repo (auto-detected if not provided)')

    args = parser.parse_args()

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

    # Read version from version.h
    version = read_version()
    if not version:
        print("❌ Error: Could not read version from version.h")
        return 1

    print(f"📋 Version: {version}")

    # Auto-detect GitHub repository if not provided
    github_repo = args.repo
    if args.github and not github_repo:
        print("🔍 Auto-detecting GitHub repository...")
        github_repo = get_github_repo()
        if github_repo:
            print(f"✅ Detected repository: {github_repo}")
        else:
            print("❌ Error: Could not auto-detect GitHub repository")
            print("Please provide --repo owner/repo or ensure git remote is configured")
            return 1

    # Get GitHub token
    github_token = args.token or get_github_token()
    if args.github and not github_token:
        print("❌ Error: GitHub token not found")
        print("Set GITHUB_TOKEN environment variable or use --token argument")
        print("Example: export GITHUB_TOKEN=your_token_here")
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

        # Create GitHub release if requested
        if args.github:
            repo_parts = github_repo.split('/')
            if len(repo_parts) != 2:
                print("\n❌ Error: Repository format should be 'owner/repo'")
                return 1

            repo_owner, repo_name = repo_parts

            print(f"\n🚀 Creating GitHub release...")
            if create_github_release(version, zip_path, github_token, repo_owner, repo_name):
                print(f"\n🎉 Release v{version} created successfully!")
            else:
                print(f"\n❌ Failed to create GitHub release")
                return 1

        return 0

    except Exception as e:
        print(f"❌ Error creating zip file: {e}")
        return 1

if __name__ == "__main__":
    exit_code = main()
    exit(exit_code)

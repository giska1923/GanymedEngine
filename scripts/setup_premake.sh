#!/bin/bash

# Script to download and setup Premake5 for the current platform
# This ensures each platform has the native binary

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$SCRIPT_DIR/.."
PREMAKE_DIR="$PROJECT_ROOT/vendor/premake/bin"

# Create the bin directory if it doesn't exist
mkdir -p "$PREMAKE_DIR"

# Detect the current platform
if [[ "$OSTYPE" == "darwin"* ]]; then
    PLATFORM="macosx"
    BINARY_NAME="premake5"
    DOWNLOAD_URL="https://github.com/premake/premake-core/releases/download/v5.0.0-beta2/premake-5.0.0-beta2-macosx.tar.gz"
elif [[ "$OSTYPE" == "linux-gnu"* ]]; then
    PLATFORM="linux"
    BINARY_NAME="premake5"
    DOWNLOAD_URL="https://github.com/premake/premake-core/releases/download/v5.0.0-beta2/premake-5.0.0-beta2-linux.tar.gz"
elif [[ "$OSTYPE" == "msys" ]] || [[ "$OSTYPE" == "cygwin" ]] || [[ "$OSTYPE" == "win32" ]]; then
    PLATFORM="windows"
    BINARY_NAME="premake5.exe"
    DOWNLOAD_URL="https://github.com/premake/premake-core/releases/download/v5.0.0-beta2/premake-5.0.0-beta2-windows.zip"
else
    echo "Unsupported platform: $OSTYPE"
    exit 1
fi

echo "Detected platform: $PLATFORM"

# Check if the binary already exists
if [ -f "$PREMAKE_DIR/$BINARY_NAME" ]; then
    echo "Premake5 binary already exists for $PLATFORM"
    echo "Path: $PREMAKE_DIR/$BINARY_NAME"
    exit 0
fi

echo "Downloading Premake5 for $PLATFORM..."

# Create a temporary directory
TEMP_DIR=$(mktemp -d)

# Download the appropriate version
if command -v curl >/dev/null 2>&1; then
    curl -L "$DOWNLOAD_URL" -o "$TEMP_DIR/premake5_archive"
elif command -v wget >/dev/null 2>&1; then
    wget "$DOWNLOAD_URL" -O "$TEMP_DIR/premake5_archive"
else
    echo "Error: Neither curl nor wget found. Please install one of them."
    exit 1
fi

# Extract the archive
cd "$TEMP_DIR"
if [[ "$PLATFORM" == "windows" ]]; then
    if command -v unzip >/dev/null 2>&1; then
        unzip premake5_archive
    else
        echo "Error: unzip not found. Please install unzip or manually download Premake5."
        exit 1
    fi
else
    tar -xzf premake5_archive
fi

# Copy the binary to the vendor directory
if [ -f "$BINARY_NAME" ]; then
    cp "$BINARY_NAME" "$PREMAKE_DIR/"
    chmod +x "$PREMAKE_DIR/$BINARY_NAME"
    echo "Successfully installed Premake5 to $PREMAKE_DIR/$BINARY_NAME"
else
    echo "Error: Could not find $BINARY_NAME in the extracted files"
    exit 1
fi

# Clean up
rm -rf "$TEMP_DIR"

echo "Premake5 setup complete for $PLATFORM!"
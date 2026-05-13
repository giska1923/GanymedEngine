#!/bin/bash

# Get the directory where this script is located
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Change to the project root directory (one level up from scripts)
cd "$SCRIPT_DIR/.."

# Verify we're on macOS
if [[ "$OSTYPE" != "darwin"* ]]; then
    echo "This script is intended for macOS. Use Linux_GenerateProjects.sh for Linux."
    exit 1
fi

echo "Generating project files for macOS using Premake5..."

# Check for native macOS premake5 binary first
if [ -f "vendor/premake/bin/premake5" ] && [ -x "vendor/premake/bin/premake5" ]; then
    echo "Using native macOS Premake5 binary..."
    echo "Generating Xcode project files..."
    vendor/premake/bin/premake5 xcode4
    
    if [ $? -eq 0 ]; then
        echo "Xcode project files generated successfully!"
        echo ""
        echo "You can now:"
        echo "  1. Open GanymedEngine.xcworkspace in Xcode"
        echo "  2. Or build from command line with xcodebuild"
    else
        echo "Xcode generation failed. Falling back to Makefiles..."
        vendor/premake/bin/premake5 gmake2
        echo "Makefiles generated instead."
        echo "To build, run: make config=debug"
    fi
    
elif [ -f "vendor/premake/bin/premake5.exe" ]; then
    echo "Native macOS binary not found. Attempting to use Windows binary with Wine..."
    if command -v wine >/dev/null 2>&1; then
        echo "Generating Makefiles (Wine doesn't support Xcode generation)..."
        wine vendor/premake/bin/premake5.exe gmake2
        echo "Makefiles generated successfully!"
        echo "To build, run: make config=debug"
    else
        echo "Wine not found. Please install Wine or run the setup script:"
        echo "  ./scripts/setup_premake.sh"
        exit 1
    fi
else
    echo "No Premake5 binary found. Please run the setup script first:"
    echo "  ./scripts/setup_premake.sh"
    exit 1
fi

echo ""
echo "Available build configurations:"
echo "  Debug   - Debug build with symbols"
echo "  Release - Optimized release build"  
echo "  Dist    - Distribution build"
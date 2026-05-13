#!/bin/bash

# Get the directory where this script is located
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Change to the project root directory (one level up from scripts)
cd "$SCRIPT_DIR/.."

echo "Generating Linux Makefiles using Premake5..."

# Check for native Linux premake5 binary first
if [ -f "vendor/premake/bin/premake5" ] && [ -x "vendor/premake/bin/premake5" ]; then
    echo "Using native Linux Premake5 binary..."
    vendor/premake/bin/premake5 gmake2
elif [ -f "vendor/premake/bin/premake5.exe" ]; then
    echo "Native Linux binary not found. Attempting to use Windows binary with Wine..."
    if command -v wine >/dev/null 2>&1; then
        wine vendor/premake/bin/premake5.exe gmake2
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

echo "Project files generated successfully!"
echo ""
echo "To build the project:"
echo "  make config=debug        # Debug build"
echo "  make config=release      # Release build"
echo "  make config=dist         # Distribution build"
echo ""
echo "To build with multiple cores:"
echo "  make -j\$(nproc) config=debug"
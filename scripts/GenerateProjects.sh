#!/bin/bash

# Cross-platform project generation script for GanymedEngine
# Automatically detects the platform and generates appropriate project files

# Get the directory where this script is located
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Change to the project root directory (one level up from scripts)
cd "$SCRIPT_DIR/.."

echo "GanymedEngine - Cross-Platform Project Generator"
echo "==============================================="

# Detect the current platform
if [[ "$OSTYPE" == "darwin"* ]]; then
    PLATFORM="macOS"
    TARGET_GENERATOR="xcode4"
    FALLBACK_GENERATOR="gmake2"
    BINARY_NAME="premake5"
elif [[ "$OSTYPE" == "linux-gnu"* ]]; then
    PLATFORM="Linux"
    TARGET_GENERATOR="gmake2"
    FALLBACK_GENERATOR="gmake2"
    BINARY_NAME="premake5"
elif [[ "$OSTYPE" == "msys" ]] || [[ "$OSTYPE" == "cygwin" ]]; then
    PLATFORM="Windows (MSYS/Cygwin)"
    TARGET_GENERATOR="vs2022"
    FALLBACK_GENERATOR="gmake2"
    BINARY_NAME="premake5.exe"
else
    echo "Unsupported platform: $OSTYPE"
    echo "Supported platforms: macOS, Linux, Windows"
    exit 1
fi

echo "Detected platform: $PLATFORM"
echo "Target generator: $TARGET_GENERATOR"
echo ""

# Function to check and setup premake if needed
setup_premake_if_needed() {
    local native_binary="vendor/premake/bin/$BINARY_NAME"
    local windows_binary="vendor/premake/bin/premake5.exe"
    
    # Check if native binary exists and is executable
    if [ -f "$native_binary" ] && [ -x "$native_binary" ]; then
        echo "✓ Native Premake5 binary found"
        return 0
    fi
    
    # Check if Windows binary exists (fallback)
    if [ -f "$windows_binary" ]; then
        echo "✓ Windows Premake5 binary found (fallback available)"
        return 0
    fi
    
    # No suitable binary found
    echo "✗ No Premake5 binary found"
    echo ""
    echo "Would you like to automatically download and setup Premake5? (y/n)"
    read -r response
    if [[ "$response" =~ ^[Yy]$ ]]; then
        echo "Running setup script..."
        if [ -f "scripts/setup_premake.sh" ]; then
            bash scripts/setup_premake.sh
            return $?
        else
            echo "Setup script not found. Please manually install Premake5."
            return 1
        fi
    else
        echo "Please run: ./scripts/setup_premake.sh"
        return 1
    fi
}

# Generate project files
generate_projects() {
    local native_binary="vendor/premake/bin/$BINARY_NAME"
    local windows_binary="vendor/premake/bin/premake5.exe"
    
    echo "Generating project files..."
    
    # Try native binary first
    if [ -f "$native_binary" ] && [ -x "$native_binary" ]; then
        echo "Using native Premake5 binary..."
        "$native_binary" $TARGET_GENERATOR
        
        if [ $? -eq 0 ]; then
            echo "✓ Project files generated successfully with $TARGET_GENERATOR"
            return 0
        else
            echo "⚠ Failed with $TARGET_GENERATOR, trying fallback..."
            "$native_binary" $FALLBACK_GENERATOR
            if [ $? -eq 0 ]; then
                echo "✓ Project files generated successfully with $FALLBACK_GENERATOR"
                return 0
            fi
        fi
    fi
    
    # Fallback to Windows binary with Wine (Unix systems only)
    if [ -f "$windows_binary" ] && [[ "$OSTYPE" != "msys" ]] && [[ "$OSTYPE" != "cygwin" ]]; then
        if command -v wine >/dev/null 2>&1; then
            echo "Using Windows binary with Wine..."
            wine "$windows_binary" $FALLBACK_GENERATOR
            if [ $? -eq 0 ]; then
                echo "✓ Project files generated successfully with Wine"
                return 0
            fi
        else
            echo "Wine not available for Windows binary fallback"
        fi
    fi
    
    echo "✗ Failed to generate project files"
    return 1
}

# Print build instructions
print_build_instructions() {
    echo ""
    echo "Build Instructions:"
    echo "==================="
    
    case $PLATFORM in
        "macOS")
            if [ -f "GanymedEngine.xcworkspace" ] || [ -f "GanymedEngine.xcodeproj" ]; then
                echo "Xcode project generated:"
                echo "  • Open GanymedEngine.xcworkspace (or .xcodeproj) in Xcode"
                echo "  • Or build from command line: xcodebuild -configuration Debug"
            elif [ -f "Makefile" ]; then
                echo "Makefiles generated:"
                echo "  • make config=debug"
                echo "  • make config=release"
                echo "  • make config=dist"
                echo "  • For faster builds: make -j\$(sysctl -n hw.ncpu) config=debug"
            fi
            ;;
        "Linux")
            echo "Makefiles generated:"
            echo "  • make config=debug        # Debug build"
            echo "  • make config=release      # Release build"  
            echo "  • make config=dist         # Distribution build"
            echo "  • For faster builds: make -j\$(nproc) config=debug"
            ;;
        "Windows"*)
            echo "Visual Studio solution generated:"
            echo "  • Open GanymedEngine.sln in Visual Studio"
            echo "  • Or use MSBuild from command line"
            ;;
    esac
    
    echo ""
    echo "Available configurations: Debug, Release, Dist"
}

# Main execution
main() {
    # Setup premake if needed
    if ! setup_premake_if_needed; then
        exit 1
    fi
    
    # Generate project files
    if generate_projects; then
        print_build_instructions
        echo ""
        echo "✓ Project generation completed successfully!"
    else
        echo ""
        echo "✗ Project generation failed!"
        echo "Please check the error messages above and ensure Premake5 is properly installed."
        exit 1
    fi
}

# Run main function
main "$@"
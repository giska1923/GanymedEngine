#!/bin/bash

echo "=== GanymedEngine - Dependency Setup Script ==="
echo ""

# Get script location and change to project root
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR/.."

echo "Project root: $(pwd)"
echo ""

# Check if we're in a git repository
if [ ! -d ".git" ]; then
    echo "❌ Not in a git repository! Please run this from the GanymedEngine project root."
    exit 1
fi

echo "🔍 Checking git submodules..."

# Check if .gitmodules exists
if [ ! -f ".gitmodules" ]; then
    echo "❌ No .gitmodules file found. This project may not use git submodules."
    exit 1
fi

echo "📋 Found .gitmodules file:"
cat .gitmodules
echo ""

echo "🚀 Initializing and updating git submodules..."
echo "This may take a few minutes depending on your internet connection..."
echo ""

# Initialize and update submodules
git submodule update --init --recursive

if [ $? -eq 0 ]; then
    echo ""
    echo "✅ Git submodules updated successfully!"
else
    echo ""
    echo "❌ Failed to update git submodules. Please check your git configuration and internet connection."
    echo ""
    echo "You can try manually running:"
    echo "  git submodule init"
    echo "  git submodule update --recursive"
    exit 1
fi

echo ""
echo "🔍 Verifying dependencies..."

# Check key dependencies
deps_ok=true

if [ ! -f "GanymedEngine/extern/GLFW/premake5.lua" ]; then
    echo "❌ GLFW premake5.lua still missing"
    deps_ok=false
else
    echo "✅ GLFW dependency ready"
fi

if [ ! -f "GanymedEngine/extern/Glad/premake5.lua" ]; then
    echo "❌ Glad premake5.lua missing"
    deps_ok=false
else
    echo "✅ Glad dependency ready"
fi

if [ ! -f "GanymedEngine/extern/imgui/premake5.lua" ]; then
    echo "❌ ImGui premake5.lua missing"
    deps_ok=false
else
    echo "✅ ImGui dependency ready"
fi

if [ ! -d "GanymedEngine/extern/glm/glm" ]; then
    echo "❌ GLM headers missing"
    deps_ok=false
else
    echo "✅ GLM dependency ready"
fi

echo ""
if [ "$deps_ok" = true ]; then
    echo "🎉 All dependencies are ready!"
    echo ""
    echo "Next steps:"
    echo "1. Setup Premake5: ./scripts/setup_premake_Unix.sh"
    echo "2. Generate projects: ./scripts/GenerateProjects_Unix.sh"
else
    echo "⚠️  Some dependencies are still missing. You may need to:"
    echo "1. Check your internet connection"
    echo "2. Ensure git credentials are set up for GitHub"
    echo "3. Try running: git submodule update --init --recursive --force"
fi
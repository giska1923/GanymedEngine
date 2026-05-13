#!/bin/bash

echo "=== GanymedEngine - Submodule Fix Script ==="
echo ""

# Get script location and change to project root
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR/.."

echo "Project root: $(pwd)"
echo ""

echo "🔧 Fixing git submodule issues..."
echo ""

# Function to fix a specific submodule
fix_submodule() {
    local submodule_path="$1"
    local repo_url="$2"
    local submodule_name=$(basename "$submodule_path")
    
    echo "📦 Fixing $submodule_name..."
    
    # Remove the problematic submodule directory
    if [ -d "$submodule_path" ]; then
        echo "  🗑️  Removing existing directory: $submodule_path"
        rm -rf "$submodule_path"
    fi
    
    # Remove from git index if it exists
    git rm --cached "$submodule_path" 2>/dev/null || true
    
    # Re-add the submodule with the latest version
    echo "  📥 Cloning fresh copy from $repo_url"
    git submodule add --force "$repo_url" "$submodule_path"
    
    if [ $? -eq 0 ]; then
        echo "  ✅ $submodule_name fixed successfully"
        return 0
    else
        echo "  ❌ Failed to fix $submodule_name"
        return 1
    fi
}

# Alternative approach: Clone dependencies directly without submodules
clone_dependency() {
    local path="$1"
    local url="$2"
    local name=$(basename "$path")
    
    echo "📦 Cloning $name directly..."
    
    # Remove existing directory
    if [ -d "$path" ]; then
        rm -rf "$path"
    fi
    
    # Clone the repository
    git clone "$url" "$path"
    
    if [ $? -eq 0 ]; then
        echo "  ✅ $name cloned successfully"
        
        # Remove the .git directory to make it part of our repo (not a submodule)
        rm -rf "$path/.git"
        echo "  📝 Converted to regular directory (not submodule)"
        return 0
    else
        echo "  ❌ Failed to clone $name"
        return 1
    fi
}

echo "Choose your preferred approach:"
echo "1. Fix submodules (recommended for development)"
echo "2. Clone dependencies directly (simpler, but not submodules)"
echo "3. Manual setup (just create directories for you to handle manually)"
echo ""
read -p "Enter your choice (1-3): " choice

case $choice in
    1)
        echo ""
        echo "🔄 Attempting to fix submodules..."
        echo ""
        
        # Try to fix each submodule
        success=true
        
        if ! fix_submodule "GanymedEngine/extern/GLFW" "https://github.com/glfw/glfw"; then
            success=false
        fi
        
        if ! fix_submodule "GanymedEngine/extern/imgui" "https://github.com/ocornut/imgui"; then
            success=false
        fi
        
        if ! fix_submodule "GanymedEngine/extern/glm" "https://github.com/g-truc/glm"; then
            success=false
        fi
        
        if ! fix_submodule "GanymedEngine/extern/spdlog" "https://github.com/gabime/spdlog"; then
            success=false
        fi
        
        if [ "$success" = true ]; then
            echo ""
            echo "🎉 All submodules fixed!"
        else
            echo ""
            echo "⚠️  Some submodules failed. Try option 2 instead."
        fi
        ;;
        
    2)
        echo ""
        echo "📥 Cloning dependencies directly..."
        echo ""
        
        # Clone each dependency directly
        clone_dependency "GanymedEngine/extern/GLFW" "https://github.com/glfw/glfw"
        clone_dependency "GanymedEngine/extern/imgui" "https://github.com/ocornut/imgui"  
        clone_dependency "GanymedEngine/extern/glm" "https://github.com/g-truc/glm"
        clone_dependency "GanymedEngine/extern/spdlog" "https://github.com/gabime/spdlog"
        
        echo ""
        echo "🎉 Dependencies cloned directly!"
        echo "Note: These are now regular directories, not git submodules."
        ;;
        
    3)
        echo ""
        echo "📝 Creating directory structure for manual setup..."
        
        # Create directories
        mkdir -p "GanymedEngine/extern/GLFW"
        mkdir -p "GanymedEngine/extern/imgui" 
        mkdir -p "GanymedEngine/extern/glm"
        mkdir -p "GanymedEngine/extern/spdlog"
        mkdir -p "GanymedEngine/extern/Glad"
        mkdir -p "GanymedEngine/extern/yaml-cpp"
        mkdir -p "GanymedEngine/extern/entt"
        mkdir -p "GanymedEngine/extern/stb_image"
        
        echo "✅ Directories created. You need to manually download:"
        echo "  - GLFW: https://github.com/glfw/glfw"
        echo "  - ImGui: https://github.com/ocornut/imgui"
        echo "  - GLM: https://github.com/g-truc/glm"  
        echo "  - spdlog: https://github.com/gabime/spdlog"
        echo "And extract them to their respective directories."
        ;;
        
    *)
        echo "Invalid choice. Exiting."
        exit 1
        ;;
esac

echo ""
echo "🔍 Verifying setup..."

# Check if key files exist
if [ -f "GanymedEngine/extern/GLFW/CMakeLists.txt" ] || [ -f "GanymedEngine/extern/GLFW/premake5.lua" ]; then
    echo "✅ GLFW appears to be set up correctly"
else
    echo "❌ GLFW setup may have failed"
fi

if [ -f "GanymedEngine/extern/imgui/imgui.h" ]; then
    echo "✅ ImGui appears to be set up correctly"  
else
    echo "❌ ImGui setup may have failed"
fi

echo ""
echo "Next steps:"
echo "1. Setup Premake5: ./scripts/setup_premake_Unix.sh"
echo "2. Generate projects: ./scripts/GenerateProjects_Unix.sh"
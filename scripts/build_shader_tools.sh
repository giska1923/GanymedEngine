#!/bin/bash
# Builds bgfx's shaderc (offline shader compiler) and stages it in scripts/tools/<os>.
#
# shaderc is built with bgfx's own GENie project rather than our premake5 setup:
# it pulls in glslang, spirv-tools, spirv-cross, glsl-optimizer, fcpp and tint,
# and re-describing that dependency graph in premake would be a large, fragile
# duplication for a tool that is built once and rarely changes.
#
# The binaries are gitignored, so run this once per machine / after a bgfx
# submodule update.

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$SCRIPT_DIR/.."
BGFX="$ROOT/GanymedEngine/extern/bgfx"

if [ ! -f "$BGFX/src/amalgamated.cpp" ]; then
    echo "❌ bgfx submodule not checked out. Run: git submodule update --init --recursive"
    exit 1
fi

case "$(uname -s)" in
    Linux*)
        GENIE="$ROOT/GanymedEngine/extern/bx/tools/bin/linux/genie"
        GENIE_ARGS="--gcc=linux-gcc"
        PROJ_DIR="gmake-linux"
        OUT="$ROOT/scripts/tools/linux"
        ;;
    Darwin*)
        GENIE="$ROOT/GanymedEngine/extern/bx/tools/bin/darwin/genie"
        if [ "$(uname -m)" = "arm64" ]; then
            GENIE_ARGS="--gcc=osx-arm64"
            PROJ_DIR="gmake-osx-arm64"
        else
            GENIE_ARGS="--gcc=osx-x64"
            PROJ_DIR="gmake-osx-x64"
        fi
        OUT="$ROOT/scripts/tools/darwin"
        ;;
    *)
        echo "❌ Unsupported platform $(uname -s). On Windows use scripts/build_shader_tools.bat"
        exit 1
        ;;
esac

echo "[1/3] Generating bgfx tool projects with GENie..."
cd "$BGFX"
chmod +x "$GENIE" 2>/dev/null || true
"$GENIE" --with-tools $GENIE_ARGS gmake > /dev/null

echo "[2/3] Building shaderc (release64)... this takes a few minutes."
make -C ".build/projects/$PROJ_DIR" config=release64 -j"$(getconf _NPROCESSORS_ONLN)" shaderc

echo "[3/3] Staging into $OUT..."
mkdir -p "$OUT"
BUILT=$(find "$BGFX/.build" -name "shadercRelease" -type f | head -1)
if [ -z "$BUILT" ]; then
    echo "❌ Could not locate the built shaderc binary."
    exit 1
fi
cp "$BUILT" "$OUT/shaderc"
chmod +x "$OUT/shaderc"

echo ""
echo "Done: $OUT/shaderc"
"$OUT/shaderc" --version

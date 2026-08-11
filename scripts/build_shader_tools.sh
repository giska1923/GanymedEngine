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
        BUNDLED_GENIE="$ROOT/GanymedEngine/extern/bx/tools/bin/linux/genie"
        GCC_TOOLCHAIN="linux-gcc"
        EXTRA_GENIE_ARGS=""
        OUT="$ROOT/scripts/tools/linux"
        ;;
    Darwin*)
        BUNDLED_GENIE="$ROOT/GanymedEngine/extern/bx/tools/bin/darwin/genie"
        if [ "$(uname -m)" = "arm64" ]; then
            GCC_TOOLCHAIN="osx-arm64"
        else
            GCC_TOOLCHAIN="osx-x64"
        fi
        # bx defaults the macOS target to 10.13.6 for the gmake action (its newer defaults
        # only apply to the xcode* actions), and glslang uses std::filesystem, which libc++
        # marks unavailable before 10.15 - so the tool build fails on 'absolute' is
        # unavailable unless the target is raised. 13.0 is what bx's own --with-macos help
        # claims as the default.
        EXTRA_GENIE_ARGS="--with-macos=13.0"
        OUT="$ROOT/scripts/tools/darwin"
        ;;
    *)
        echo "❌ Unsupported platform $(uname -s). On Windows use scripts/build_shader_tools.bat"
        exit 1
        ;;
esac

GENIE_ARGS="--gcc=$GCC_TOOLCHAIN $EXTRA_GENIE_ARGS"

# bx puts the generated makefiles in .build/projects/<action>-<gcc value> (toolchain.lua),
# so the directory name is derived rather than spelled out - hardcoding it silently drifts
# from the --gcc value and make then fails with "No such file or directory".
PROJ_DIR="gmake-$GCC_TOOLCHAIN"

# bx bundles prebuilt GENie binaries, and they do not run everywhere: the darwin one is
# arm64-only, so an Intel Mac reports "Bad CPU type in executable", and the linux one is
# linked against glibc 2.38, so anything older than Ubuntu 24.04 fails in the loader.
# GENie itself is a small C/Lua project that builds in seconds, so the escape hatch is to
# build it and point GENIE at the result:
#
#   git clone https://github.com/bkaradzic/GENie && make -C GENie
#   GENIE=/path/to/GENie/bin/darwin/genie ./scripts/build_shader_tools.sh
GENIE="${GENIE:-$BUNDLED_GENIE}"
chmod +x "$GENIE" 2>/dev/null || true

# --version exits non-zero even on success, so check that it actually produced output:
# that distinguishes "ran fine" from "could not exec" and "loader rejected it" alike.
set +e
GENIE_CHECK="$("$GENIE" --version 2>/dev/null)"
set -e
case "$GENIE_CHECK" in
    *GENie*) ;;
    *)
        echo "❌ $GENIE will not run on this machine."
        echo "   Build GENie from source and re-run with GENIE=<path> (see comment above)."
        exit 1
        ;;
esac

echo "[1/3] Generating bgfx tool projects with GENie..."
cd "$BGFX"
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

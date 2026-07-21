#!/bin/bash
# Compiles every .sc shader in assets/shaders/src with bgfx's shaderc, once per
# backend profile, into each app's assets/shaders/compiled/<profile>/ folder.
#
# Shaders are no longer compiled at runtime: bgfx consumes pre-compiled
# bytecode, so this has to run after any shader edit. Run
# scripts/build_shader_tools.sh first if shaderc is missing.
#
# Profile -> folder mapping must match ProfileDirectory() in Shader.cpp.
#
# The profile set is per-OS because the folder is chosen at runtime from the
# live bgfx backend: Metal on macOS, Vulkan or GL on Linux. dx11 is Windows-only
# and is deliberately absent here (see scripts/compile_shaders.bat).

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$SCRIPT_DIR/.."
SRC="$ROOT/assets/shaders/src"
INCLUDE="$ROOT/GanymedEngine/extern/bgfx/src"

case "$(uname -s)" in
    Linux*)
        SHADERC="$ROOT/scripts/tools/linux/shaderc"
        PLATFORM="linux"
        # folder:shaderc-profile
        PROFILES=("spirv:spirv" "glsl:410")
        ;;
    Darwin*)
        SHADERC="$ROOT/scripts/tools/darwin/shaderc"
        PLATFORM="osx"
        # "metal" is shaderc's alias for Metal 1.2; glsl covers the GL fallback,
        # which macOS caps at 4.1.
        PROFILES=("metal:metal" "glsl:410")
        ;;
    *)
        echo "Unsupported platform $(uname -s). On Windows use scripts/compile_shaders.bat"
        exit 1
        ;;
esac

if [ ! -x "$SHADERC" ]; then
    echo "[ERROR] shaderc not found at $SHADERC"
    echo "        Run scripts/build_shader_tools.sh first."
    exit 1
fi

# Every app that loads shaders at runtime gets its own compiled copy,
# because assets are resolved relative to the working directory.
TARGETS=("$ROOT/GanymedEditor/assets/shaders/compiled" "$ROOT/Sandbox/assets/shaders/compiled")

FAILED=0
BUILT=0

# Shaders whose attributes differ from the engine's standard vertex layout
# (ImGui, for one) can ship varying.<name>.def.sc and it is preferred.
# $1 is the file stem, e.g. "vs_ImGui" -> name "ImGui".
varyingdef() {
    local name="${1:3}"
    if [ -f "$SRC/varying.$name.def.sc" ]; then
        echo "$SRC/varying.$name.def.sc"
    else
        echo "$SRC/varying.def.sc"
    fi
}

compile_profile() {
    local outdir="$1/$2"
    local profile="$3"

    mkdir -p "$outdir"

    local type
    for type in vertex fragment; do
        local prefix="vs_"
        [ "$type" = "fragment" ] && prefix="fs_"

        local file
        for file in "$SRC/$prefix"*.sc; do
            [ -e "$file" ] || continue
            local stem
            stem="$(basename "$file" .sc)"

            if "$SHADERC" -f "$file" -o "$outdir/$stem.bin" --type "$type" \
                --platform "$PLATFORM" -p "$profile" -i "$INCLUDE" \
                --varyingdef "$(varyingdef "$stem")"; then
                BUILT=$((BUILT + 1))
            else
                echo "  FAILED $2/$stem"
                FAILED=$((FAILED + 1))
            fi
        done
    done
}

for target in "${TARGETS[@]}"; do
    for entry in "${PROFILES[@]}"; do
        compile_profile "$target" "${entry%%:*}" "${entry##*:}"
    done
done

echo ""
if [ "$FAILED" -eq 0 ]; then
    echo "Compiled $BUILT shader binaries."
else
    echo "[ERROR] $FAILED shader(s) failed to compile."
    exit 1
fi

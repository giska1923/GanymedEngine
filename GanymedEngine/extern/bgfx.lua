-- Build scripts for the bx / bimg / bgfx submodules. Lives outside the submodules
-- because git cannot track files inside a submodule's working tree.
--
-- These replace bgfx's own GENie build (scripts/genie.lua) so the whole solution
-- generates from one premake5 pass. Each library uses its amalgamated translation
-- unit, which is what bgfx's `--with-amalgamated` option does.
--
-- Note: bx requires C++20 to compile. The engine stays on C++17 -- this is safe
-- because <bgfx/bgfx.h> pulls in no bx headers, only <stdint.h> and "defines.h".
--
-- bimg_decode (PNG/JPEG/EXR/AVIF decoding) is deliberately NOT built: the engine
-- loads images through stb_image. Add it only if we adopt KTX/DDS via bimg.

local BX_DIR   = "bx"
local BIMG_DIR = "bimg"
local BGFX_DIR = "bgfx"

-- Settings bx's own toolchain applies to every project it builds.
local function bxDefines()
	-- bx uses SEH (__try) in thread.cpp, which MSVC rejects when object unwinding
	-- is on. Only a codegen flag -- we deliberately do NOT define _HAS_EXCEPTIONS=0,
	-- which would change STL layout and break ABI against the engine.
	exceptionhandling "Off"

	defines
	{
		"__STDC_LIMIT_MACROS",
		"__STDC_FORMAT_MACROS",
		"__STDC_CONSTANT_MACROS"
	}

	filter "system:windows"
		defines
		{
			"_SCL_SECURE_NO_WARNINGS",
			"_CRT_SECURE_NO_WARNINGS",
			"_CRT_SECURE_NO_DEPRECATE",
			"NOMINMAX",
			"WIN32_LEAN_AND_MEAN"
		}
		-- bx ships MSVC shims for headers the CRT lacks (alloca.h, dirent.h, ...)
		includedirs { BX_DIR .. "/include/compat/msvc" }
		-- bx/platform.h hard-errors without the conforming preprocessor.
		buildoptions { "/Zc:__cplusplus", "/Zc:preprocessor" }

	-- Same idea on the other side: allocator.cpp includes <malloc.h>, which libc does
	-- not ship here, and bx supplies the shim. bx's own genie build adds this for
	-- macosx too; it was missing, so bx never compiled on this platform.
	filter "system:macosx"
		includedirs { BX_DIR .. "/include/compat/osx" }
		externalincludedirs { BX_DIR .. "/include/compat/osx" }

	-- And the third: dxgi.h includes <sal.h>, which bx shims here. bgfx enables the D3D11
	-- and D3D12 renderers on Linux by default (src/config.h - they run over vkd3d), so
	-- dxgi.cpp is compiled even though nothing here will ever select a D3D backend.
	-- upstream bx adds this dir for every linux target.
	--
	-- -msse4.2 is upstream's baseline too, and it is not optional: bx's simd128_selb is
	-- inline and reaches for _mm_blendv_ps, which gcc refuses to inline without SSE4.1.
	-- It has to be identical across bx/bimg/bgfx or the BX_SIMD_* selection in those
	-- inline headers diverges between the three static libs.
	filter "system:linux"
		pic "On"
		includedirs { BX_DIR .. "/include/compat/linux" }
		buildoptions { "-msse4.2", "-mfpmath=sse" }

	filter {}

	filter "configurations:Debug"
		defines { "BX_CONFIG_DEBUG=1" }
		runtime "Debug"
		symbols "on"

	filter "configurations:Release"
		defines { "BX_CONFIG_DEBUG=0" }
		runtime "Release"
		optimize "on"

	filter "configurations:Dist"
		defines { "BX_CONFIG_DEBUG=0" }
		runtime "Release"
		optimize "on"
		symbols "off"

	filter {}
end

project "bx"
	kind "StaticLib"
	language "C++"
	cppdialect "C++20"
	staticruntime "off"
	warnings "Off"
	floatingpoint "Fast"

	targetdir ("%{wks.location}/bin/" .. outputdir .. "/%{prj.name}")
	objdir ("%{wks.location}/temp/" .. outputdir .. "/%{prj.name}")

	files
	{
		BX_DIR .. "/include/**.h",
		BX_DIR .. "/include/**.inl",
		BX_DIR .. "/src/amalgamated.cpp",
		BX_DIR .. "/scripts/**.natvis"
	}

	angledIncludeDirs
	{
		BX_DIR .. "/include",
		BX_DIR .. "/3rdparty"
	}

	bxDefines()

	filter "system:windows"
		systemversion "latest"

project "bimg"
	kind "StaticLib"
	language "C++"
	cppdialect "C++20"
	staticruntime "off"
	warnings "Off"
	-- astc-encoder produces wrong output under fast math, so no floatingpoint "Fast" here.

	targetdir ("%{wks.location}/bin/" .. outputdir .. "/%{prj.name}")
	objdir ("%{wks.location}/temp/" .. outputdir .. "/%{prj.name}")

	-- image.cpp alone is what bgfx needs; image_encode.cpp and the block compressors under it
	-- are here for the asset pipeline's TextureCompiler (docs/engine/assets.md). Upstream keeps
	-- them in a separate `bimg_encode` project, and they are folded in here instead because a
	-- fourth vendored project buys nothing when both halves are already C++20 with the same
	-- include paths.
	--
	-- What is deliberately NOT built is upstream's `bimg_decode`: it drags in dav1d and libavif
	-- for AV1 support. Ganymed decodes PNG and JPEG with the stb_image it already vendors and
	-- hands bimg raw RGBA8, so `bimg::imageParse` is never called.
	files
	{
		BIMG_DIR .. "/include/**.h",
		BIMG_DIR .. "/src/image.cpp",
		BIMG_DIR .. "/src/image_encode.cpp",
		BIMG_DIR .. "/src/image_cubemap_filter.cpp",
		BIMG_DIR .. "/src/bimg_p.h",
		BIMG_DIR .. "/src/config.h",
		BIMG_DIR .. "/3rdparty/astc-encoder/source/**.cpp",
		BIMG_DIR .. "/3rdparty/astc-encoder/source/**.h",
		BIMG_DIR .. "/3rdparty/libsquish/**.cpp",
		BIMG_DIR .. "/3rdparty/libsquish/**.h",
		BIMG_DIR .. "/3rdparty/edtaa3/**.cpp",
		BIMG_DIR .. "/3rdparty/edtaa3/**.h",
		BIMG_DIR .. "/3rdparty/etc1/**.cpp",
		BIMG_DIR .. "/3rdparty/etc1/**.h",
		BIMG_DIR .. "/3rdparty/etcpak/**.cpp",
		BIMG_DIR .. "/3rdparty/etcpak/**.hpp",
		BIMG_DIR .. "/3rdparty/nvtt/**.cpp",
		BIMG_DIR .. "/3rdparty/nvtt/**.h",
		BIMG_DIR .. "/3rdparty/pvrtc/**.cpp",
		BIMG_DIR .. "/3rdparty/pvrtc/**.h",
		BIMG_DIR .. "/3rdparty/iqa/include/**.h",
		BIMG_DIR .. "/3rdparty/iqa/source/**.c"
	}

	angledIncludeDirs
	{
		BIMG_DIR .. "/include",
		BIMG_DIR .. "/3rdparty",
		BIMG_DIR .. "/3rdparty/astc-encoder/include",
		BIMG_DIR .. "/3rdparty/iqa/include",
		BIMG_DIR .. "/3rdparty/nvtt",
		BIMG_DIR .. "/3rdparty/tinyexr/deps/miniz",
		BX_DIR .. "/include"
	}

	defines
	{
		"ASTCENC_F16C=0",
		"ASTCENC_NEON=0"
	}

	bxDefines()

	-- Optimised even in Debug, and this one is not a nicety. The BC7 encoder under
	-- image_encode.cpp is nvtt's, and unoptimised it is roughly two orders of magnitude
	-- slower - a 2560x1664 texture did not finish compiling in four minutes, which makes the
	-- asset compiler unusable in the configuration everyone develops in. Nobody steps through
	-- a vendored block compressor, so the symbols that stay on are enough. Placed after
	-- bxDefines() so it overrides the shared Debug filter for this project only.
	filter "configurations:Debug"
		optimize "Speed"
		-- MSVC rejects /O2 alongside /RTC1, which premake's Debug default turns on.
		runtime "Debug"
		flags { "NoRuntimeChecks" }

	filter "system:windows"
		systemversion "latest"

project "bgfx"
	kind "StaticLib"
	language "C++"
	cppdialect "C++20"
	staticruntime "off"
	warnings "Off"
	floatingpoint "Fast"

	targetdir ("%{wks.location}/bin/" .. outputdir .. "/%{prj.name}")
	objdir ("%{wks.location}/temp/" .. outputdir .. "/%{prj.name}")

	files
	{
		BGFX_DIR .. "/include/**.h",
		BGFX_DIR .. "/src/amalgamated.cpp",
		BGFX_DIR .. "/scripts/**.natvis"
	}

	angledIncludeDirs
	{
		BGFX_DIR .. "/include",
		BGFX_DIR .. "/3rdparty",
		BGFX_DIR .. "/3rdparty/khronos",
		BIMG_DIR .. "/include",
		BX_DIR .. "/include"
	}

	bxDefines()

	filter "system:windows"
		systemversion "latest"
		includedirs { BGFX_DIR .. "/3rdparty/directx-headers/include/directx" }

	filter "system:linux"
		includedirs
		{
			BGFX_DIR .. "/3rdparty/directx-headers/include/directx",
			BGFX_DIR .. "/3rdparty/directx-headers/include",
			BGFX_DIR .. "/3rdparty/directx-headers/include/wsl/stubs"
		}

	filter "system:macosx"
		systemversion "latest"
		-- No amalgamated.mm upstream; renderer_mtl.cpp needs the ObjC++ frontend.
		buildoptions { "-x objective-c++" }

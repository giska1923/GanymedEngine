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

-- bx, bimg and bgfx include their own public headers with the angled form
-- (<bx/allocator.h>, <bimg/bimg.h>). premake's xcode4 exporter puts includedirs into
-- USER_HEADER_SEARCH_PATHS, which clang searches for quoted includes only -- it emits
-- ALWAYS_SEARCH_USER_PATHS = NO, and the headermap that would otherwise resolve the
-- quoted form is the header style Xcode now warns is unsupported. So on that exporter
-- the same paths have to go out again as SYSTEM_HEADER_SEARCH_PATHS (-isystem), which
-- externalincludedirs produces and angled includes do search.
--
-- Scoped to the exporter rather than applied everywhere so vs2022/gmake2 output is
-- unchanged: those map includedirs to plain -I // AdditionalIncludeDirectories, which
-- already works for both include forms.
local function bxIncludeDirs(dirs)
	includedirs(dirs)

	filter "action:xcode4"
		externalincludedirs(dirs)
	filter {}
end

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

	filter "system:linux"
		pic "On"

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

	bxIncludeDirs
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

	files
	{
		BIMG_DIR .. "/include/**.h",
		BIMG_DIR .. "/src/image.cpp",
		BIMG_DIR .. "/src/bimg_p.h",
		BIMG_DIR .. "/src/config.h",
		BIMG_DIR .. "/3rdparty/astc-encoder/source/**.cpp",
		BIMG_DIR .. "/3rdparty/astc-encoder/source/**.h"
	}

	bxIncludeDirs
	{
		BIMG_DIR .. "/include",
		BIMG_DIR .. "/3rdparty/astc-encoder/include",
		BX_DIR .. "/include"
	}

	defines
	{
		"ASTCENC_F16C=0",
		"ASTCENC_NEON=0"
	}

	bxDefines()

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

	bxIncludeDirs
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

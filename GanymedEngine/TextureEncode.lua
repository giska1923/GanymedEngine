-- Build script for source/Platform/Bimg, the texture block-compressor.
--
-- Its own project for one reason: **bx does not compile below C++20** (`bx/platform.h`
-- static_asserts on it and #errors without /Zc:__cplusplus and /Zc:preprocessor), and
-- GanymedEngine is C++17 per AGENTS.md. bimg's public headers only forward-declare the bx
-- types, but its API takes them by pointer, so a caller needs the real definitions.
--
-- This is the same boundary bgfx.lua already establishes - its own comment records that
-- <bgfx/bgfx.h> was chosen precisely because it pulls in no bx headers. Compiling the whole
-- engine as C++20 for one file is one line and a project-wide language change touching every
-- vendored header; this is a build file and a narrow API (TextureEncode.h mentions neither
-- bimg nor bx). If the engine moves to C++20 for its own reasons, fold this back in.
--
-- It is also why the engine project below removes these files from its own source/** glob.
project "TextureEncode"
	kind "StaticLib"
	language "C++"
	cppdialect "C++20"
	staticruntime "off"

	targetdir ("%{wks.location}/bin/" .. outputdir .. "/%{prj.name}")
	objdir ("%{wks.location}/temp/" .. outputdir .. "/%{prj.name}")

	files
	{
		"source/Platform/Bimg/**.h",
		"source/Platform/Bimg/**.cpp"
	}

	includedirs
	{
		"source",
		"%{IncludeDir.bx}",
		"%{IncludeDir.bimg}"
	}

	filter "system:windows"
		systemversion "latest"
		buildoptions { "/Zc:__cplusplus", "/Zc:preprocessor" }

	filter "system:linux"
		pic "On"
		systemversion "latest"

	filter "system:macosx"
		systemversion "latest"

	-- bx reads BX_CONFIG_DEBUG rather than NDEBUG, and #errors when it is not defined at all.
	filter "configurations:Debug"
		runtime "Debug"
		symbols "on"
		defines { "BX_CONFIG_DEBUG=1" }

	filter "configurations:Release"
		runtime "Release"
		optimize "on"
		defines { "BX_CONFIG_DEBUG=0" }

	filter "configurations:Dist"
		runtime "Release"
		optimize "on"
		symbols "off"
		defines { "BX_CONFIG_DEBUG=0" }

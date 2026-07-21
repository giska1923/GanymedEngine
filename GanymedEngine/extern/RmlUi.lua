-- Build script for the RmlUi submodule. Lives outside the submodule because
-- git cannot track files inside a submodule's working tree.
--
-- Core + the Lua plugin. The Lua plugin runs on the SAME lua_State as gameplay
-- scripting (see GanymedE/Scripting/ScriptEngine.h), so UI logic and gameplay
-- logic share globals.
--
-- Deliberately NOT built: Lottie and SVG, which pull in rlottie and lunasvg.
project "RmlUi"
	kind "StaticLib"
	language "C++"
	cppdialect "C++17"
	staticruntime "off"
	warnings "Off"

	targetdir ("%{wks.location}/bin/" .. outputdir .. "/%{prj.name}")
	objdir ("%{wks.location}/temp/" .. outputdir .. "/%{prj.name}")

	files
	{
		"RmlUi/Source/Core/**.cpp",
		"RmlUi/Source/Core/**.h",
		"RmlUi/Source/Lua/**.cpp",
		"RmlUi/Source/Lua/**.h",
		"RmlUi/Include/RmlUi/**.h"
	}

	includedirs
	{
		"RmlUi/Include",
		"RmlUi/Source",
		"freetype/include",
		"lua",
		"lua_cxx"        -- supplies lua.hpp; see extern/lua_cxx/lua.hpp
	}

	defines
	{
		-- Must match the consumer's defines. Without it RmlUi's headers decorate
		-- every API with __declspec(dllimport) on MSVC and the static link fails.
		"RMLUI_STATIC_LIB",

		-- Registers the built-in FreeType font engine in Core.cpp. CMake sets this
		-- from its RMLUI_FONT_ENGINE option, which defaults to "freetype" - so a
		-- hand-written build script has to supply it or the sources compile but
		-- nothing ever registers them, and Rml::Initialise fails at runtime with
		-- "No font engine interface set!". Internal to Source/Core; consumers do
		-- not need it.
		"RMLUI_FONT_ENGINE_FREETYPE"
	}

	links { "FreeType", "Lua" }

	filter "system:windows"
		systemversion "latest"
		buildoptions { "/utf-8" }

	filter "system:linux"
		pic "On"
		systemversion "latest"

	filter "system:macosx"
		systemversion "latest"

	filter "configurations:Debug"
		runtime "Debug"
		symbols "on"
		-- Visual document inspector: element tree, computed RCSS, event log.
		-- Debug-only so Dist does not carry it.
		files { "RmlUi/Source/Debugger/**.cpp", "RmlUi/Source/Debugger/**.h" }

	filter "configurations:Release"
		runtime "Release"
		optimize "on"
		defines { "NDEBUG" }

	filter "configurations:Dist"
		runtime "Release"
		optimize "on"
		symbols "off"
		defines { "NDEBUG" }

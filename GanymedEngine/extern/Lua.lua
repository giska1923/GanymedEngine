-- Build script for the Lua submodule. Lives outside the submodule because
-- git cannot track files inside a submodule's working tree.
project "Lua"
	kind "StaticLib"
	language "C"
	staticruntime "off"
	warnings "Off"

	-- Workspace-level output dirs, like every other extern project. Writing
	-- bin/ inside the submodule tree makes the submodule permanently dirty in
	-- git status - the parent repo's .gitignore does not apply inside it.
	targetdir ("%{wks.location}/bin/" .. outputdir .. "/%{prj.name}")
	objdir ("%{wks.location}/temp/" .. outputdir .. "/%{prj.name}")

	files
	{
		"lua/*.c",
		"lua/*.h"
	}

	-- lua.c = standalone interpreter main(), onelua.c = amalgamated build (which
	-- also carries luac's main), ltests.c = internal test hooks. None belong in the
	-- embedded library; the main()s would collide at link time.
	removefiles
	{
		"lua/lua.c",
		"lua/onelua.c",
		"lua/ltests.c"
	}

	filter "system:windows"
		systemversion "latest"
		-- luaconf.h derives LUA_USE_WINDOWS from _WIN32 by itself.

	filter "system:linux"
		pic "On"
		systemversion "latest"
		-- Not auto-detected: enables dlopen-based package.loadlib and readline-free
		-- POSIX bits. Without it loadlib.c compiles to the "no dynamic loading" stub.
		defines { "LUA_USE_LINUX" }

	filter "system:macosx"
		systemversion "latest"
		defines { "LUA_USE_MACOSX" }

	filter "configurations:Debug"
		runtime "Debug"
		symbols "on"

	filter "configurations:Release"
		runtime "Release"
		optimize "on"

	filter "configurations:Dist"
		runtime "Release"
		optimize "on"
		symbols "off"

-- Build script for the IXWebSocket submodule (pinned to v12.0.1). Lives outside the submodule
-- because git cannot track files inside a submodule's working tree. Its own project for the same
-- reason as enkiTS.lua: the engine project forces gepch.h on every TU.
--
-- Built without TLS and without zlib (docs/ToDo/ONLINE.md, O0). Upstream's CMake makes both
-- options, and every use of either is behind #ifdef IXWEBSOCKET_USE_TLS / _ZLIB inside the
-- sources, so leaving those undefined is the whole of "off". The one thing CMake does with the
-- file list is add a TLS backend when TLS is on, so the list here is every source minus the three
-- backends: exactly CMake's list with USE_TLS and USE_ZLIB off (diffed against v12.0.1's
-- CMakeLists.txt). Dropping the backends also keeps OpenSSL/mbedTLS headers from ever being
-- required on a clean clone.
project "IXWebSocket"
	kind "StaticLib"
	language "C++"
	cppdialect "C++17"
	staticruntime "off"
	warnings "Off"

	-- Workspace-level output dirs, like every other extern project, so the submodule stays clean.
	targetdir ("%{wks.location}/bin/" .. outputdir .. "/%{prj.name}")
	objdir ("%{wks.location}/temp/" .. outputdir .. "/%{prj.name}")

	files
	{
		"IXWebSocket/ixwebsocket/*.h",
		"IXWebSocket/ixwebsocket/*.cpp"
	}

	removefiles
	{
		"IXWebSocket/ixwebsocket/IXSocketOpenSSL.*",
		"IXWebSocket/ixwebsocket/IXSocketMbedTLS.*",
		"IXWebSocket/ixwebsocket/IXSocketAppleSSL.*"
	}

	-- Its sources include their own headers as <ixwebsocket/...>.
	includedirs
	{
		"IXWebSocket"
	}

	filter "system:windows"
		systemversion "latest"
		-- The only define CMake sets with TLS and zlib off (PRIVATE there, so not needed by
		-- consumers).
		defines { "_CRT_SECURE_NO_WARNINGS" }
		-- Only Winsock 2. CMake's WIN32 list also has wsock32 and shlwapi, but shlwapi is used only
		-- by the OpenSSL backend (excluded above) and nothing includes the Winsock 1 header. They
		-- are not harmless extras either: a StaticLib's links are merged into the .lib by lib.exe,
		-- and merging wsock32 over ws2_32 produced ~60 LNK4006 duplicate-symbol warnings. The
		-- merge is how the engine's own gdi32/psapi/uuid reach the executables on MSVC.
		links { "ws2_32" }

	filter "system:linux"
		pic "On"
		systemversion "latest"
		links { "pthread" }

	filter "system:macosx"
		systemversion "latest"

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

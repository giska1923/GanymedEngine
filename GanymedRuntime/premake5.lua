project "GanymedRuntime"
	kind "ConsoleApp"
	language "C++"
	cppdialect "C++17"
	staticruntime "off"

	targetdir ("%{wks.location}/bin/" .. outputdir .. "/%{prj.name}")
	objdir ("%{wks.location}/temp/" .. outputdir .. "/%{prj.name}")

	files
	{
		"source/**.h",
		"source/**.cpp"
	}

	includedirs
	{
		"%{wks.location}/GanymedEngine/extern/spdlog/include",
		"%{wks.location}/GanymedEngine/source",
		"%{wks.location}/GanymedEngine/extern",
		"%{IncludeDir.glm}",
		"%{IncludeDir.entt}",
		-- Buffer.h now exposes bgfx types, so anything including GanymedE.h needs this
		"%{IncludeDir.bgfx}",
		-- RuntimeConfig parses assets/runtime.yaml directly. The engine already links
		-- yaml-cpp, so this is an include path only - no new link entry on MSVC.
		"%{IncludeDir.yaml_cpp}"
	}

	links
	{
		"GanymedEngine"
	}

	-- Assets are loaded via relative paths, so the debugger must launch from the project folder
	debugdir "%{prj.location}"

	-- Xcode puts includedirs in USER_HEADER_SEARCH_PATHS which angled includes (spdlog) don't see
	filter "action:xcode4"
		xcodebuildsettings { ["ALWAYS_SEARCH_USER_PATHS"] = "YES" }
	filter {}

	filter "system:windows"
		systemversion "latest"
		buildoptions { "/utf-8" }

	-- /SUBSYSTEM:WINDOWS so a shipped game has no console window sitting behind it;
	-- mainCRTStartup keeps the entry point at main() rather than WinMain.
	--
	-- Scoped to Dist, unlike GanymedEditor which is windowed in every configuration.
	-- Note what the console does and does not buy on Windows: Log's non-file sink here
	-- is spdlog's msvc_sink (OutputDebugString), so the window itself stays empty and
	-- the boot log goes to GanymedE.log and the debugger's Output pane. Keeping
	-- ConsoleApp in Debug/Release is about matching the other projects and leaving a
	-- place for ad-hoc stdio, not about reading the log.
	filter { "system:windows", "configurations:Dist" }
		kind "WindowedApp"
		entrypoint "mainCRTStartup"

	-- Static libraries do not propagate their links outside Visual Studio,
	-- so the executable links the dependency projects and system libraries itself.
	--
	-- Order matters here in a way it does not on MSVC: GNU ld walks archives once,
	-- left to right, pulling only the objects that resolve symbols undefined *so far*.
	-- A library must therefore appear before the ones it depends on - RmlUi before
	-- Lua and FreeType, bgfx before bimg and bx - or the link fails on symbols that
	-- are plainly present in the archive list. Copied verbatim from
	-- GanymedEditor/premake5.lua; keep the three lists in step.
	filter "system:linux"
		systemversion "latest"

		links
		{
			"GLFW",
			"ImGui",
			"yaml-cpp",
			"Jolt",
			"bgfx",
			"bimg",
			"bx",
			"RmlUi",
			"FreeType",
			"Lua",
			"GL",
			"X11",
			"dl",
			"pthread"
		}

	filter "system:macosx"
		systemversion "latest"

		links
		{
			"GLFW",
			"ImGui",
			"yaml-cpp",
			"Jolt",
			"bgfx",
			"bimg",
			"bx",
			"Lua",
			"RmlUi",
			"FreeType",
			"Cocoa.framework",
			"OpenGL.framework",
			"IOKit.framework",
			"CoreFoundation.framework",
			"CoreVideo.framework",
			"QuartzCore.framework",
			"Metal.framework",
			"MetalKit.framework",
			-- bgfx's Metal backend carries a hardware video decoder that is always
			-- compiled in, so its two frameworks are required even though nothing
			-- here decodes video. See GanymedEditor/premake5.lua.
			"CoreMedia.framework",
			"VideoToolbox.framework"
		}

	filter "configurations:Debug"
		defines "GE_DEBUG"
		runtime "Debug"
		symbols "on"

	filter "configurations:Release"
		defines "GE_RELEASE"
		runtime "Release"
		optimize "on"

	filter "configurations:Dist"
		defines "GE_DIST"
		runtime "Release"
		optimize "on"

project "GanymedEditor"
	-- WindowedApp only on Windows (see the system:windows filter). On xcode4 it would
	-- emit a .app bundle, and Xcode refuses to code sign a bundle without an Info.plist
	-- that premake does not generate - plus a bundle rewrites the working directory,
	-- which the relative asset paths depend on.
	kind "ConsoleApp"
	language "C++"
	cppdialect "C++17"
	staticruntime "off"

	targetdir ("%{wks.location}/bin/" .. outputdir .. "/%{prj.name}")
	objdir ("%{wks.location}/temp/" .. outputdir .. "/%{prj.name}")

	files
	{
		"source/**.h",
		"source/**.cpp",
		"%{wks.location}/GanymedEngine/extern/ImGuizmo/src/ImGuizmo.h",
		"%{wks.location}/GanymedEngine/extern/ImGuizmo/src/ImGuizmo.cpp"
	}

	includedirs
	{
		"%{wks.location}/GanymedEngine/extern/spdlog/include",
		"%{wks.location}/GanymedEngine/source",
		"%{wks.location}/GanymedEngine/extern",
		"%{IncludeDir.glm}",
		"%{IncludeDir.entt}",
		"%{IncludeDir.ImGui}",
		"%{IncludeDir.ImGuizmo}",
		-- Buffer.h now exposes bgfx types, so anything including GanymedE.h needs this
		"%{IncludeDir.bgfx}"
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

		-- /SUBSYSTEM:WINDOWS so no console window sits behind the editor;
		-- mainCRTStartup keeps the entry point at main() rather than WinMain
		kind "WindowedApp"
		entrypoint "mainCRTStartup"

		-- Embeds the executable icon (resources/icon.ico)
		files { "resources/GanymedEditor.rc" }

	-- Static libraries do not propagate their links outside Visual Studio,
	-- so the executable links the dependency projects and system libraries itself
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
			"Lua",
			"RmlUi",
			"FreeType",
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
			"MetalKit.framework"
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
project "GanymedEditor"
	kind "WindowedApp"
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
		"%{IncludeDir.ImGuizmo}"
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
		entrypoint "mainCRTStartup"

	-- Static libraries do not propagate their links outside Visual Studio,
	-- so the executable links the dependency projects and system libraries itself
	filter "system:linux"
		systemversion "latest"

		links
		{
			"GLFW",
			"Glad",
			"ImGui",
			"yaml-cpp",
			"Jolt",
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
			"Glad",
			"ImGui",
			"yaml-cpp",
			"Jolt",
			"Cocoa.framework",
			"OpenGL.framework",
			"IOKit.framework",
			"CoreFoundation.framework",
			"CoreVideo.framework",
			"QuartzCore.framework"
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
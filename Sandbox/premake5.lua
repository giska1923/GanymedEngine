project "Sandbox"
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
		"%{IncludeDir.entt}"
	}

	links
	{
		"GanymedEngine"
	}

	-- Assets are loaded via relative paths, so the debugger must launch from the project folder
	debugdir "%{prj.location}"

	filter "system:windows"
		systemversion "latest"
		buildoptions { "/utf-8" }

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
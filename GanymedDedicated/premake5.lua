project "GanymedDedicated"
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
		"%{IncludeDir.bgfx}",
		"%{IncludeDir.yaml_cpp}"
	}

	links
	{
		"GanymedEngine"
	}

	debugdir "%{wks.location}"

	filter "action:xcode4"
		xcodebuildsettings { ["ALWAYS_SEARCH_USER_PATHS"] = "YES" }
	filter {}

	filter "system:windows"
		systemversion "latest"
		buildoptions { "/utf-8" }
		links { "ws2_32.lib" }

	-- Same link order as GanymedRuntime. A static lib does not propagate its dependencies
	-- to GNU ld, so the executable lists them itself. Keep the three app lists in step.
	filter "system:linux"
		systemversion "latest"

		links
		{
			"GLFW",
			"ImGui",
			"yaml-cpp",
			"Jolt",
			"TextureEncode",
			"enkiTS",
			"IXWebSocket",
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
			"TextureEncode",
			"enkiTS",
			"IXWebSocket",
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
			"CoreMedia.framework",
			"VideoToolbox.framework",
			"CoreAudio.framework",
			"AudioToolbox.framework"
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

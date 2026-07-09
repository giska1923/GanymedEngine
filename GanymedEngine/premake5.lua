project "GanymedEngine"
	kind "StaticLib"
	language "C++"
	cppdialect "C++17"
	staticruntime "off"

	targetdir ("%{wks.location}/bin/" .. outputdir .. "/%{prj.name}")
	objdir ("%{wks.location}/temp/" .. outputdir .. "/%{prj.name}")

	pchheader "gepch.h"
	pchsource "source/gepch.cpp"

	-- Xcode resolves the prefix header relative to the project directory, not the include dirs
	filter "action:xcode4"
		pchheader "source/gepch.h"
	filter {}

	files
	{
		"source/**.h",
		"source/**.cpp",
		"extern/stb_image/**.cpp",
		"extern/stb_image/**.h",
		"extern/glm/glm/**.hpp",
		"extern/glm/glm/**.inl"
	}

	defines
	{
		"_CRT_SECURE_NO_WARNINGS",
		"GLFW_INCLUDE_NONE"
	}

	includedirs
	{
		"source",
		"extern/spdlog/include",
		"%{IncludeDir.Glad}",
		"%{IncludeDir.GLFW}",
		"%{IncludeDir.ImGui}",
		"%{IncludeDir.glm}",
		"%{IncludeDir.stb_image}",
		"%{IncludeDir.entt}",
		"%{IncludeDir.yaml_cpp}"
	}

	links
	{
		"GLFW",
		"Glad",
		"ImGui",
		"yaml-cpp"
	}

	filter "system:windows"
		systemversion "latest"
		buildoptions { "/utf-8" }

		links
		{
			"opengl32.lib"
		}

	filter "system:linux"
		pic "On"
		systemversion "latest"

	filter "system:macosx"
		systemversion "latest"

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
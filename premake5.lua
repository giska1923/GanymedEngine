workspace "GanymedEngine"
	architecture "x64"
	startproject "Sandbox"

	configurations
	{
		"Debug",
		"Release",
		"Dist"
	}

outputdir = "%{cfg.buildcfg}-%{cfg.system}-%{cfg.architecture}"

-- Include directories relative to root folder (solution directory)
IncludeDir = {}
IncludeDir["GLFW"] = "GanymedEngine/extern/GLFW/include"
IncludeDir["Glad"] = "GanymedEngine/extern/Glad/include"
IncludeDir["ImGui"] = "GanymedEngine/extern/imgui"
IncludeDir["glm"] = "GanymedEngine/extern/glm"
IncludeDir["stb_image"] = "GanymedEngine/extern/stb_image"

group "Dependencies"
	include "GanymedEngine/extern/GLFW"
	include "GanymedEngine/extern/Glad"
	include "GanymedEngine/extern/imgui"

group ""

project "GanymedEngine"
	location "GanymedEngine"
	kind "StaticLib"
	language "C++"
	cppdialect "C++17"
	staticruntime "on"

	targetdir ("bin/" .. outputdir .. "/%{prj.name}")
	objdir ("temp/" .. outputdir .. "/%{prj.name}")

	pchheader "gepch.h"
	pchsource "GanymedEngine/source/gepch.cpp"

	files
	{
		"%{prj.name}/source/**.h",
		"%{prj.name}/source/**.cpp",
		"%{prj.name}/extern/stb_image/**.cpp",
		"%{prj.name}/extern/stb_image/**.h",
		"%{prj.name}/extern/glm/glm/**.hpp",
		"%{prj.name}/extern/glm/glm/**.inl"
	}

	defines
	{
		"_CRT_SECURE_NO_WARNINGS"
	}

	includedirs
	{
		"%{prj.name}/source",
		"%{prj.name}/extern/spdlog/include",
		"%{IncludeDir.Glad}",
		"%{IncludeDir.GLFW}",
		"%{IncludeDir.ImGui}",
		"%{IncludeDir.glm}",
		"%{IncludeDir.stb_image}"
	}

	links
	{
		"GLFW",
		"Glad",
		"ImGui",
		"opengl32.lib"
	}
	
	filter "system:windows"
		systemversion "latest"

		defines
		{
			"GE_PLATFORM_WINDOWS",
			"GLFW_INCLUDE_NONE",
			"GE_BUILD_DLL"
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

project "Sandbox"
	location "Sandbox"
	kind "ConsoleApp"
	language "C++"
	cppdialect "C++17"
	staticruntime "on"

	targetdir ("bin/" .. outputdir .. "/%{prj.name}")
	objdir ("temp/" .. outputdir .. "/%{prj.name}")

	files
	{
		"%{prj.name}/source/**.h",
		"%{prj.name}/source/**.cpp"
	}

	includedirs
	{
		"GanymedEngine/extern/spdlog/include",
		"GanymedEngine/source",
		"GanymedEngine/extern/imgui",
		"%{IncludeDir.glm}"
	}

	links
	{
		"GanymedEngine"
	}

	filter "system:windows"
		systemversion "latest"

		defines
		{
			"GE_PLATFORM_WINDOWS"
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

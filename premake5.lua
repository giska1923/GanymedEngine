include "./vendor/premake/premake_customization/solution_items.lua"

workspace "GanymedEngine"
	architecture "x86_64"
	startproject "GanymedEditor"

	configurations
	{
		"Debug",
		"Release",
		"Dist"
	}

	solution_items
	{
		".editorconfig"
	}

	flags
	{
		"MultiProcessorCompile"
	}

outputdir = "%{cfg.buildcfg}-%{cfg.system}-%{cfg.architecture}"

-- Include directories relative to root folder (solution directory)
IncludeDir = {}
IncludeDir["GLFW"] = "%{wks.location}/GanymedEngine/extern/GLFW/include"
IncludeDir["Glad"] = "%{wks.location}/GanymedEngine/extern/Glad/include"
IncludeDir["ImGui"] = "%{wks.location}/GanymedEngine/extern/imgui"
IncludeDir["glm"] = "%{wks.location}/GanymedEngine/extern/glm"
IncludeDir["stb_image"] = "%{wks.location}/GanymedEngine/extern/stb_image"
IncludeDir["entt"] = "%{wks.location}/GanymedEngine/extern/entt/include"

group "Dependencies"
	include "vendor/premake"
	include "GanymedEngine/extern/GLFW"
	include "GanymedEngine/extern/Glad"
	include "GanymedEngine/extern/imgui"
group ""

include "GanymedEngine"
include "Sandbox"
include "GanymedEditor"
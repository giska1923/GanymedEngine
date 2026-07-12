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

	-- Enable multi-processor compilation (compatible with older Premake5 versions)
	filter "system:windows"
		flags { "MultiProcessorCompile" }
	filter {}

outputdir = "%{cfg.buildcfg}-%{cfg.system}-%{cfg.architecture}"

-- Include directories relative to root folder (solution directory)
IncludeDir = {}
IncludeDir["GLFW"] = "%{wks.location}/GanymedEngine/extern/GLFW/include"
IncludeDir["Glad"] = "%{wks.location}/GanymedEngine/extern/Glad/include"
IncludeDir["ImGui"] = "%{wks.location}/GanymedEngine/extern/imgui"
IncludeDir["glm"] = "%{wks.location}/GanymedEngine/extern/glm"
IncludeDir["stb_image"] = "%{wks.location}/GanymedEngine/extern/stb_image"
IncludeDir["entt"] = "%{wks.location}/GanymedEngine/extern/entt/single_include"
IncludeDir["yaml_cpp"] = "%{wks.location}/GanymedEngine/extern/yaml-cpp/include"
IncludeDir["ImGuizmo"] = "%{wks.location}/GanymedEngine/extern/ImGuizmo/src"
IncludeDir["cgltf"] = "%{wks.location}/GanymedEngine/extern/cgltf"

group "Dependencies"
	include "vendor/premake"
	include "GanymedEngine/extern/GLFW.lua"
	include "GanymedEngine/extern/Glad"
	include "GanymedEngine/extern/imgui.lua"
	include "GanymedEngine/extern/yaml-cpp"
group ""

include "GanymedEngine"
include "Sandbox"
include "GanymedEditor"
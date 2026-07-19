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

	-- bgfx normalises clip space to [0,1] on D3D/Vulkan/Metal, but glm defaults to
	-- OpenGL's [-1,1]. Left unset, everything nearer than ~2*near*far/(far+near)
	-- maps to a negative ndc z and is clipped away - verified by capture: a probe
	-- at 0.15 units vanished until this was defined.
	--
	-- Workspace scope on purpose: glm is header-only, so a project disagreeing
	-- here would silently change the layout of shared glm types across the static
	-- library boundary. BgfxContext asserts the live backend agrees.
	defines
	{
		"GLM_FORCE_DEPTH_ZERO_TO_ONE"
	}

	-- Enable multi-processor compilation (compatible with older Premake5 versions)
	filter "system:windows"
		flags { "MultiProcessorCompile" }
	filter {}

outputdir = "%{cfg.buildcfg}-%{cfg.system}-%{cfg.architecture}"

-- Include directories relative to root folder (solution directory)
IncludeDir = {}
IncludeDir["GLFW"] = "%{wks.location}/GanymedEngine/extern/GLFW/include"
IncludeDir["ImGui"] = "%{wks.location}/GanymedEngine/extern/imgui"
IncludeDir["glm"] = "%{wks.location}/GanymedEngine/extern/glm"
IncludeDir["stb_image"] = "%{wks.location}/GanymedEngine/extern/stb_image"
IncludeDir["entt"] = "%{wks.location}/GanymedEngine/extern/entt/single_include"
IncludeDir["yaml_cpp"] = "%{wks.location}/GanymedEngine/extern/yaml-cpp/include"
IncludeDir["ImGuizmo"] = "%{wks.location}/GanymedEngine/extern/ImGuizmo/src"
IncludeDir["cgltf"] = "%{wks.location}/GanymedEngine/extern/cgltf"
IncludeDir["Jolt"] = "%{wks.location}/GanymedEngine/extern/JoltPhysics"
IncludeDir["bx"] = "%{wks.location}/GanymedEngine/extern/bx/include"
IncludeDir["bimg"] = "%{wks.location}/GanymedEngine/extern/bimg/include"
IncludeDir["bgfx"] = "%{wks.location}/GanymedEngine/extern/bgfx/include"
IncludeDir["lua"] = "%{wks.location}/GanymedEngine/extern/lua"
-- Supplies lua.hpp, which the lua/lua source mirror does not ship. Must come
-- alongside IncludeDir.lua, never instead of it.
IncludeDir["lua_cxx"] = "%{wks.location}/GanymedEngine/extern/lua_cxx"
IncludeDir["sol2"] = "%{wks.location}/GanymedEngine/extern/sol2/include"

group "Dependencies"
	include "vendor/premake"
	include "GanymedEngine/extern/GLFW.lua"
	include "GanymedEngine/extern/imgui.lua"
	include "GanymedEngine/extern/yaml-cpp"
	include "GanymedEngine/extern/Jolt.lua"
	include "GanymedEngine/extern/bgfx.lua"
	include "GanymedEngine/extern/Lua.lua"
group ""

include "GanymedEngine"
include "Sandbox"
include "GanymedEditor"
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

-- Include paths for third-party libraries that include their own public headers with
-- the angled form - <Jolt/Jolt.h>, <RmlUi/Core/Core.h>, <freetype/freetype.h>,
-- <bx/allocator.h>. premake's xcode4 exporter maps includedirs to
-- USER_HEADER_SEARCH_PATHS and emits ALWAYS_SEARCH_USER_PATHS = NO, and clang searches
-- user paths for quoted includes only, so on that exporter the angled form resolves
-- against nothing. (It appears to half-work in Xcode's own diagnostics because the
-- headermap indexes the target's file list and answers the quoted form - that is the
-- header style Xcode 26 now warns is unsupported.)
--
-- externalincludedirs emits SYSTEM_HEADER_SEARCH_PATHS, i.e. -isystem, which the angled
-- form does search. Scoped to the exporter on purpose: vs2022 and gmake2 map includedirs
-- to plain AdditionalIncludeDirectories / -I, which already serves both include forms,
-- and routing them through /external:I would change MSVC warning behaviour for no gain.
function angledIncludeDirs(dirs)
	includedirs(dirs)

	filter "action:xcode4"
		externalincludedirs(dirs)
	filter {}
end

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
IncludeDir["RmlUi"] = "%{wks.location}/GanymedEngine/extern/RmlUi/Include"
IncludeDir["freetype"] = "%{wks.location}/GanymedEngine/extern/freetype/include"

group "Dependencies"
	include "vendor/premake"
	include "GanymedEngine/extern/GLFW.lua"
	include "GanymedEngine/extern/imgui.lua"
	include "GanymedEngine/extern/yaml-cpp"
	include "GanymedEngine/extern/Jolt.lua"
	include "GanymedEngine/extern/bgfx.lua"
	include "GanymedEngine/extern/Lua.lua"
	include "GanymedEngine/extern/FreeType.lua"
	include "GanymedEngine/extern/RmlUi.lua"
group ""

include "GanymedEngine"
include "Sandbox"
include "GanymedEditor"
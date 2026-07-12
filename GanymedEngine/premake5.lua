project "GanymedEngine"
	kind "StaticLib"
	language "C++"
	cppdialect "C++17"
	staticruntime "off"

	targetdir ("%{wks.location}/bin/" .. outputdir .. "/%{prj.name}")
	objdir ("%{wks.location}/temp/" .. outputdir .. "/%{prj.name}")

	pchheader "gepch.h"
	pchsource "source/gepch.cpp"

	-- Xcode resolves the prefix header relative to the project directory, not the include dirs,
	-- and puts includedirs in USER_HEADER_SEARCH_PATHS which angled includes (spdlog) don't see
	filter "action:xcode4"
		pchheader "source/gepch.h"
		xcodebuildsettings { ["ALWAYS_SEARCH_USER_PATHS"] = "YES" }
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
		"GLFW_INCLUDE_NONE",
		-- Must match Jolt.lua so headers/types agree across the static lib boundary
		"JPH_OBJECT_STREAM",
		"JPH_USE_AVX2",
		"JPH_USE_AVX",
		"JPH_USE_SSE4_1",
		"JPH_USE_SSE4_2",
		"JPH_USE_LZCNT",
		"JPH_USE_TZCNT",
		"JPH_USE_F16C",
		"JPH_USE_FMADD"
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
		"%{IncludeDir.yaml_cpp}",
		"%{IncludeDir.cgltf}",
		"%{IncludeDir.Jolt}"
	}

	links
	{
		"GLFW",
		"Glad",
		"ImGui",
		"yaml-cpp",
		"Jolt"
	}

	filter "system:windows"
		systemversion "latest"
		buildoptions { "/utf-8", "/arch:AVX2" }

		links
		{
			"opengl32.lib"
		}

	filter "system:linux"
		pic "On"
		systemversion "latest"
		buildoptions { "-mavx2", "-mbmi", "-mpopcnt", "-mlzcnt", "-mf16c", "-mfma" }

	filter "system:macosx"
		systemversion "latest"

	filter "configurations:Debug"
		defines { "GE_DEBUG", "JPH_PROFILE_ENABLED", "JPH_DEBUG_RENDERER" }
		runtime "Debug"
		symbols "on"

	filter "configurations:Release"
		defines { "GE_RELEASE", "NDEBUG", "JPH_PROFILE_ENABLED", "JPH_DEBUG_RENDERER" }
		runtime "Release"
		optimize "on"

	filter "configurations:Dist"
		defines { "GE_DIST", "NDEBUG" }
		runtime "Release"
		optimize "on"

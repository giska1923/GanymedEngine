-- Build script for the JoltPhysics submodule. Lives outside the submodule because
-- git cannot track files inside a submodule's working tree.
-- Matches Jolt's default x86_64 instruction set (AVX2) without GPU compute backends.
project "Jolt"
	kind "StaticLib"
	language "C++"
	cppdialect "C++17"
	staticruntime "off"
	warnings "Off"

	targetdir ("%{wks.location}/bin/" .. outputdir .. "/%{prj.name}")
	objdir ("%{wks.location}/temp/" .. outputdir .. "/%{prj.name}")

	files
	{
		"JoltPhysics/Jolt/**.h",
		"JoltPhysics/Jolt/**.cpp",
		"JoltPhysics/Jolt/**.inl",
		"JoltPhysics/Jolt/Jolt.natvis"
	}

	-- Exclude GPU compute backends, CPU hair wrappers, and HLSL assets.
	-- Keep Compute/ComputeSystem.cpp (abstract base, always required).
	removefiles
	{
		"JoltPhysics/Jolt/Shaders/**",
		"JoltPhysics/Jolt/Compute/CPU/**",
		"JoltPhysics/Jolt/Compute/DX12/**",
		"JoltPhysics/Jolt/Compute/VK/**",
		"JoltPhysics/Jolt/Compute/MTL/**"
	}

	includedirs
	{
		"JoltPhysics"
	}

	defines
	{
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

	filter "system:windows"
		systemversion "latest"
		buildoptions { "/arch:AVX2", "/fp:except-", "/Zc:__cplusplus" }

	filter "system:linux"
		pic "On"
		systemversion "latest"
		buildoptions { "-mavx2", "-mbmi", "-mpopcnt", "-mlzcnt", "-mf16c", "-mfma", "-mfpmath=sse" }
		links { "pthread" }

	filter "system:macosx"
		systemversion "latest"
		-- Apple Silicon uses NEON; Intel Macs still get AVX2 via the defines above when applicable.

	filter "configurations:Debug"
		runtime "Debug"
		symbols "on"
		defines { "JPH_PROFILE_ENABLED", "JPH_DEBUG_RENDERER" }

	filter "configurations:Release"
		runtime "Release"
		optimize "on"
		defines { "NDEBUG", "JPH_PROFILE_ENABLED", "JPH_DEBUG_RENDERER" }

	filter "configurations:Dist"
		runtime "Release"
		optimize "on"
		defines { "NDEBUG" }
		symbols "off"

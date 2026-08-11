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

	-- Jolt reaches for its own headers angled from the repo root (<Jolt/Core/Core.h>);
	-- see angledIncludeDirs in the workspace premake5.lua for why xcode4 needs more
	-- than includedirs for that.
	angledIncludeDirs
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
		-- The JPH_USE_* defines above only tell Jolt's headers to reach for the intrinsics;
		-- clang still refuses to inline _mm_fmadd_ps and friends unless the target features
		-- are actually enabled, so the flags have to be repeated per platform.
		--
		-- This assumes x86_64, which the workspace pins. A native arm64 build would need
		-- these dropped and the JPH_USE_* set swapped for the NEON path.
		buildoptions { "-mavx2", "-mbmi", "-mpopcnt", "-mlzcnt", "-mf16c", "-mfma" }

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

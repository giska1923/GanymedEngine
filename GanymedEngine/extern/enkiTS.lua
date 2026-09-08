-- Build script for the enkiTS submodule. Lives outside the submodule because
-- git cannot track files inside a submodule's working tree.
--
-- Its own project rather than compiling the sources into GanymedEngine: the engine
-- project forces gepch.h on every TU (<PrecompiledHeader>Use</PrecompiledHeader>), and
-- the only way to satisfy that is to edit the source to include it - which is what
-- extern/stb_image/stb_image.cpp does, and which is not available for a submodule.
project "enkiTS"
	kind "StaticLib"
	language "C++"
	cppdialect "C++17"
	staticruntime "off"
	warnings "Off"

	-- Workspace-level output dirs, like every other extern project. Writing
	-- bin/ inside the submodule tree makes the submodule permanently dirty in
	-- git status - the parent repo's .gitignore does not apply inside it.
	targetdir ("%{wks.location}/bin/" .. outputdir .. "/%{prj.name}")
	objdir ("%{wks.location}/temp/" .. outputdir .. "/%{prj.name}")

	-- Listed rather than globbed: src/ also holds TaskScheduler_c.h/.cpp, the C API
	-- wrapper. Nothing in Ganymed uses it, and it drags in its own allocation shims.
	files
	{
		"enkiTS/src/LockLessMultiReadPipe.h",
		"enkiTS/src/TaskScheduler.h",
		"enkiTS/src/TaskScheduler.cpp"
	}

	-- No defines needed. ENKITS_API expands to nothing unless ENKITS_DLL is set, and
	-- ENKITS_TASK_PRIORITIES_NUM defaults to 3, which Core/JobSystem.cpp static_asserts
	-- rather than sets - changing it here would silently change JobPriority's meaning.

	filter "system:windows"
		systemversion "latest"

	filter "system:linux"
		pic "On"
		systemversion "latest"
		links { "pthread" }

	filter "system:macosx"
		systemversion "latest"

	filter "configurations:Debug"
		runtime "Debug"
		symbols "on"

	filter "configurations:Release"
		runtime "Release"
		optimize "on"

	filter "configurations:Dist"
		runtime "Release"
		optimize "on"
		symbols "off"

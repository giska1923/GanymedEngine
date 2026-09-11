-- Build script for the imgui submodule. Lives outside the submodule because
-- git cannot track files inside a submodule's working tree.
project "ImGui"
	kind "StaticLib"
	language "C++"
	cppdialect "C++17"
	staticruntime "off"

	targetdir ("%{wks.location}/bin/" .. outputdir .. "/%{prj.name}")
	objdir ("%{wks.location}/temp/" .. outputdir .. "/%{prj.name}")

	files
	{
		"imgui/imconfig.h",
		"imgui/imgui.h",
		"imgui/imgui.cpp",
		"imgui/imgui_draw.cpp",
		"imgui/imgui_internal.h",
		"imgui/imgui_tables.cpp",
		"imgui/imgui_widgets.cpp",
		"imgui/imgui_demo.cpp",
		"imgui/imstb_rectpack.h",
		"imgui/imstb_textedit.h",
		"imgui/imstb_truetype.h",
		"imgui/misc/freetype/imgui_freetype.cpp",
		"imgui/misc/freetype/imgui_freetype.h"
	}

	-- IMGUI_ENABLE_FREETYPE is also a workspace define so every TU that includes
	-- imgui_internal.h agrees on the builder. It is repeated here because this is
	-- the project that actually compiles imgui_draw.cpp, which hooks the builder.
	defines { "IMGUI_ENABLE_FREETYPE" }

	includedirs
	{
		"imgui"
	}

	-- imgui_freetype.cpp includes <ft2build.h>. FreeType's public headers are
	-- angled, so this path has to go through angledIncludeDirs for the xcode4
	-- exporter (see the helper in the workspace premake5.lua).
	angledIncludeDirs { "%{IncludeDir.freetype}" }

	filter "system:windows"
		systemversion "latest"

	filter "system:linux"
		pic "On"
		systemversion "latest"

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

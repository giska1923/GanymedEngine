-- Build script for the GLFW submodule. Lives outside the submodule because
-- git cannot track files inside a submodule's working tree.
project "GLFW"
	kind "StaticLib"
	language "C"
	staticruntime "off"

	targetdir ("%{wks.location}/bin/" .. outputdir .. "/%{prj.name}")
	objdir ("%{wks.location}/temp/" .. outputdir .. "/%{prj.name}")

	files
	{
		"GLFW/include/GLFW/glfw3.h",
		"GLFW/include/GLFW/glfw3native.h",
		"GLFW/src/glfw_config.h",
		"GLFW/src/context.c",
		"GLFW/src/init.c",
		"GLFW/src/input.c",
		"GLFW/src/monitor.c",
		"GLFW/src/platform.c",
		"GLFW/src/vulkan.c",
		"GLFW/src/window.c",
		"GLFW/src/egl_context.c",
		"GLFW/src/osmesa_context.c",
		"GLFW/src/null_platform.h",
		"GLFW/src/null_joystick.h",
		"GLFW/src/null_init.c",
		"GLFW/src/null_monitor.c",
		"GLFW/src/null_window.c",
		"GLFW/src/null_joystick.c"
	}

	filter "system:linux"
		pic "On"

		systemversion "latest"

		files
		{
			"GLFW/src/x11_platform.h",
			"GLFW/src/xkb_unicode.h",
			"GLFW/src/posix_time.h",
			"GLFW/src/posix_thread.h",
			"GLFW/src/posix_poll.h",
			"GLFW/src/linux_joystick.h",

			"GLFW/src/x11_init.c",
			"GLFW/src/x11_monitor.c",
			"GLFW/src/x11_window.c",
			"GLFW/src/xkb_unicode.c",
			"GLFW/src/posix_time.c",
			"GLFW/src/posix_thread.c",
			"GLFW/src/posix_module.c",
			"GLFW/src/posix_poll.c",
			"GLFW/src/glx_context.c",
			"GLFW/src/egl_context.c",
			"GLFW/src/osmesa_context.c",
			"GLFW/src/linux_joystick.c"
		}

		defines
		{
			"_GLFW_X11"
		}

	filter "system:macosx"
		systemversion "latest"

		files
		{
			"GLFW/src/cocoa_platform.h",
			"GLFW/src/cocoa_joystick.h",
			"GLFW/src/cocoa_time.h",
			"GLFW/src/posix_thread.h",
			"GLFW/src/nsgl_context.h",
			"GLFW/src/egl_context.h",
			"GLFW/src/osmesa_context.h",

			"GLFW/src/cocoa_init.m",
			"GLFW/src/cocoa_joystick.m",
			"GLFW/src/cocoa_monitor.m",
			"GLFW/src/cocoa_window.m",
			"GLFW/src/cocoa_time.c",
			"GLFW/src/posix_thread.c",
			"GLFW/src/posix_module.c",
			"GLFW/src/nsgl_context.m",
			"GLFW/src/egl_context.c",
			"GLFW/src/osmesa_context.c"
		}

		defines
		{
			"_GLFW_COCOA"
		}

	filter "system:windows"
		systemversion "latest"

		files
		{
			"GLFW/src/win32_platform.h",
			"GLFW/src/win32_joystick.h",
			"GLFW/src/win32_time.h",
			"GLFW/src/win32_thread.h",
			"GLFW/src/wgl_context.h",
			"GLFW/src/egl_context.h",
			"GLFW/src/osmesa_context.h",

			"GLFW/src/win32_init.c",
			"GLFW/src/win32_joystick.c",
			"GLFW/src/win32_module.c",
			"GLFW/src/win32_monitor.c",
			"GLFW/src/win32_time.c",
			"GLFW/src/win32_thread.c",
			"GLFW/src/win32_window.c",
			"GLFW/src/wgl_context.c",
			"GLFW/src/egl_context.c",
			"GLFW/src/osmesa_context.c"
		}

		defines
		{
			"_GLFW_WIN32",
			"_CRT_SECURE_NO_WARNINGS"
		}

	filter "configurations:Debug"
		runtime "Debug"
		symbols "on"

	filter "configurations:Release"
		runtime "Release"
		optimize "speed"

	filter "configurations:Dist"
		runtime "Release"
		optimize "speed"
		symbols "off"

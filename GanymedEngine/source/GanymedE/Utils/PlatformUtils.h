#pragma once

#include <filesystem>
#include <string>

namespace GanymedE {

	class FileDialogs
	{
	public:
		// These return empty strings if cancelled
		static std::string OpenFile(const char* filter);
		static std::string SaveFile(const char* filter);
	};

	// Hand-offs to other desktop programs. Both launch and return; false means the launch
	// itself failed (nothing to launch with), not that the other program later errored.
	class DesktopShell
	{
	public:
		// Opens the OS file browser on the folder holding `path`, with `path` selected where
		// the platform supports it. Works for files and directories.
		static bool RevealInFileBrowser(const std::filesystem::path& path);
		// Opens `path` in Visual Studio Code, in its last active window.
		static bool OpenInVSCode(const std::filesystem::path& path);
	};

}

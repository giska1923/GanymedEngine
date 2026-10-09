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

	// The per-user, writable directory for engine data that must survive between runs and must not
	// live beside the executable: a shipped game's install directory is read-only by design
	// (docs/runtime/runtime.md). Today it holds the online identity, profiles/<name>/device_id.
	class UserData
	{
	public:
		// %LOCALAPPDATA%\GanymedEngine on Windows; $XDG_DATA_HOME/GanymedEngine, else
		// ~/.local/share/GanymedEngine, on Linux; ~/Library/Application Support/GanymedEngine on
		// macOS. Not created here. Empty if the OS reports no such location.
		static std::filesystem::path GetDirectory();
	};

}

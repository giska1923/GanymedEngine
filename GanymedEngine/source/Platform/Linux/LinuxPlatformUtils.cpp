#include "gepch.h"
#include "GanymedE/Utils/PlatformUtils.h"

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string>

namespace GanymedE {

#ifdef GE_PLATFORM_LINUX
	std::string FileDialogs::OpenFile(const char* filter)
	{
		// Use zenity for file dialogs on Linux
		std::string command = "zenity --file-selection";
		
		// Add filter if provided
		if (filter && strlen(filter) > 0)
		{
			command += " --file-filter=\"";
			command += filter;
			command += "\"";
		}

		// Execute command and capture output
		std::array<char, 128> buffer;
		std::string result;
		std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(command.c_str(), "r"), pclose);
		
		if (!pipe) {
			GE_CORE_ERROR("Failed to run zenity command for file dialog");
			return std::string();
		}
		
		while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
			result += buffer.data();
		}
		
		// Remove trailing newline if present
		if (!result.empty() && result.back() == '\n') {
			result.pop_back();
		}
		
		return result;
	}

	std::string FileDialogs::SaveFile(const char* filter)
	{
		// Use zenity for save file dialogs on Linux
		std::string command = "zenity --file-selection --save --confirm-overwrite";
		
		// Add filter if provided
		if (filter && strlen(filter) > 0)
		{
			command += " --file-filter=\"";
			command += filter;
			command += "\"";
		}

		// Execute command and capture output
		std::array<char, 128> buffer;
		std::string result;
		std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(command.c_str(), "r"), pclose);
		
		if (!pipe) {
			GE_CORE_ERROR("Failed to run zenity command for save dialog");
			return std::string();
		}
		
		while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
			result += buffer.data();
		}
		
		// Remove trailing newline if present
		if (!result.empty() && result.back() == '\n') {
			result.pop_back();
		}
		
		return result;
	}

	namespace {

		// Single-quoted for /bin/sh: inside '...' nothing is special except ' itself.
		std::string ShellQuote(const std::string& text)
		{
			std::string quoted = "'";
			for (char c : text)
				quoted += (c == '\'') ? std::string("'\\''") : std::string(1, c);
			return quoted + "'";
		}

	}

	bool DesktopShell::RevealInFileBrowser(const std::filesystem::path& path)
	{
		// xdg-open has no "select this item"; the org.freedesktop.FileManager1 D-Bus ShowItems
		// call does, but not every file manager implements it. Opening the parent is the floor.
		if (std::system("command -v xdg-open >/dev/null 2>&1") != 0)
			return false;

		const std::string folder = path.parent_path().string();
		return std::system(("xdg-open " + ShellQuote(folder) + " >/dev/null 2>&1 &").c_str()) == 0;
	}

	bool DesktopShell::OpenInVSCode(const std::filesystem::path& path)
	{
		// The trailing & detaches it, so the exit status only says the shell forked - check the
		// launcher exists first, or a missing VS Code would report success.
		if (std::system("command -v code >/dev/null 2>&1") != 0)
			return false;

		return std::system(("code " + ShellQuote(path.string()) + " >/dev/null 2>&1 &").c_str()) == 0;
	}

#endif

}
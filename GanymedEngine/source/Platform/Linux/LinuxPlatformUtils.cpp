#include "gepch.h"
#include "GanymedE/Utils/PlatformUtils.h"

#include <cstdio>
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
#endif

}
#include "gepch.h"
#include "GanymedE/Utils/PlatformUtils.h"

#include <cstdio>
#include <memory>
#include <string>
#include <array>

namespace GanymedE {

#ifdef GE_PLATFORM_MACOS
	std::string FileDialogs::OpenFile(const char* filter)
	{
		// Use osascript to show native macOS file dialog
		std::string command = "osascript -e 'tell application \"System Events\" to return POSIX path of (choose file";
		
		// Add filter if provided (basic support for common types)
		if (filter && strlen(filter) > 0)
		{
			// Convert Windows-style filter to macOS types
			std::string filterStr(filter);
			if (filterStr.find("*.png") != std::string::npos || 
				filterStr.find("*.jpg") != std::string::npos || 
				filterStr.find("*.jpeg") != std::string::npos)
			{
				command += " of type {\"public.image\"}";
			}
			else if (filterStr.find("*.txt") != std::string::npos)
			{
				command += " of type {\"public.plain-text\"}";
			}
			// Add more filter types as needed
		}
		
		command += ")'";

		// Execute command and capture output
		std::array<char, 128> buffer;
		std::string result;
		std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(command.c_str(), "r"), pclose);
		
		if (!pipe) {
			GE_CORE_ERROR("Failed to run osascript command for file dialog");
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
		// Use osascript to show native macOS save dialog
		std::string command = "osascript -e 'tell application \"System Events\" to return POSIX path of (choose file name";
		
		// Add default name
		command += " default name \"Untitled\"";
		
		command += ")'";

		// Execute command and capture output
		std::array<char, 128> buffer;
		std::string result;
		std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(command.c_str(), "r"), pclose);
		
		if (!pipe) {
			GE_CORE_ERROR("Failed to run osascript command for save dialog");
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
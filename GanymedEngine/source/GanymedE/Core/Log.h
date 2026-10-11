#pragma once

#include "Core.h"

// This ignores all warnings raised inside External headers
#pragma warning(push, 0)
#include "spdlog/spdlog.h"
#include "spdlog/fmt/ostr.h"
#pragma warning(pop)

namespace GanymedE {
	class GE_API Log
	{
	public:
		static void Init();

		// Before the first log line. `--root` becomes the working directory, so assets, shaders
		// and this process's log file all resolve there. `--server-id` selects the dedicated
		// log (stderr plus `dedicated-<id>.log`) instead of the shared `GanymedE.log`.
		// False: `error` is set and nothing was opened. `main` exits 2.
		static bool Prepare(int argc, char** argv, std::string& error);

		inline static std::shared_ptr<spdlog::logger>& GetCoreLogger() { return s_CoreLogger; }
		inline static std::shared_ptr<spdlog::logger>& GetClientLogger() { return s_ClientLogger; }
	private:
		static std::shared_ptr<spdlog::logger> s_CoreLogger;
		static std::shared_ptr<spdlog::logger> s_ClientLogger;
	};
}

//THESE MACROS SHOULD BE USED FOR INVOKING THE CORE LOGGER
#define GE_CORE_TRACE(...) ::GanymedE::Log::GetCoreLogger()->trace(__VA_ARGS__)
#define GE_CORE_INFO(...) ::GanymedE::Log::GetCoreLogger()->info(__VA_ARGS__)
#define GE_CORE_WARN(...) ::GanymedE::Log::GetCoreLogger()->warn(__VA_ARGS__)
#define GE_CORE_ERROR(...) ::GanymedE::Log::GetCoreLogger()->error(__VA_ARGS__)
#define GE_CORE_FATAL(...) ::GanymedE::Log::GetCoreLogger()->fatal(__VA_ARGS__)

//THESE MACROS SHOULD BE USED FOR INVOKING THE CLIENT LOGGER
#define GE_TRACE(...) ::GanymedE::Log::GetClientLogger()->trace(__VA_ARGS__)
#define GE_INFO(...) ::GanymedE::Log::GetClientLogger()->info(__VA_ARGS__)
#define GE_WARN(...) ::GanymedE::Log::GetClientLogger()->warn(__VA_ARGS__)
#define GE_ERROR(...) ::GanymedE::Log::GetClientLogger()->error(__VA_ARGS__)
#define GE_FATAL(...) ::GanymedE::Log::GetClientLogger()->fatal(__VA_ARGS__)

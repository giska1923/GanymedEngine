#pragma once

#include <memory>

#include "Core.h"
#include "spdlog/spdlog.h"
#include "spdlog/fmt/ostr.h"

namespace GanymedE {
	class GE_API Log {
	public:
		static void Init();

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
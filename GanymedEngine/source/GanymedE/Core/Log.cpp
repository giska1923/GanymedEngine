#include "gepch.h"
#include "Log.h"

#include <spdlog/sinks/basic_file_sink.h>

#ifdef GE_PLATFORM_WINDOWS
#include <spdlog/sinks/msvc_sink.h>
#else
#include <spdlog/sinks/stdout_color_sinks.h>
#endif

namespace GanymedE {
	std::shared_ptr<spdlog::logger> Log::s_CoreLogger;
	std::shared_ptr<spdlog::logger> Log::s_ClientLogger;

	void Log::Init()
	{
		std::vector<spdlog::sink_ptr> logSinks;
#ifdef GE_PLATFORM_WINDOWS
		// Routes to Visual Studio's Output window (Debug pane) when debugging
		logSinks.emplace_back(std::make_shared<spdlog::sinks::msvc_sink_mt>());
#else
		logSinks.emplace_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
#endif
		logSinks.emplace_back(std::make_shared<spdlog::sinks::basic_file_sink_mt>("GanymedE.log", true));

		logSinks[0]->set_pattern("%^[%T] %n: %v%$");
		logSinks[1]->set_pattern("[%T] [%l] %n: %v");

		// ---- Level and flush policy, which differ in Dist and have to ----
		//
		// Every configuration used to log at `trace` and flush **on every line**. In an editor
		// that is what you want: the last line before a crash is on disk, and the volume is a
		// scene load's worth.
		//
		// In a shipped game it is neither. A run of the Proving Ground's P7 gate wrote 15,463
		// trace lines and 1.7 MB in 150 seconds - one line per spawned entity, each one
		// individually flushed to disk - because gameplay spawns thousands of prefabs and the
		// deserializer traces each. That is a synchronous write in the middle of a frame, and it
		// buys nothing: nobody reads a shipped game's trace log, and the lines carry build
		// machine paths.
		//
		// So Dist logs `info` and above and flushes on `warn`. A user's bug report still arrives
		// with the boot banner, the asset scan, every warning and every error - which is what a
		// log is for once there is no debugger attached - and a quiet frame writes nothing.
	#ifdef GE_DIST
		constexpr auto level = spdlog::level::info;
		constexpr auto flushLevel = spdlog::level::warn;
	#else
		constexpr auto level = spdlog::level::trace;
		constexpr auto flushLevel = spdlog::level::trace;
	#endif

		s_CoreLogger = std::make_shared<spdlog::logger>("GanymedEngine", begin(logSinks), end(logSinks));
		spdlog::register_logger(s_CoreLogger);
		s_CoreLogger->set_level(level);
		s_CoreLogger->flush_on(flushLevel);

		s_ClientLogger = std::make_shared<spdlog::logger>("APP", begin(logSinks), end(logSinks));
		spdlog::register_logger(s_ClientLogger);
		s_ClientLogger->set_level(level);
		s_ClientLogger->flush_on(flushLevel);

		// Dist drops the per-line flush, so anything still buffered when the process ends would
		// be lost - including the last words of a crash, which is the one case that matters.
		// spdlog's own atexit handler covers a clean exit; this covers the rest.
		std::atexit([]() { spdlog::shutdown(); });
	}
}




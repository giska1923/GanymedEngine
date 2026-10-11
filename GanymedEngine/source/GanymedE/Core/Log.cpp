#include "gepch.h"
#include "Log.h"

#include <filesystem>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_sinks.h>

#ifdef GE_PLATFORM_WINDOWS
#include <spdlog/sinks/msvc_sink.h>
#else
#include <spdlog/sinks/stdout_color_sinks.h>
#endif

namespace GanymedE {
	std::shared_ptr<spdlog::logger> Log::s_CoreLogger;
	std::shared_ptr<spdlog::logger> Log::s_ClientLogger;

	namespace {

		// `--name value` and `--name=value`, plus the single dash Go's flag package prints.
		// False when this argument is a different flag. A missing value is reported via `error`.
		bool TakeFlag(int argc, char** argv, int& index, const char* name, const char*& value, std::string& error)
		{
			const char* arg = argv[index];
			if (!arg)
				return false;

			const std::string dashed = std::string("--") + name;
			const std::string single = std::string("-") + name;
			const std::string text = arg;

			if (text == dashed || text == single)
			{
				if (index + 1 >= argc || !argv[index + 1] || argv[index + 1][0] == '\0')
				{
					error = dashed + " needs a value";
					return true;
				}
				value = argv[++index];
				return true;
			}

			const std::string dashedEq = dashed + "=";
			if (text.rfind(dashedEq, 0) == 0)
			{
				value = arg + dashedEq.size();
				if (!*value)
					error = dashed + " needs a value";
				return true;
			}
			const std::string singleEq = single + "=";
			if (text.rfind(singleEq, 0) == 0)
			{
				value = arg + singleEq.size();
				if (!*value)
					error = single + " needs a value";
				return true;
			}
			return false;
		}

		// The id is also a URL path segment. Fold anything that is not a file-name character so
		// a surprising id cannot leave this directory. UUIDs pass through unchanged.
		std::string ServerLogFile(const std::string& serverId)
		{
			std::string safe;
			safe.reserve(serverId.size());
			for (unsigned char c : serverId)
			{
				const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
					|| (c >= '0' && c <= '9') || c == '-' || c == '_';
				safe.push_back(ok ? static_cast<char>(c) : '_');
				if (safe.size() == 80)
					break;
			}
			return "dedicated-" + safe + ".log";
		}

		void Install(std::vector<spdlog::sink_ptr> logSinks)
		{
			// Dist logs info and above and flushes on warn. Every other configuration logs trace
			// and flushes every line, so the last line before a crash is already on disk. A
			// shipped game's trace log is a synchronous write in the middle of a frame (the
			// Proving Ground's P7 gate wrote 15,463 lines and 1.7 MB in 150 seconds, one per
			// spawned prefab), and nobody reads it. The atexit flush is what Dist loses the
			// per-line flush for: the last words of a crash still land.
#ifdef GE_DIST
			constexpr auto level = spdlog::level::info;
			constexpr auto flushLevel = spdlog::level::warn;
#else
			constexpr auto level = spdlog::level::trace;
			constexpr auto flushLevel = spdlog::level::trace;
#endif
			auto& core = Log::GetCoreLogger();
			core = std::make_shared<spdlog::logger>("GanymedEngine", begin(logSinks), end(logSinks));
			spdlog::register_logger(core);
			core->set_level(level);
			core->flush_on(flushLevel);

			auto& client = Log::GetClientLogger();
			client = std::make_shared<spdlog::logger>("APP", begin(logSinks), end(logSinks));
			spdlog::register_logger(client);
			client->set_level(level);
			client->flush_on(flushLevel);

			std::atexit([]() { spdlog::shutdown(); });
		}

	}

	void Log::Init()
	{
		std::vector<spdlog::sink_ptr> logSinks;
#ifdef GE_PLATFORM_WINDOWS
		// Routes to Visual Studio's Output window (Debug pane) when debugging. There is no
		// console on the editor: it is a WindowedApp, and this sink is what a debugger shows.
		logSinks.emplace_back(std::make_shared<spdlog::sinks::msvc_sink_mt>());
#else
		logSinks.emplace_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
#endif
		// Truncate. One editor, one file. A dedicated process must not call this: a pool of
		// servers would erase each other's match.
		logSinks.emplace_back(std::make_shared<spdlog::sinks::basic_file_sink_mt>("GanymedE.log", true));

		logSinks[0]->set_pattern("%^[%T] %n: %v%$");
		logSinks[1]->set_pattern("[%T] [%l] %n: %v");
		Install(std::move(logSinks));
	}

	bool Log::Prepare(int argc, char** argv, std::string& error)
	{
		const char* root = nullptr;
		const char* serverId = nullptr;
		for (int i = 1; i < argc; i++)
		{
			const char* value = nullptr;
			if (TakeFlag(argc, argv, i, "root", value, error))
			{
				if (!error.empty())
					return false;
				root = value;
			}
			else if (TakeFlag(argc, argv, i, "server-id", value, error))
			{
				if (!error.empty())
					return false;
				serverId = value;
			}
		}

		if (root)
		{
			// Relative to wherever the agent launched us. Everything opened after this —
			// the log, dedicated.yaml, shaders, the asset root — is relative to here.
			std::error_code ec;
			std::filesystem::current_path(root, ec);
			if (ec)
			{
				error = std::string("--root '") + root + "' is not a directory (" + ec.message() + ")";
				return false;
			}
		}

		// `--dev` has no value, so TakeFlag will not see it. A dev server is still a dedicated
		// process: it must not truncate the editor's GanymedE.log. Without an id the file is
		// dedicated-dev.log.
		bool dev = false;
		for (int i = 1; i < argc; i++)
		{
			const char* arg = argv[i];
			if (arg && (std::string(arg) == "--dev" || std::string(arg) == "-dev"))
				dev = true;
		}
		if (!serverId && !dev)
		{
			Init();
			return true;
		}

		const char* logId = serverId ? serverId : "dev";

		std::vector<spdlog::sink_ptr> logSinks;
#ifdef GE_PLATFORM_WINDOWS
		logSinks.emplace_back(std::make_shared<spdlog::sinks::msvc_sink_mt>());
		logSinks.back()->set_pattern("%^[%T] %n: %v%$");
#endif
		// The agent captures stderr, which is where stubserver writes. No color codes: the
		// capture is a log, not a terminal. Append, so a replacement that reuses this server
		// id does not erase the match it replaced. The name is per id, so a pool does not
		// share one file, and GanymedE.log is never opened.
		auto err = std::make_shared<spdlog::sinks::stderr_sink_mt>();
		err->set_pattern("[%T] [%l] %n: %v");
		logSinks.push_back(std::move(err));

		const std::string file = ServerLogFile(logId);
		try
		{
			auto disk = std::make_shared<spdlog::sinks::basic_file_sink_mt>(file, /*truncate=*/false);
			disk->set_pattern("[%T] [%l] %n: %v");
			logSinks.push_back(std::move(disk));
			Install(std::move(logSinks));
		}
		catch (const spdlog::spdlog_ex& e)
		{
			error = "could not open " + file + " (" + e.what() + ")";
			return false;
		}
		return true;
	}
}




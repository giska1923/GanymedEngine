#include "DedicatedConfig.h"

#include "GanymedE/Core/Log.h"
#include "GanymedE/Net/Net.h"
#include "GanymedE/Online/ConnectToken.h"

#include <cstdlib>
#include <filesystem>
#include <yaml-cpp/yaml.h>

namespace GanymedE {

	namespace {

		template<typename T>
		void ReadInto(const YAML::Node& root, const char* key, T& out)
		{
			if (const YAML::Node node = root[key])
				out = node.as<T>();
		}

		// `name` has no dashes. Accepts --name, --name=value, and the single-dash form the Go
		// flag package prints, because fleetagent's example command uses that form.
		bool TakeFlag(int argc, char** argv, int& index, const char* name, const char*& value)
		{
			const char* arg = argv[index];
			if (!arg)
				return false;

			const std::string dashed = std::string("--") + name;
			const std::string single = std::string("-") + name;
			const std::string text = arg;

			if (text == dashed || text == single)
			{
				if (index + 1 >= argc || !argv[index + 1])
					return false;
				value = argv[++index];
				return true;
			}

			const std::string dashedEq = dashed + "=";
			const std::string singleEq = single + "=";
			if (text.rfind(dashedEq, 0) == 0)
			{
				value = arg + dashedEq.size();
				return true;
			}
			if (text.rfind(singleEq, 0) == 0)
			{
				value = arg + singleEq.size();
				return true;
			}
			return false;
		}

		bool ParseFloat(const char* text, float& out)
		{
			if (!text || !*text)
				return false;
			char* end = nullptr;
			out = std::strtof(text, &end);
			return end != text && *end == '\0';
		}

		bool ParseInt(const char* text, int& out)
		{
			if (!text || !*text)
				return false;
			char* end = nullptr;
			long value = std::strtol(text, &end, 10);
			if (end == text || *end != '\0')
				return false;
			out = static_cast<int>(value);
			return true;
		}

	}

	DedicatedConfig DedicatedConfig::Load(const std::string& path)
	{
		DedicatedConfig config;
		try
		{
			if (!std::filesystem::exists(path))
			{
				GE_CORE_WARN("Dedicated config '{0}' not found - using defaults", path);
				return config;
			}

			YAML::Node root = YAML::LoadFile(path);
			ReadInto(root, "AssetRoot", config.AssetRoot);
			ReadInto(root, "Scene", config.Scene);
			ReadInto(root, "MatchSeconds", config.MatchSeconds);
			ReadInto(root, "LoadDelaySeconds", config.LoadDelaySeconds);
			ReadInto(root, "Outcome", config.Outcome);
		}
		catch (const YAML::Exception& e)
		{
			GE_CORE_ERROR("Dedicated config '{0}' failed to parse ({1}) - using defaults", path, e.what());
			return DedicatedConfig{};
		}
		return config;
	}

	DedicatedConfig ConfigFromCommandLine(DedicatedConfig config, int argc, char** argv, std::string& error, bool& ok)
	{
		ok = false;
		std::string publicKey;
		std::string netSim;
		std::string netTest;
		for (int i = 1; i < argc; i++)
		{
			const char* arg = argv[i];
			if (arg && (std::string(arg) == "--dev" || std::string(arg) == "-dev"))
			{
				config.Dev = true;
				continue;
			}

			const char* value = nullptr;
			if (TakeFlag(argc, argv, i, "server-id", value))
				config.ServerId = value;
			else if (TakeFlag(argc, argv, i, "agent", value))
				config.Agent = value;
			else if (TakeFlag(argc, argv, i, "game-port", value))
			{
				if (!ParseInt(value, config.GamePort))
				{
					error = "--game-port is not an integer";
					return config;
				}
			}
			else if (TakeFlag(argc, argv, i, "advertise", value))
				config.Advertise = value;
			else if (TakeFlag(argc, argv, i, "public-key", value))
				publicKey = value;
			else if (TakeFlag(argc, argv, i, "match-seconds", value))
			{
				if (!ParseFloat(value, config.MatchSeconds))
				{
					error = "--match-seconds is not a number";
					return config;
				}
			}
			else if (TakeFlag(argc, argv, i, "outcome", value))
				config.Outcome = value;
			else if (TakeFlag(argc, argv, i, "scene", value))
				config.Scene = value;
			else if (TakeFlag(argc, argv, i, "load-delay", value))
			{
				if (!ParseFloat(value, config.LoadDelaySeconds))
				{
					error = "--load-delay is not a number";
					return config;
				}
			}
			else if (TakeFlag(argc, argv, i, "net-sim", value))
				netSim = value;
			else if (TakeFlag(argc, argv, i, "net-test", value))
				netTest = value;
		}

		if (!Net::ParseSim(netSim, config.Sim, error))
			return config;
		if (!netTest.empty() && netTest != "move" && netTest != "spawn"
			&& netTest != "prefab" && netTest != "rest")
		{
			error = "--net-test on the server is move, spawn, prefab, or rest";
			return config;
		}
		config.NetTest = netTest;

		if (config.Dev)
		{
			if (config.GamePort == 0)
				config.GamePort = 7777;
			const std::string loopback = "127.0.0.1:" + std::to_string(config.GamePort);
			if (config.Advertise.empty())
				config.Advertise = loopback;
			else
			{
				Net::Address advertised;
				if (!Net::ParseAddress(config.Advertise, advertised, error) || !advertised.IsLoopback()
					|| advertised.Port != static_cast<uint16_t>(config.GamePort))
				{
					error = "--dev listens on 127.0.0.1:" + std::to_string(config.GamePort) + " only";
					return config;
				}
			}
			ok = true;
			return config;
		}

		if (config.ServerId.empty() || config.Agent.empty() || config.GamePort == 0
			|| config.Advertise.empty() || publicKey.empty())
		{
			error = "missing flags: --server-id, --agent, --game-port, --advertise and --public-key are required";
			return config;
		}
		if (config.Outcome != "victory" && config.Outcome != "defeat")
		{
			error = "--outcome must be victory or defeat";
			return config;
		}
		if (config.MatchSeconds <= 0.0f)
		{
			error = "--match-seconds must be positive";
			return config;
		}
		if (config.LoadDelaySeconds < 0.0f)
		{
			error = "--load-delay must not be negative";
			return config;
		}
		if (!ParseConnectPublicKey(publicKey, config.PublicKey))
		{
			error = "--public-key is not a 32-byte base64url Ed25519 key";
			return config;
		}

		ok = true;
		return config;
	}

}

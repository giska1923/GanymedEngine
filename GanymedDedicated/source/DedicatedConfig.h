#pragma once

#include <cstdint>
#include <string>

namespace GanymedE {

	// assets/dedicated.yaml, plus the flags the fleet agent appends. The five agent flags are
	// required; everything else has a default so a hand run can omit it.
	struct DedicatedConfig
	{
		std::string AssetRoot = "assets";
		std::string Scene = "scenes/BoxesPhysicsExample.ganymede";
		float MatchSeconds = 10.0f;
		float LoadDelaySeconds = 0.0f;
		std::string Outcome = "victory";

		std::string ServerId;
		std::string Agent;
		int GamePort = 0;
		std::string Advertise;
		uint8_t PublicKey[32]{};

		static DedicatedConfig Load(const std::string& path);
	};

	// `ok` is false when a required flag is missing or a value cannot be read. `error` says which.
	DedicatedConfig ConfigFromCommandLine(DedicatedConfig config, int argc, char** argv, std::string& error, bool& ok);

}

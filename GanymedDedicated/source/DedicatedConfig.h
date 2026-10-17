#pragma once

#include "GanymedE/Net/Net.h"

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

		// Loopback, no agent, `HELLO dev:<name>`. The match clock does not run: the
		// process stays up until it is killed, which is what a transport test needs.
		bool Dev = false;
		Net::Sim Sim;

		// move, spawn, prefab, or rest. Empty on a fleet server. See netcode.md.
		std::string NetTest;

		static DedicatedConfig Load(const std::string& path);
	};

	// `ok` is false when a required flag is missing or a value cannot be read. `error` says which.
	DedicatedConfig ConfigFromCommandLine(DedicatedConfig config, int argc, char** argv, std::string& error, bool& ok);

}

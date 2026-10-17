#pragma once

#include "GanymedE/Core/UUID.h"
#include "GanymedE/Net/Command.h"

#include <string>

namespace GanymedE {

	class Entity;
	class Scene;
	class PhysicsScene;

	// `--net-test` modes: the scripted movers, arenas, synthetic sticks and stats lines that let a
	// netcode phase be verified headless. None of it is game behaviour, and none of it is in
	// World.cpp: replication calls these hooks, and each one is a no-op when no mode is set.
	// The transport's own probes (`reliable:<n>`, `spoof`) live in Host.cpp beside the channel
	// they test.
	namespace Net::Test {

		// After OnRuntimeStart, on the server or the client. An unknown mode logs and stays off.
		void Start(Scene& scene, const std::string& mode);

		// Dedicated server, once a frame, outside the system update: spawns and destroys.
		void Advance(Scene& scene);

		// Inside BeginStep, before the scripts tick (the desync teleport).
		void BeforeScripts(Scene& scene, PhysicsScene* physics, uint32_t tick);

		// After BeginStep, before the physics step (the scripted mover).
		void AfterScripts(Scene& scene);

		// Server, after BeginStep's scripts: the once-a-second predict line.
		void AfterServerStep(Scene& scene, uint32_t tick);

		// Standalone, after BeginStep's scripts (the offline walk).
		void AfterStandaloneStep(Scene& scene, uint32_t tick);

		// Any mode is set. Mouse look is off then: a headless run has no cursor to trust.
		bool Active();

		// True when a synthetic stick replaces the keyboard. `Stick` fills it in.
		bool DrivesInput();
		void Stick(PlayerCommand& cmd, uint32_t settleFrom);

		// Client, after prediction this frame. Ends a predict run once its span has elapsed.
		struct PredictProgress
		{
			uint32_t SimTick = 0;
			uint32_t SettleFrom = 0;
			int Corrections = 0;
			float MaxCorrectMm = 0.0f;
			int MaxReplay = 0;
		};
		void AfterPredict(const PredictProgress& progress);

		// Client, when a snapshot spawned a prefab.
		void OnClientSpawn(Scene& scene, Entity root, const std::string& prefab);

	}

}

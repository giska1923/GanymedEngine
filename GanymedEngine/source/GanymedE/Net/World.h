#pragma once

#include "GanymedE/Core/UUID.h"
#include "GanymedE/Net/ScriptWire.h"

#include <string>
#include <vector>

namespace GanymedE {

	class Scene;
	class PhysicsScene;

	namespace Net {

		// Client only, and before the scene update: applying a spawn is CreateEntity, which
		// asserts while a system is running. Interpolation is written here too, so the physics
		// step that follows sees the pose. The locally controlled entity is left for prediction.
		void PresentWorld(Scene& scene, float frameDt);

		// Client only, after bodies exist and before the accumulator. Reconciles the owned
		// character, then simulates it ahead by a full round trip.
		void PredictLocal(Scene& scene, PhysicsScene* physics);

		// Server and standalone, once per physics step, before that step. OnTick.
		void BeginStep(Scene& scene, PhysicsScene* physics);

		// Client only, after the final SyncTransforms. Adds the decaying visual error.
		void ApplyVisualError(Scene& scene, float frameDt);

		// The entity this client predicts. False on a server, and for every other entity.
		bool Controls(UUID id);

		// Server only. Once per physics step, after that step.
		void NoteSimulatedStep(Scene& scene);

		// Server only. After the steps: the delta against what that client has acknowledged.
		void CaptureWorld(Scene& scene, PhysicsScene* physics);

		// Server. Instantiates a prefab that replicates: a client spawns the same path with the
		// same root UUID, and its children get the same derived UUIDs. `relative` is under the
		// asset root. Not safe during a system update. Zero when the prefab did not load.
		UUID SpawnReplicatedPrefab(Scene& scene, const std::string& relative);

		// Server. From now on this account's commands drive this entity, and the snapshots sent
		// to that account name it as the one it predicts.
		void AssignPawn(const std::string& account, UUID id);
		bool HasPawn(const std::string& account);

		// Server. One pawn: the account, its entity, and the server tick it was assigned on.
		struct PawnInfo
		{
			std::string Account;
			UUID Id{ 0 };
			uint32_t AssignedTick = 0;
		};
		std::vector<PawnInfo> Pawns();

		// Server. The last tick the physics step finished.
		uint32_t ServerTick();

		// Server. The game decided the match: "victory" or "defeat". The dedicated process takes
		// it once (TakeMatchEnd) and reports it through its agent. The first call wins.
		bool RequestMatchEnd(const std::string& outcome);
		bool TakeMatchEnd(std::string& outcome);

		// Server. Ticks that reused the previous command because the real one had not arrived,
		// summed over accounts, in total and since each account first moved its stick.
		struct InputStats
		{
			uint32_t Repeats = 0;
			uint32_t RepeatsSinceMove = 0;
		};
		InputStats GetInputStats();

		// Client only, after new scripts exist and before OnUpdate. Writes replicated fields,
		// then delivers events that arrived this frame or were held for an entity that had
		// not spawned yet.
		void DeliverScriptTraffic(Scene& scene);

		// Server. Assigns a net id the first time a replicated entity asks, the same counter
		// Gather uses, so a broadcast before the first snapshot still names the entity.
		uint16_t EnsureNetId(Scene& scene, UUID id);

		// The tick BeginStep is running. Zero before the first step.
		uint32_t CurrentScriptTick();

		// Server, from the script that is currently executing. Unreliable rides the next
		// snapshot and is not retried. Reliable is a new message on the reliable channel.
		void BroadcastEvent(uint16_t netId, std::string name, std::vector<EventArg> args, bool reliable);
		void BroadcastShot(uint16_t netId, float muzzleX, float muzzleY, float muzzleZ,
			float dirX, float dirY, float dirZ, float speed, uint32_t tick);

	}

}

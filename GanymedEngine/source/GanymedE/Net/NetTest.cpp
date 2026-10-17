#include "gepch.h"
#include "GanymedE/Net/NetTest.h"

#include "GanymedE/Core/Log.h"
#include "GanymedE/Net/Net.h"
#include "GanymedE/Net/World.h"
#include "GanymedE/Physics/PhysicsScene.h"
#include "GanymedE/Scene/Components.h"
#include "GanymedE/Scene/Entity.h"
#include "GanymedE/Scene/Scene.h"
#include "GanymedE/Scene/SceneRole.h"
#include "GanymedE/main/Application.h"

#include <chrono>
#include <cmath>
#include <unordered_set>
#include <vector>

namespace GanymedE::Net::Test {

	namespace {

		enum class Mode
		{
			None, Move, Spawn, Prefab, Rest, Churn,
			Predict, PredictLoss, PredictDesync, PredictWall, PredictStep, Offline, Fields
		};

		// The 500 of `spawn` are destroyed at 18 s of server ticks.
		constexpr uint32_t kDestroyTick = 60 * 18;

		// `churn`: one entity a frame, each destroyed 3 ticks after it was made, for 20 s.
		// A life that short is inside one reorder hold, so a held snapshot that spawned an
		// entity can arrive after the one that destroyed it. That is what the client must
		// survive: it has to end with no churn entity left.
		constexpr uint32_t kChurnLife = 3;
		constexpr uint32_t kChurnUntil = 60 * 20;

		struct TestState
		{
			Mode Current = Mode::None;
			UUID Mover{ 0 };
			UUID PrefabRoot{ 0 };
			std::vector<UUID> Spawned;
			bool SpawnGone = false;
			std::vector<std::pair<UUID, uint32_t>> Churn;
			uint32_t Churned = 0;
			bool ChurnLogged = false;
			bool PrefabLogged = false;
			bool ClientPrefabLogged = false;
			bool Arena = false;
			bool Done = false;
			std::unordered_set<std::string> Teleported;
			std::chrono::steady_clock::time_point NextServerLog{};
		};

		TestState& State()
		{
			static TestState state;
			return state;
		}

		bool IsPredict(Mode mode)
		{
			return mode == Mode::Predict || mode == Mode::PredictLoss || mode == Mode::PredictDesync
				|| mode == Mode::PredictWall || mode == Mode::PredictStep;
		}

		uint32_t PredictSpan(Mode mode)
		{
			if (mode == Mode::Predict)
				return 3600;
			if (mode == Mode::PredictLoss)
				return 1200;
			return 600;
		}

		glm::vec3 MoverPosition(uint32_t tick)
		{
			const float t = static_cast<float>(tick);
			return { std::sin(t * 0.02f) * 3.0f, 1.0f, t * 0.02f };
		}

		void WriteMover(Scene& scene, UUID id, uint32_t tick)
		{
			Entity entity = scene.FindEntityByUUID(id);
			if (!entity)
				return;
			auto& transform = entity.GetComponent<TransformComponent>();
			transform.Translation = MoverPosition(tick);
			transform.Rotation = { 0.0f, static_cast<float>(tick) * 0.01f, 0.0f };
			scene.MarkChanged<TransformComponent>(entity);
		}

		void MakeBox(Scene& scene, const char* name, const glm::vec3& position, const glm::vec3& halfExtents)
		{
			Entity entity = scene.CreateEntity(name);
			entity.GetComponent<TransformComponent>().Translation = position;
			scene.MarkChanged<TransformComponent>(entity);
			entity.AddComponent<RigidBodyComponent>().Type = RigidBodyType::Static;
			entity.AddComponent<BoxColliderComponent>().HalfExtents = halfExtents;
		}

		void BuildArena(Scene& scene, TestState& state)
		{
			if (state.Arena)
				return;
			state.Arena = true;
			// 4 m/s for 60 s is 240 m of +Z, and the wall slide is 4 m/s of +X for 10 s.
			// A 20 m pad ends the flat walk in the air, which is a different test.
			MakeBox(scene, "net-floor", { 20.0f, -0.5f, 130.0f }, { 40.0f, 0.5f, 160.0f });
			if (state.Current == Mode::PredictWall)
				MakeBox(scene, "net-wall", { 20.0f, 1.0f, 4.0f }, { 30.0f, 1.0f, 0.25f });
			if (state.Current == Mode::PredictStep)
				MakeBox(scene, "net-step", { 0.0f, 0.1f, 3.0f }, { 2.0f, 0.1f, 1.0f });
		}

		void LogPrefab(const char* role, Entity root, bool& logged)
		{
			if (logged || !root)
				return;
			uint64_t child = 0;
			if (root.HasComponent<RelationshipComponent>())
			{
				const auto& children = root.GetComponent<RelationshipComponent>().Children;
				if (!children.empty())
					child = static_cast<uint64_t>(children.front());
			}
			char rootHex[32];
			char childHex[32];
			std::snprintf(rootHex, sizeof(rootHex), "%016llx", static_cast<unsigned long long>(root.GetUUID()));
			std::snprintf(childHex, sizeof(childHex), "%016llx", static_cast<unsigned long long>(child));
			GE_CORE_INFO("Replicate prefab {0} root={1} child={2}", role, rootHex, childHex);
			logged = true;
		}

		// Every account that has a session and no pawn gets one. The game assigns pawns itself
		// at N5; until then this is the only caller of AssignPawn.
		void EnsurePawns(Scene& scene, TestState& state)
		{
			const bool fields = state.Current == Mode::Fields;
			if ((!IsPredict(state.Current) && !fields) || scene.GetRole() != SceneRole::Server)
				return;
			const char* prefab = fields ? "prefabs/NetEcho.gprefab" : "prefabs/NetWalker.gprefab";
			for (const std::string& account : ServerAccounts())
			{
				if (HasPawn(account))
					continue;
				const UUID id = SpawnReplicatedPrefab(scene, prefab);
				if (id == UUID{ 0 })
					continue;
				AssignPawn(account, id);
				GE_CORE_INFO("{0} spawn account={1} tick={2}", fields ? "Field" : "Predict", account, ServerTick());
			}
		}

	}

	void Start(Scene& scene, const std::string& mode)
	{
		TestState& state = State();
		if (mode.empty() || mode == "spoof" || mode == "reliable-flood" || mode.rfind("reliable:", 0) == 0)
			return;
		if (mode == "move")
			state.Current = Mode::Move;
		else if (mode == "spawn")
			state.Current = Mode::Spawn;
		else if (mode == "prefab")
			state.Current = Mode::Prefab;
		else if (mode == "rest")
			state.Current = Mode::Rest;
		else if (mode == "churn")
			state.Current = Mode::Churn;
		else if (mode == "predict")
			state.Current = Mode::Predict;
		else if (mode == "predict-loss")
			state.Current = Mode::PredictLoss;
		else if (mode == "predict-desync")
			state.Current = Mode::PredictDesync;
		else if (mode == "predict-wall")
			state.Current = Mode::PredictWall;
		else if (mode == "predict-step")
			state.Current = Mode::PredictStep;
		else if (mode == "fields")
			state.Current = Mode::Fields;
		else if (mode == "offline")
		{
			if (scene.GetRole() != SceneRole::Standalone)
			{
				GE_CORE_ERROR("Predict offline is a standalone runtime");
				return;
			}
			state.Current = Mode::Offline;
		}
		else
		{
			GE_CORE_ERROR("Replicate test '{0}' is not a known mode", mode);
			return;
		}

		if (IsPredict(state.Current) || state.Current == Mode::Offline)
		{
			BuildArena(scene, state);
			if (state.Current == Mode::Offline)
			{
				// Standalone: nothing replicates, so the walker is a plain prefab.
				state.Mover = SpawnReplicatedPrefab(scene, "prefabs/NetWalker.gprefab");
				Entity walker = scene.FindEntityByUUID(state.Mover);
				if (walker && walker.HasComponent<ReplicatedComponent>())
					walker.RemoveComponent<ReplicatedComponent>();
			}
			GE_CORE_INFO("Predict test {0}", mode);
			return;
		}

		GE_CORE_INFO("Replicate test {0}", mode);
		if (state.Current == Mode::Move)
		{
			state.Mover = SpawnReplicatedPrefab(scene, "prefabs/NetMover.gprefab");
		}
		else if (state.Current == Mode::Rest)
		{
			Entity entity = scene.CreateEntity("net-rest");
			entity.AddComponent<ReplicatedComponent>();
			entity.GetComponent<TransformComponent>().Translation = { 2.0f, 1.0f, 0.0f };
			scene.MarkChanged<TransformComponent>(entity);
		}
		else if (state.Current == Mode::Spawn)
		{
			state.Spawned.reserve(500);
			for (int i = 0; i < 500; i++)
			{
				Entity entity = scene.CreateEntity("net-spawn");
				entity.AddComponent<ReplicatedComponent>();
				entity.GetComponent<TransformComponent>().Translation = { static_cast<float>(i) * 0.1f, 0.0f, 0.0f };
				state.Spawned.push_back(entity.GetUUID());
			}
			GE_CORE_INFO("Replicate phase spawn");
		}
		else if (state.Current == Mode::Prefab)
		{
			state.PrefabRoot = SpawnReplicatedPrefab(scene, "prefabs/NetPair.gprefab");
			LogPrefab("server", scene.FindEntityByUUID(state.PrefabRoot), state.PrefabLogged);
		}
	}

	void Advance(Scene& scene)
	{
		TestState& state = State();
		if (state.Current == Mode::None || scene.GetRole() != SceneRole::Server)
			return;
		EnsurePawns(scene, state);
		if (state.Current == Mode::Churn)
		{
			const uint32_t tick = ServerTick();
			for (auto it = state.Churn.begin(); it != state.Churn.end(); )
			{
				if (tick < it->second + kChurnLife)
				{
					++it;
					continue;
				}
				Entity entity = scene.FindEntityByUUID(it->first);
				if (entity)
					scene.DestroyEntity(entity);
				it = state.Churn.erase(it);
			}
			if (tick > 0 && tick < kChurnUntil && !ServerAccounts().empty())
			{
				Entity entity = scene.CreateEntity("net-churn");
				entity.AddComponent<ReplicatedComponent>();
				entity.GetComponent<TransformComponent>().Translation = { static_cast<float>(state.Churned % 50), 0.0f, 0.0f };
				scene.MarkChanged<TransformComponent>(entity);
				state.Churn.emplace_back(entity.GetUUID(), tick);
				state.Churned++;
			}
			if (tick >= kChurnUntil && state.Churn.empty() && !state.ChurnLogged)
			{
				state.ChurnLogged = true;
				GE_CORE_INFO("Churn done spawned={0}; the server holds none of them now", state.Churned);
			}
			return;
		}
		if (state.Current != Mode::Spawn || state.SpawnGone || ServerTick() < kDestroyTick)
			return;
		for (UUID id : state.Spawned)
		{
			Entity entity = scene.FindEntityByUUID(id);
			if (entity)
				scene.DestroyEntity(entity);
		}
		state.Spawned.clear();
		state.SpawnGone = true;
		GE_CORE_INFO("Replicate phase destroy");
	}

	void BeforeScripts(Scene& scene, PhysicsScene* physics, uint32_t tick)
	{
		TestState& state = State();
		if (state.Current != Mode::PredictDesync || scene.GetRole() != SceneRole::Server || !physics)
			return;
		for (const PawnInfo& pawn : Pawns())
		{
			// An even tick, so the snapshot that carries the move is the teleport's own tick.
			uint32_t when = pawn.AssignedTick + 180;
			if ((when % 2) != 0)
				when++;
			if (tick != when || state.Teleported.count(pawn.Account))
				continue;
			Entity entity = scene.FindEntityByUUID(pawn.Id);
			if (!entity)
				continue;
			glm::vec3 position = entity.GetComponent<TransformComponent>().Translation;
			position.x += 3.0f;
			physics->Teleport(pawn.Id, position);
			state.Teleported.insert(pawn.Account);
			GE_CORE_INFO("Predict teleport tick={0}", tick);
		}
	}

	void AfterScripts(Scene& scene)
	{
		TestState& state = State();
		if (scene.GetRole() != SceneRole::Server || state.Current != Mode::Move || state.Mover == UUID{ 0 })
			return;
		// Before the step, so the kinematic body copies this pose, not the previous step's.
		WriteMover(scene, state.Mover, ServerTick() + 1);
	}

	void AfterServerStep(Scene& scene, uint32_t tick)
	{
		TestState& state = State();
		if (!IsPredict(state.Current))
			return;
		const auto now = std::chrono::steady_clock::now();
		if (now < state.NextServerLog)
			return;
		state.NextServerLog = now + std::chrono::seconds(1);
		const InputStats inputs = GetInputStats();
		for (const PawnInfo& pawn : Pawns())
		{
			Entity entity = scene.FindEntityByUUID(pawn.Id);
			if (!entity)
				continue;
			const glm::vec3 position = entity.GetComponent<TransformComponent>().Translation;
			GE_CORE_INFO(
				"Predict server tick={0} x={1:.3f} y={2:.3f} z={3:.3f} repeats={4} walkRepeats={5}",
				tick, position.x, position.y, position.z, inputs.Repeats, inputs.RepeatsSinceMove);
		}
	}

	void AfterStandaloneStep(Scene& scene, uint32_t tick)
	{
		TestState& state = State();
		if (state.Current != Mode::Offline)
			return;
		if (tick == 120)
		{
			Entity walker = scene.FindEntityByUUID(state.Mover);
			if (walker)
			{
				const glm::vec3 position = walker.GetComponent<TransformComponent>().Translation;
				GE_CORE_INFO("Predict offline tick={0} x={1:.3f} y={2:.3f} z={3:.3f}",
					tick, position.x, position.y, position.z);
			}
		}
		if (tick >= 180 && !state.Done)
		{
			state.Done = true;
			Application::Get().Close();
		}
	}

	bool Active()
	{
		return State().Current != Mode::None;
	}

	bool DrivesInput()
	{
		const Mode mode = State().Current;
		return IsPredict(mode) || mode == Mode::Offline;
	}

	void Stick(PlayerCommand& cmd, uint32_t settleFrom)
	{
		const Mode mode = State().Current;
		// The offline walk starts at tick 60. A connected walk starts when prediction has
		// settled, a second after it began.
		const uint32_t from = mode == Mode::Offline ? 60u : settleFrom;
		if (from == 0 || cmd.Tick < from)
			return;
		if (mode == Mode::PredictLoss)
			cmd.MoveZ = ((cmd.Tick - from) / 30) % 2 == 0 ? 1.0f : -1.0f;
		else if (mode == Mode::PredictWall)
		{
			cmd.MoveX = 1.0f;
			cmd.MoveZ = 1.0f;
		}
		else
			cmd.MoveZ = 1.0f;
	}

	void AfterPredict(const PredictProgress& progress)
	{
		TestState& state = State();
		if (state.Done || progress.SettleFrom == 0 || !IsPredict(state.Current)
			|| progress.SimTick < progress.SettleFrom + PredictSpan(state.Current))
			return;
		state.Done = true;
		GE_CORE_INFO("Predict done corrections={0} maxMm={1:.0f} replay={2}",
			progress.Corrections, progress.MaxCorrectMm, progress.MaxReplay);
		Application::Get().Close();
	}

	void OnClientSpawn(Scene& scene, Entity root, const std::string& prefab)
	{
		(void)scene;
		// The client is not told the server's mode; NetPair only exists for `prefab`.
		if (prefab.find("NetPair") != std::string::npos)
			LogPrefab("client", root, State().ClientPrefabLogged);
	}

}

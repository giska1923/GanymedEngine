#include "gepch.h"
#include "World.h"

#include "GanymedE/Assets/AssetPaths.h"
#include "GanymedE/Core/Log.h"
#include "GanymedE/ECS/Singleton.h"
#include "GanymedE/Net/BitStream.h"
#include "GanymedE/Net/Connection.h"
#include "GanymedE/Net/Net.h"
#include "GanymedE/Net/Quantize.h"
#include "GanymedE/Physics/PhysicsScene.h"
#include "GanymedE/Scene/Components.h"
#include "GanymedE/Scene/Entity.h"
#include "GanymedE/Scene/PrefabSerializer.h"
#include "GanymedE/Scene/Scene.h"
#include "GanymedE/Scene/SceneRole.h"
#include "GanymedE/Scene/SceneSingletons.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace GanymedE::Net {

	namespace {

		constexpr int kBudget = kMaxDatagram - kHeaderBytes;
		constexpr uint8_t kSpawn = 1;
		constexpr uint8_t kDestroy = 2;
		constexpr uint8_t kUpdate = 3;
		constexpr float kDelaySeconds = 0.100f;
		constexpr uint32_t kSnapshotTicks = 2;
		constexpr uint32_t kDestroyTick = 60 * 18;

		static_assert(kPosSpanMm * 2 < (1 << kPosBits), "biased millimetres do not fit");

		enum class Test { None, Move, Spawn, Prefab, Rest };

		struct RepState
		{
			uint16_t NetId = 0;
			uint64_t Uuid = 0;
			std::string Prefab;
			uint8_t Op = kSpawn;
			bool HasPos = false;
			int Xmm = 0;
			int Ymm = 0;
			int Zmm = 0;
			bool HasYaw = false;
			uint16_t Yaw = 0;
			bool HasEuler = false;
			uint16_t Ex = 0;
			uint16_t Ey = 0;
			uint16_t Ez = 0;
			bool HasVel = false;
			int16_t Vx = 0;
			int16_t Vy = 0;
			int16_t Vz = 0;
			bool HasGround = false;
			uint8_t Ground = 0;
			bool HasAim = false;
			uint16_t AimPitch = 0;
			uint16_t AimYaw = 0;
		};

		struct Sample
		{
			uint32_t Tick = 0;
			bool HasPos = false;
			int Xmm = 0;
			int Ymm = 0;
			int Zmm = 0;
			bool HasRot = false;
			float Pitch = 0.0f;
			float Yaw = 0.0f;
			float Roll = 0.0f;
		};

		struct Ghost
		{
			UUID Id{ 0 };
			Sample Latest;
			std::vector<Sample> Buffer;
			glm::vec3 LastDrawn{ 0.0f };
			bool Drawn = false;
			float LastTarget = 0.0f;
			bool HaveTarget = false;
			uint32_t LoggedTick = 0;
		};

		struct Ring
		{
			struct Snap
			{
				uint16_t Seq = 0;
				std::vector<RepState> State;
			};

			std::vector<Snap> Snaps;
			std::vector<std::pair<std::chrono::steady_clock::time_point, int>> Bytes;
			int Last = 0;
			int Min = 0x7fffffff;
		};

		struct World
		{
			Test Mode = Test::None;
			uint32_t Tick = 0;
			uint32_t LastSnapTick = 0;
			uint32_t LastPoseTick = 0;
			uint16_t NextId = 1;
			UUID Mover{ 0 };
			UUID PrefabRoot{ 0 };
			std::vector<UUID> Spawned;
			bool SpawnGone = false;
			bool PrefabLogged = false;
			bool ClientPrefabLogged = false;
			std::unordered_map<uint64_t, std::string> PrefabOf;
			std::unordered_map<std::string, Ring> Rings;
			std::chrono::steady_clock::time_point NextServerLog{};

			std::unordered_map<uint16_t, Ghost> Ghosts;
			uint32_t LatestTick = 0;
			bool HaveLatest = false;
			std::chrono::steady_clock::time_point LatestAt{};
			float RttMs = 0.0f;
			int LastSnapBytes = 0;
			float SinceFirst = 0.0f;
			bool Warmed = false;
			int Frames = 0;
			int DryFrames = 0;
			float MaxRatio = 0.0f;
			std::chrono::steady_clock::time_point NextClientLog{};
		};

		World& State()
		{
			static World world;
			return world;
		}

		int SeqDiff(uint16_t a, uint16_t b)
		{
			return static_cast<int16_t>(a - b);
		}

		std::string Hex(uint64_t value)
		{
			char buffer[32];
			std::snprintf(buffer, sizeof(buffer), "%016llx", static_cast<unsigned long long>(value));
			return buffer;
		}

		bool StreamOk(const BitRead& stream) { return stream.Ok(); }
		template<typename Stream>
		bool StreamOk(const Stream&) { return true; }

		template<typename Stream>
		void BoolBit(Stream& stream, bool& value)
		{
			uint32_t bit = value ? 1u : 0u;
			Bits(stream, bit, 1);
			value = bit != 0;
		}

		template<typename Stream>
		void PathBits(Stream& stream, std::string& text)
		{
			uint32_t length = static_cast<uint32_t>(std::min<size_t>(text.size(), 80));
			Bits(stream, length, 7);
			if constexpr (Stream::Reading)
				text.assign(length, '\0');
			for (uint32_t i = 0; i < length; i++)
			{
				uint32_t ch = static_cast<uint8_t>(text[i]);
				Bits(stream, ch, 8);
				if constexpr (Stream::Reading)
					text[i] = static_cast<char>(ch);
			}
		}

		template<typename Stream>
		void HeaderBits(Stream& stream, uint32_t& tick, uint32_t& baseline, uint32_t& count)
		{
			Bits(stream, tick, 32);
			Bits(stream, baseline, 16);
			Bits(stream, count, 16);
		}

		template<typename Stream>
		bool RecordBits(Stream& stream, RepState& state)
		{
			uint32_t id = state.NetId;
			uint32_t op = state.Op;
			Bits(stream, id, 16);
			Bits(stream, op, 2);
			state.NetId = static_cast<uint16_t>(id);
			state.Op = static_cast<uint8_t>(op);
			if (!StreamOk(stream))
				return false;
			if (state.Op == kSpawn)
			{
				uint32_t lo = static_cast<uint32_t>(state.Uuid);
				uint32_t hi = static_cast<uint32_t>(state.Uuid >> 32);
				Bits(stream, lo, 32);
				Bits(stream, hi, 32);
				state.Uuid = static_cast<uint64_t>(lo) | (static_cast<uint64_t>(hi) << 32);
				PathBits(stream, state.Prefab);
			}
			else if (state.Op != kDestroy && state.Op != kUpdate)
				return false;
			if (state.Op == kDestroy)
				return StreamOk(stream);

			uint32_t mask = 0;
			if constexpr (!Stream::Reading)
			{
				if (state.HasPos) mask |= 1u;
				if (state.HasYaw) mask |= 2u;
				if (state.HasEuler) mask |= 4u;
				if (state.HasVel) mask |= 8u;
				if (state.HasGround) mask |= 16u;
				if (state.HasAim) mask |= 32u;
			}
			Bits(stream, mask, 6);
			if constexpr (Stream::Reading)
			{
				state.HasPos = (mask & 1u) != 0;
				state.HasYaw = (mask & 2u) != 0;
				state.HasEuler = (mask & 4u) != 0;
				state.HasVel = (mask & 8u) != 0;
				state.HasGround = (mask & 16u) != 0;
				state.HasAim = (mask & 32u) != 0;
			}

			if (state.HasPos)
			{
				uint32_t x = BiasMillimetres(state.Xmm);
				uint32_t y = BiasMillimetres(state.Ymm);
				uint32_t z = BiasMillimetres(state.Zmm);
				Bits(stream, x, kPosBits);
				Bits(stream, y, kPosBits);
				Bits(stream, z, kPosBits);
				if constexpr (Stream::Reading)
				{
					state.Xmm = UnbiasMillimetres(x);
					state.Ymm = UnbiasMillimetres(y);
					state.Zmm = UnbiasMillimetres(z);
				}
			}
			if (state.HasYaw)
			{
				uint32_t yaw = state.Yaw;
				Bits(stream, yaw, 16);
				state.Yaw = static_cast<uint16_t>(yaw);
			}
			if (state.HasEuler)
			{
				uint32_t ex = state.Ex;
				uint32_t ey = state.Ey;
				uint32_t ez = state.Ez;
				Bits(stream, ex, 16);
				Bits(stream, ey, 16);
				Bits(stream, ez, 16);
				state.Ex = static_cast<uint16_t>(ex);
				state.Ey = static_cast<uint16_t>(ey);
				state.Ez = static_cast<uint16_t>(ez);
			}
			if (state.HasVel)
			{
				uint32_t vx = static_cast<uint16_t>(state.Vx);
				uint32_t vy = static_cast<uint16_t>(state.Vy);
				uint32_t vz = static_cast<uint16_t>(state.Vz);
				Bits(stream, vx, 16);
				Bits(stream, vy, 16);
				Bits(stream, vz, 16);
				state.Vx = static_cast<int16_t>(static_cast<uint16_t>(vx));
				state.Vy = static_cast<int16_t>(static_cast<uint16_t>(vy));
				state.Vz = static_cast<int16_t>(static_cast<uint16_t>(vz));
			}
			if (state.HasGround)
			{
				uint32_t ground = state.Ground;
				Bits(stream, ground, 2);
				state.Ground = static_cast<uint8_t>(ground);
			}
			if (state.HasAim)
			{
				uint32_t pitch = state.AimPitch;
				uint32_t yaw = state.AimYaw;
				Bits(stream, pitch, 16);
				Bits(stream, yaw, 16);
				state.AimPitch = static_cast<uint16_t>(pitch);
				state.AimYaw = static_cast<uint16_t>(yaw);
			}
			return StreamOk(stream);
		}

		float FixedDt(Scene& scene)
		{
			ECS::SingletonAccessView<PhysicsSettings> settingsView{ scene };
			if (!settingsView.Has())
				return 1.0f / 60.0f;
			const PhysicsSettings& settings = *settingsView.Get();
			if (settings.FixedTimestep <= 0.0f)
				return 1.0f / 60.0f;
			return settings.FixedTimestep;
		}

		int RegistryCount(Scene& scene)
		{
			return static_cast<int>(scene.Reg().view<TagComponent>().size());
		}

		const RepState* FindId(const std::vector<RepState>& list, uint16_t id)
		{
			for (const RepState& state : list)
			{
				if (state.NetId == id)
					return &state;
			}
			return nullptr;
		}

		RepState* FindId(std::vector<RepState>& list, uint16_t id)
		{
			for (RepState& state : list)
			{
				if (state.NetId == id)
					return &state;
			}
			return nullptr;
		}

		void Overlay(RepState& dst, const RepState& src)
		{
			if (src.Op == kSpawn)
			{
				dst = src;
				dst.Op = kSpawn;
				return;
			}
			if (src.HasPos)
			{
				dst.HasPos = true;
				dst.Xmm = src.Xmm;
				dst.Ymm = src.Ymm;
				dst.Zmm = src.Zmm;
			}
			if (src.HasYaw)
			{
				dst.HasYaw = true;
				dst.HasEuler = false;
				dst.Yaw = src.Yaw;
			}
			if (src.HasEuler)
			{
				dst.HasEuler = true;
				dst.HasYaw = false;
				dst.Ex = src.Ex;
				dst.Ey = src.Ey;
				dst.Ez = src.Ez;
			}
			if (src.HasVel)
			{
				dst.HasVel = true;
				dst.Vx = src.Vx;
				dst.Vy = src.Vy;
				dst.Vz = src.Vz;
			}
			if (src.HasGround)
			{
				dst.HasGround = true;
				dst.Ground = src.Ground;
			}
			if (src.HasAim)
			{
				dst.HasAim = true;
				dst.AimPitch = src.AimPitch;
				dst.AimYaw = src.AimYaw;
			}
		}

		std::vector<RepState> Gather(Scene& scene, PhysicsScene* physics)
		{
			World& world = State();
			std::vector<RepState> list;
			auto view = scene.Reg().view<ReplicatedComponent, TransformComponent>();
			for (auto handle : view)
			{
				Entity entity{ handle, &scene };
				auto& replicated = entity.GetComponent<ReplicatedComponent>();
				if (replicated.NetId == 0)
				{
					replicated.NetId = world.NextId++;
					if (world.NextId == 0)
						world.NextId = 1;
				}

				RepState state;
				state.NetId = replicated.NetId;
				state.Uuid = static_cast<uint64_t>(entity.GetUUID());
				state.Op = kSpawn;
				auto prefab = world.PrefabOf.find(state.Uuid);
				if (prefab != world.PrefabOf.end())
					state.Prefab = prefab->second;

				const auto& transform = entity.GetComponent<TransformComponent>();
				state.HasPos = true;
				state.Xmm = QuantizeMeters(transform.Translation.x);
				state.Ymm = QuantizeMeters(transform.Translation.y);
				state.Zmm = QuantizeMeters(transform.Translation.z);

				const bool character = entity.HasComponent<CharacterControllerComponent>();
				if (character)
				{
					state.HasYaw = true;
					state.Yaw = QuantizeYaw(transform.Rotation.y);
					if (physics)
					{
						const glm::vec3 velocity = physics->GetLinearVelocity(entity.GetUUID());
						state.HasVel = true;
						state.Vx = QuantizeSpeed(velocity.x);
						state.Vy = QuantizeSpeed(velocity.y);
						state.Vz = QuantizeSpeed(velocity.z);
						const int ground = physics->CharacterGround(entity.GetUUID());
						if (ground >= 0)
						{
							state.HasGround = true;
							state.Ground = static_cast<uint8_t>(ground & 3);
						}
					}
				}
				else
				{
					state.HasEuler = true;
					state.Ex = QuantizeSignedAngle(transform.Rotation.x);
					state.Ey = QuantizeSignedAngle(transform.Rotation.y);
					state.Ez = QuantizeSignedAngle(transform.Rotation.z);
				}

				if (entity.HasComponent<AimOffsetComponent>())
				{
					const auto& aim = entity.GetComponent<AimOffsetComponent>();
					state.HasAim = true;
					state.AimPitch = QuantizeSignedAngle(aim.Pitch);
					state.AimYaw = QuantizeSignedAngle(aim.Yaw);
				}
				list.push_back(std::move(state));
			}

			std::sort(list.begin(), list.end(), [](const RepState& a, const RepState& b)
			{
				return a.NetId < b.NetId;
			});
			return list;
		}

		std::vector<RepState> Diff(const std::vector<RepState>& baseline, const std::vector<RepState>& current)
		{
			std::vector<RepState> records;
			for (const RepState& entity : current)
			{
				const RepState* previous = FindId(baseline, entity.NetId);
				if (!previous)
				{
					records.push_back(entity);
					continue;
				}

				RepState update;
				update.Op = kUpdate;
				update.NetId = entity.NetId;
				bool changed = false;
				if (!previous->HasPos || previous->Xmm != entity.Xmm || previous->Ymm != entity.Ymm
					|| previous->Zmm != entity.Zmm)
				{
					update.HasPos = true;
					update.Xmm = entity.Xmm;
					update.Ymm = entity.Ymm;
					update.Zmm = entity.Zmm;
					changed = true;
				}
				if (entity.HasYaw && (!previous->HasYaw || previous->Yaw != entity.Yaw))
				{
					update.HasYaw = true;
					update.Yaw = entity.Yaw;
					changed = true;
				}
				if (entity.HasEuler && (!previous->HasEuler || previous->Ex != entity.Ex
					|| previous->Ey != entity.Ey || previous->Ez != entity.Ez))
				{
					update.HasEuler = true;
					update.Ex = entity.Ex;
					update.Ey = entity.Ey;
					update.Ez = entity.Ez;
					changed = true;
				}
				if (entity.HasVel && (!previous->HasVel || previous->Vx != entity.Vx
					|| previous->Vy != entity.Vy || previous->Vz != entity.Vz))
				{
					update.HasVel = true;
					update.Vx = entity.Vx;
					update.Vy = entity.Vy;
					update.Vz = entity.Vz;
					changed = true;
				}
				if (entity.HasGround && (!previous->HasGround || previous->Ground != entity.Ground))
				{
					update.HasGround = true;
					update.Ground = entity.Ground;
					changed = true;
				}
				if (entity.HasAim && (!previous->HasAim || previous->AimPitch != entity.AimPitch
					|| previous->AimYaw != entity.AimYaw))
				{
					update.HasAim = true;
					update.AimPitch = entity.AimPitch;
					update.AimYaw = entity.AimYaw;
					changed = true;
				}
				if (changed)
					records.push_back(std::move(update));
			}

			for (const RepState& entity : baseline)
			{
				if (!FindId(current, entity.NetId))
				{
					RepState destroyed;
					destroyed.Op = kDestroy;
					destroyed.NetId = entity.NetId;
					records.push_back(destroyed);
				}
			}
			return records;
		}

		int MeasurePrefix(const std::vector<RepState>& records, int count)
		{
			BitMeasure measure;
			uint32_t tick = 0;
			uint32_t baseline = 0;
			uint32_t n = static_cast<uint32_t>(count);
			HeaderBits(measure, tick, baseline, n);
			for (int i = 0; i < count; i++)
			{
				RepState copy = records[static_cast<size_t>(i)];
				if (!RecordBits(measure, copy))
					return kBudget + 1;
			}
			return measure.Bytes();
		}

		int FitCount(const std::vector<RepState>& records)
		{
			int lo = 0;
			int hi = static_cast<int>(records.size());
			while (lo < hi)
			{
				const int mid = (lo + hi + 1) / 2;
				if (MeasurePrefix(records, mid) <= kBudget)
					lo = mid;
				else
					hi = mid - 1;
			}
			return lo;
		}

		std::vector<uint8_t> WritePacket(uint32_t tick, uint16_t baseline, const std::vector<RepState>& records, int count)
		{
			std::vector<uint8_t> out;
			BitWrite writer(out);
			uint32_t tickBits = tick;
			uint32_t baselineBits = baseline;
			uint32_t countBits = static_cast<uint32_t>(count);
			HeaderBits(writer, tickBits, baselineBits, countBits);
			for (int i = 0; i < count; i++)
			{
				RepState copy = records[static_cast<size_t>(i)];
				RecordBits(writer, copy);
			}
			return out;
		}

		std::vector<RepState> StampAfter(std::vector<RepState> baseline, const std::vector<RepState>& records, int count)
		{
			for (int i = 0; i < count; i++)
			{
				const RepState& record = records[static_cast<size_t>(i)];
				if (record.Op == kDestroy)
				{
					baseline.erase(std::remove_if(baseline.begin(), baseline.end(),
						[&](const RepState& state) { return state.NetId == record.NetId; }), baseline.end());
					continue;
				}
				RepState* slot = FindId(baseline, record.NetId);
				if (!slot)
				{
					baseline.push_back(record);
					baseline.back().Op = kSpawn;
					continue;
				}
				Overlay(*slot, record);
			}
			return baseline;
		}

		bool Decode(const uint8_t* data, int length, uint32_t& tick, std::vector<RepState>& records)
		{
			BitRead reader(data, length);
			uint32_t baseline = 0;
			uint32_t count = 0;
			HeaderBits(reader, tick, baseline, count);
			if (!reader.Ok() || count > 512)
				return false;
			records.clear();
			records.reserve(count);
			for (uint32_t i = 0; i < count; i++)
			{
				RepState record;
				if (!RecordBits(reader, record))
					return false;
				records.push_back(std::move(record));
			}
			return reader.Ok();
		}

		bool SafePrefabPath(const std::string& path)
		{
			if (path.empty() || path.size() > 80 || path.find("..") != std::string::npos)
				return false;
			for (unsigned char c : path)
			{
				const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
					|| (c >= '0' && c <= '9') || c == '/' || c == '_' || c == '-' || c == '.';
				if (!ok)
					return false;
			}
			return true;
		}

		void LogPrefab(const char* role, Scene& scene, Entity root, bool& logged)
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
			GE_CORE_INFO("Replicate prefab {0} root={1} child={2}",
				role, Hex(static_cast<uint64_t>(root.GetUUID())), Hex(child));
			logged = true;
		}

		void DestroyTree(Scene& scene, Entity root)
		{
			std::vector<Entity> subtree;
			std::unordered_set<UUID> visited;
			scene.CollectSubtree(root, subtree, visited);
			for (auto it = subtree.rbegin(); it != subtree.rend(); ++it)
			{
				if (*it)
					scene.DestroyEntity(*it);
			}
		}

		void PushSample(Ghost& ghost, const Sample& sample)
		{
			for (Sample& existing : ghost.Buffer)
			{
				if (existing.Tick == sample.Tick)
				{
					existing = sample;
					return;
				}
			}
			ghost.Buffer.push_back(sample);
			std::sort(ghost.Buffer.begin(), ghost.Buffer.end(), [](const Sample& a, const Sample& b)
			{
				return a.Tick < b.Tick;
			});
			while (ghost.Buffer.size() > 32)
				ghost.Buffer.erase(ghost.Buffer.begin());
		}

		void OverlaySample(Sample& sample, const RepState& record)
		{
			if (record.HasPos)
			{
				sample.HasPos = true;
				sample.Xmm = record.Xmm;
				sample.Ymm = record.Ymm;
				sample.Zmm = record.Zmm;
			}
			if (record.HasYaw)
			{
				sample.HasRot = true;
				sample.Pitch = 0.0f;
				sample.Yaw = UnquantizeYaw(record.Yaw);
				sample.Roll = 0.0f;
			}
			if (record.HasEuler)
			{
				sample.HasRot = true;
				sample.Pitch = UnquantizeSignedAngle(record.Ex);
				sample.Yaw = UnquantizeSignedAngle(record.Ey);
				sample.Roll = UnquantizeSignedAngle(record.Ez);
			}
		}

		bool ApplyPacket(Scene& scene, const uint8_t* data, int length, float rttMs,
			std::chrono::steady_clock::time_point at)
		{
			uint32_t tick = 0;
			std::vector<RepState> records;
			if (!Decode(data, length, tick, records))
				return false;

			World& world = State();
			world.LatestTick = tick;
			world.LatestAt = at;
			world.RttMs = rttMs;
			world.LastSnapBytes = length;
			world.HaveLatest = true;

			for (const RepState& record : records)
			{
				if (record.Op == kDestroy)
				{
					auto ghost = world.Ghosts.find(record.NetId);
					if (ghost == world.Ghosts.end())
						continue;
					Entity entity = scene.FindEntityByUUID(ghost->second.Id);
					if (entity)
						DestroyTree(scene, entity);
					world.Ghosts.erase(ghost);
					continue;
				}

				if (record.Op == kSpawn)
				{
					Entity entity = scene.FindEntityByUUID(UUID(record.Uuid));
					if (!entity && !record.Prefab.empty())
					{
						if (!SafePrefabPath(record.Prefab))
						{
							GE_CORE_WARN("Replicate refused a prefab path");
							continue;
						}
						PrefabSerializer::InstantiateOptions options;
						options.RootUUID = UUID(record.Uuid);
						entity = PrefabSerializer::Instantiate(GetAssetRoot() / record.Prefab, scene, UUID{ 0 }, options);
						if (entity && record.Prefab.find("NetPair") != std::string::npos)
							LogPrefab("client", scene, entity, world.ClientPrefabLogged);
					}
					if (!entity)
						entity = scene.CreateEntityWithUUID(UUID(record.Uuid), "replicated");
					if (!entity)
						continue;
					if (!entity.HasComponent<ReplicatedComponent>())
						entity.AddComponent<ReplicatedComponent>();
					entity.GetComponent<ReplicatedComponent>().NetId = record.NetId;

					Ghost& ghost = world.Ghosts[record.NetId];
					ghost.Id = entity.GetUUID();
					Sample sample;
					sample.Tick = tick;
					OverlaySample(sample, record);
					ghost.Latest = sample;
					PushSample(ghost, sample);
					continue;
				}

				auto ghost = world.Ghosts.find(record.NetId);
				if (ghost == world.Ghosts.end())
					continue;
				Sample sample = ghost->second.Latest;
				sample.Tick = tick;
				OverlaySample(sample, record);
				ghost->second.Latest = sample;
				PushSample(ghost->second, sample);
			}
			return true;
		}

		glm::vec3 SamplePos(const Sample& sample)
		{
			return {
				MillimetresToMeters(sample.Xmm),
				MillimetresToMeters(sample.Ymm),
				MillimetresToMeters(sample.Zmm)
			};
		}

		float Distance(const glm::vec3& a, const glm::vec3& b)
		{
			const glm::vec3 d = a - b;
			return std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
		}

		// Metres per second along the snapshot segment that contains this tick. The step
		// check compares a frame's drawn travel to this, times how far the render clock
		// moved — not to the newest samples times the wall-clock frame, which diverges
		// whenever several physics steps land in one frame.
		float SegmentSpeed(const std::vector<Sample>& buffer, float target, float fixedDt)
		{
			if (buffer.size() < 2 || fixedDt <= 0.0f)
				return 0.0f;

			size_t hi = 1;
			if (target >= static_cast<float>(buffer.back().Tick))
				hi = buffer.size() - 1;
			else if (target > static_cast<float>(buffer.front().Tick))
			{
				while (hi < buffer.size() && static_cast<float>(buffer[hi].Tick) < target)
					hi++;
			}

			const Sample& a = buffer[hi - 1];
			const Sample& b = buffer[hi];
			if (!a.HasPos || !b.HasPos)
				return 0.0f;
			float span = static_cast<float>(b.Tick - a.Tick);
			if (span < 1.0f)
				span = 1.0f;
			return Distance(SamplePos(a), SamplePos(b)) / (span * fixedDt);
		}

		float LerpAngle(float a, float b, float t)
		{
			constexpr float kPi = 3.14159265358979323846f;
			float delta = b - a;
			while (delta > kPi)
				delta -= 2.0f * kPi;
			while (delta < -kPi)
				delta += 2.0f * kPi;
			return a + delta * t;
		}

		void Evaluate(const Ghost& ghost, float target, float fixedDt, bool dry, uint32_t latestTick,
			glm::vec3& position, glm::vec3& euler)
		{
			const std::vector<Sample>& buffer = ghost.Buffer;
			position = ghost.LastDrawn;
			euler = { ghost.Latest.Pitch, ghost.Latest.Yaw, ghost.Latest.Roll };
			if (buffer.empty())
				return;

			auto poseOf = [&](const Sample& sample)
			{
				position = sample.HasPos ? SamplePos(sample) : position;
				if (sample.HasRot)
					euler = { sample.Pitch, sample.Yaw, sample.Roll };
			};

			if (target <= static_cast<float>(buffer.front().Tick))
			{
				poseOf(buffer.front());
				return;
			}
			if (target >= static_cast<float>(buffer.back().Tick))
			{
				const Sample& last = buffer.back();
				const bool fresh = latestTick >= last.Tick && latestTick - last.Tick <= kSnapshotTicks;
				if (dry && fresh && buffer.size() >= 2)
				{
					const Sample& before = buffer[buffer.size() - 2];
					float span = static_cast<float>(last.Tick - before.Tick);
					if (span < 1.0f)
						span = 1.0f;
					const float extra = std::min(target - static_cast<float>(last.Tick), static_cast<float>(kSnapshotTicks));
					const glm::vec3 from = SamplePos(before);
					const glm::vec3 to = SamplePos(last);
					position = to + (to - from) * (extra / span);
					euler = { last.Pitch, last.Yaw, last.Roll };
					return;
				}
				poseOf(last);
				return;
			}

			size_t hi = 1;
			while (hi < buffer.size() && static_cast<float>(buffer[hi].Tick) < target)
				hi++;
			const Sample& a = buffer[hi - 1];
			const Sample& b = buffer[hi];
			const float span = static_cast<float>(b.Tick - a.Tick);
			const float u = span > 0.0f ? (target - static_cast<float>(a.Tick)) / span : 0.0f;
			position = glm::mix(SamplePos(a), SamplePos(b), u);
			euler = {
				LerpAngle(a.Pitch, b.Pitch, u),
				LerpAngle(a.Yaw, b.Yaw, u),
				LerpAngle(a.Roll, b.Roll, u)
			};
			(void)fixedDt;
		}

		int ByteRate(const Ring& ring, std::chrono::steady_clock::time_point now)
		{
			const auto cutoff = now - std::chrono::seconds(1);
			int total = 0;
			for (const auto& sample : ring.Bytes)
			{
				if (sample.first >= cutoff)
					total += sample.second;
			}
			return total;
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

		UUID SpawnPrefab(Scene& scene, const char* relative, World& world)
		{
			UUID rootId;
			PrefabSerializer::InstantiateOptions options;
			options.RootUUID = rootId;
			Entity root = PrefabSerializer::Instantiate(GetAssetRoot() / relative, scene, UUID{ 0 }, options);
			if (!root)
			{
				GE_CORE_ERROR("Replicate test could not spawn {0}", relative);
				return UUID{ 0 };
			}
			if (!root.HasComponent<ReplicatedComponent>())
				root.AddComponent<ReplicatedComponent>();
			world.PrefabOf[static_cast<uint64_t>(root.GetUUID())] = relative;
			return root.GetUUID();
		}

	}

	void PresentWorld(Scene& scene, float frameDt)
	{
		if (scene.GetRole() != SceneRole::Client)
			return;

		World& world = State();
		const auto packets = DrainSnapshots();
		for (const SnapshotDatagram& packet : packets)
			ApplyPacket(scene, packet.Bytes.data(), static_cast<int>(packet.Bytes.size()), packet.RttMs, packet.At);

		if (!world.HaveLatest)
			return;

		const float fixedDt = FixedDt(scene);
		const float elapsed = std::chrono::duration<float>(std::chrono::steady_clock::now() - world.LatestAt).count();
		const float estimated = static_cast<float>(world.LatestTick) + elapsed / fixedDt;
		float renderTick = estimated - (kDelaySeconds / fixedDt);
		const bool dry = renderTick > static_cast<float>(world.LatestTick);
		if (dry)
		{
			const float cap = static_cast<float>(world.LatestTick) + static_cast<float>(kSnapshotTicks);
			if (renderTick > cap)
				renderTick = cap;
		}

		world.SinceFirst += frameDt;
		if (world.SinceFirst >= 1.0f)
			world.Warmed = true;
		if (world.Warmed)
		{
			world.Frames++;
			if (dry)
				world.DryFrames++;
		}

		int depth = 0;
		for (auto& entry : world.Ghosts)
		{
			Ghost& ghost = entry.second;
			Entity entity = scene.FindEntityByUUID(ghost.Id);
			if (!entity || !entity.HasComponent<TransformComponent>())
				continue;
			if (entity.HasComponent<RigidBodyComponent>()
				&& entity.GetComponent<RigidBodyComponent>().Type == RigidBodyType::Dynamic)
			{
				entity.GetComponent<RigidBodyComponent>().Type = RigidBodyType::Kinematic;
			}

			glm::vec3 position{ 0.0f };
			glm::vec3 euler{ 0.0f };
			Evaluate(ghost, renderTick, fixedDt, dry, world.LatestTick, position, euler);

			if (ghost.Drawn && ghost.HaveTarget && world.Warmed)
			{
				const float ticks = std::fabs(renderTick - ghost.LastTarget);
				const float expect = SegmentSpeed(ghost.Buffer, renderTick, fixedDt) * ticks * fixedDt;
				const float step = Distance(position, ghost.LastDrawn);
				const float ratio = expect < 1.0e-4f
					? (step < 1.0e-3f ? 0.0f : step / 0.001f)
					: step / expect;
				if (ratio > world.MaxRatio)
					world.MaxRatio = ratio;
			}
			ghost.LastDrawn = position;
			ghost.LastTarget = renderTick;
			ghost.HaveTarget = true;
			ghost.Drawn = true;

			auto& transform = entity.GetComponent<TransformComponent>();
			const bool moved = Distance(transform.Translation, position) > 1.0e-5f
				|| Distance(transform.Rotation, euler) > 1.0e-5f;
			transform.Translation = position;
			transform.Rotation = euler;
			if (moved)
				scene.MarkChanged<TransformComponent>(entity);

			if (depth < static_cast<int>(ghost.Buffer.size()))
				depth = static_cast<int>(ghost.Buffer.size());

			if (world.Ghosts.size() <= 4)
			{
				for (const Sample& sample : ghost.Buffer)
				{
					if (!sample.HasPos || sample.Tick % 60 != 0 || sample.Tick <= ghost.LoggedTick)
						continue;
					GE_CORE_INFO("Replicate pose client tick={0} x={1} y={2} z={3}",
						sample.Tick, sample.Xmm, sample.Ymm, sample.Zmm);
					ghost.LoggedTick = sample.Tick;
				}
			}
		}

		const auto now = std::chrono::steady_clock::now();
		if (now >= world.NextClientLog)
		{
			world.NextClientLog = now + std::chrono::seconds(1);
			const float dryPct = world.Frames > 0
				? 100.0f * static_cast<float>(world.DryFrames) / static_cast<float>(world.Frames)
				: 0.0f;
			GE_CORE_INFO(
				"Replicate client tick={0} est={1:.1f} lead={2:.0f} snap={3} depth={4} dry={5:.2f} step={6:.2f} entities={7} registry={8} ghosts={9} frames={10}",
				static_cast<uint32_t>(renderTick), estimated, world.RttMs * 0.5f, world.LastSnapBytes, depth,
				dryPct, world.MaxRatio, static_cast<int>(scene.Reg().view<ReplicatedComponent>().size()),
				RegistryCount(scene), static_cast<int>(world.Ghosts.size()), world.Frames);
		}
	}

	void NoteSimulatedStep(Scene& scene)
	{
		if (scene.GetRole() != SceneRole::Server)
			return;
		State().Tick++;
	}

	void DriveWorldTest(Scene& scene)
	{
		World& world = State();
		if (scene.GetRole() != SceneRole::Server || world.Mode != Test::Move || world.Mover == UUID{ 0 })
			return;
		WriteMover(scene, world.Mover, world.Tick + 1);
	}

	void CaptureWorld(Scene& scene, PhysicsScene* physics)
	{
		if (scene.GetRole() != SceneRole::Server)
			return;

		World& world = State();
		if (world.Tick == 0 || (world.Tick % kSnapshotTicks) != 0 || world.Tick == world.LastSnapTick)
			return;
		world.LastSnapTick = world.Tick;

		const std::vector<RepState> current = Gather(scene, physics);
		if (world.Tick % 60 == 0 && world.Tick != world.LastPoseTick && current.size() <= 4)
		{
			world.LastPoseTick = world.Tick;
			for (const RepState& entity : current)
			{
				if (!entity.HasPos)
					continue;
				GE_CORE_INFO("Replicate pose server tick={0} x={1} y={2} z={3}",
					world.Tick, entity.Xmm, entity.Ymm, entity.Zmm);
			}
		}

		const std::vector<std::string> accounts = ServerAccounts();
		std::unordered_set<std::string> live(accounts.begin(), accounts.end());
		for (auto it = world.Rings.begin(); it != world.Rings.end(); )
		{
			if (live.find(it->first) == live.end())
				it = world.Rings.erase(it);
			else
				++it;
		}

		const auto now = std::chrono::steady_clock::now();
		const bool logNow = now >= world.NextServerLog;
		if (logNow)
			world.NextServerLog = now + std::chrono::seconds(1);

		for (const std::string& account : accounts)
		{
			Ring& ring = world.Rings[account];
			std::vector<RepState> baseline;
			uint16_t baselineSeq = 0;
			for (const Ring::Snap& snap : ring.Snaps)
			{
				if (!SnapshotAcked(account, snap.Seq))
					continue;
				if (baselineSeq != 0 && SeqDiff(snap.Seq, baselineSeq) <= 0)
					continue;
				baselineSeq = snap.Seq;
				baseline = snap.State;
			}

			std::vector<RepState> records = Diff(baseline, current);
			// A spawn the client applied whose ack we missed is not in the acked baseline.
			// If the entity is already gone, that diff will not mention it, and the client
			// would keep it. Anything we have ever put in a sent stamp gets a destroy until
			// some later acked snapshot no longer has it.
			if (!ring.Snaps.empty())
			{
				for (const RepState& sent : ring.Snaps.back().State)
				{
					if (FindId(current, sent.NetId) || FindId(records, sent.NetId))
						continue;
					RepState destroyed;
					destroyed.Op = kDestroy;
					destroyed.NetId = sent.NetId;
					records.push_back(destroyed);
				}
			}
			std::stable_partition(records.begin(), records.end(), [](const RepState& record)
			{
				return record.Op == kDestroy;
			});
			const int fit = FitCount(records);
			if (!records.empty() && fit == 0)
				GE_CORE_WARN("Replicate record does not fit in one datagram");
			const std::vector<RepState> stamp = StampAfter(baseline, records, fit);
			const std::vector<uint8_t> bytes = WritePacket(world.Tick, baselineSeq, records, fit);
			const uint16_t sequence = SendSnapshot(account, bytes.data(), static_cast<int>(bytes.size()));
			if (sequence == 0)
				continue;

			Ring::Snap sent;
			sent.Seq = sequence;
			sent.State = stamp;
			ring.Snaps.push_back(std::move(sent));
			if (ring.Snaps.size() > 32)
				ring.Snaps.erase(ring.Snaps.begin());
			ring.Last = static_cast<int>(bytes.size());
			if (ring.Last < ring.Min)
				ring.Min = ring.Last;
			ring.Bytes.emplace_back(now, ring.Last);
			const auto cutoff = now - std::chrono::seconds(1);
			while (!ring.Bytes.empty() && ring.Bytes.front().first < cutoff)
				ring.Bytes.erase(ring.Bytes.begin());

			if (logNow)
			{
				GE_CORE_INFO(
					"Replicate server peer={0} tick={1} entities={2} registry={3} snap={4} min={5} Bps={6}",
					account, world.Tick, static_cast<int>(current.size()), RegistryCount(scene),
					ring.Last, ring.Min == 0x7fffffff ? 0 : ring.Min, ByteRate(ring, now));
			}
		}
	}

	void AdvanceWorldTest(Scene& scene)
	{
		World& world = State();
		if (scene.GetRole() != SceneRole::Server || world.Mode != Test::Spawn || world.SpawnGone)
			return;
		if (world.Tick < kDestroyTick)
			return;
		for (UUID id : world.Spawned)
		{
			Entity entity = scene.FindEntityByUUID(id);
			if (entity)
				scene.DestroyEntity(entity);
		}
		world.Spawned.clear();
		world.SpawnGone = true;
		GE_CORE_INFO("Replicate phase destroy");
	}

	void StartWorldTest(Scene& scene, const std::string& test)
	{
		World& world = State();
		if (test == "move")
			world.Mode = Test::Move;
		else if (test == "spawn")
			world.Mode = Test::Spawn;
		else if (test == "prefab")
			world.Mode = Test::Prefab;
		else if (test == "rest")
			world.Mode = Test::Rest;
		else
		{
			GE_CORE_ERROR("Replicate test '{0}' is not move, spawn, prefab, or rest", test);
			return;
		}

		GE_CORE_INFO("Replicate test {0}", test);
		if (world.Mode == Test::Move)
		{
			world.Mover = SpawnPrefab(scene, "prefabs/NetMover.gprefab", world);
		}
		else if (world.Mode == Test::Rest)
		{
			Entity entity = scene.CreateEntity("net-rest");
			entity.AddComponent<ReplicatedComponent>();
			entity.GetComponent<TransformComponent>().Translation = { 2.0f, 1.0f, 0.0f };
			scene.MarkChanged<TransformComponent>(entity);
		}
		else if (world.Mode == Test::Spawn)
		{
			world.Spawned.reserve(500);
			for (int i = 0; i < 500; i++)
			{
				Entity entity = scene.CreateEntity("net-spawn");
				entity.AddComponent<ReplicatedComponent>();
				entity.GetComponent<TransformComponent>().Translation = { static_cast<float>(i) * 0.1f, 0.0f, 0.0f };
				world.Spawned.push_back(entity.GetUUID());
			}
			GE_CORE_INFO("Replicate phase spawn");
		}
		else if (world.Mode == Test::Prefab)
		{
			world.PrefabRoot = SpawnPrefab(scene, "prefabs/NetPair.gprefab", world);
			Entity root = scene.FindEntityByUUID(world.PrefabRoot);
			LogPrefab("server", scene, root, world.PrefabLogged);
		}
	}

}

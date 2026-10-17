#include "gepch.h"
#include "World.h"

#include "GanymedE/Assets/AssetManager.h"
#include "GanymedE/Assets/AssetPaths.h"
#include "GanymedE/Core/Input.h"
#include "GanymedE/Core/KeyCodes.h"
#include "GanymedE/Core/Log.h"
#include "GanymedE/Core/MouseButtonCodes.h"
#include "GanymedE/ECS/Singleton.h"
#include "GanymedE/Net/BitStream.h"
#include "GanymedE/Net/Command.h"
#include "GanymedE/Net/Connection.h"
#include "GanymedE/Net/Net.h"
#include "GanymedE/Net/NetTest.h"
#include "GanymedE/Net/Quantize.h"
#include "GanymedE/Physics/PhysicsScene.h"
#include "GanymedE/Scene/Components.h"
#include "GanymedE/Scene/Entity.h"
#include "GanymedE/Scene/PrefabSerializer.h"
#include "GanymedE/Scene/Scene.h"
#include "GanymedE/Scene/SceneRole.h"
#include "GanymedE/Scene/SceneSingletons.h"
#include "GanymedE/Scene/ScriptableEntity.h"
#include "GanymedE/Scripting/ScriptEngine.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
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
		constexpr int kLeadMarginTicks = 2;

		static_assert(kPosSpanMm * 2 < (1 << kPosBits), "biased millimetres do not fit");

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
			bool HasScript = false;
			std::vector<FieldValue> Script;
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
			bool HasVel = false;
			float Vx = 0.0f;
			float Vy = 0.0f;
			float Vz = 0.0f;
			bool HasGround = false;
			int Ground = 3;
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
				std::vector<uint16_t> Destroys;   // the destroy records this snapshot carried
				bool Confirmed = false;           // its ack has been seen, and Destroys applied
			};

			std::vector<Snap> Snaps;
			// Every net id a spawn record has gone out for, until a snapshot carrying its
			// destroy is acknowledged. The client may hold any of these, whatever the acked
			// baseline says, so each one that is gone gets a destroy in every snapshot until
			// one of those arrives.
			std::unordered_set<uint16_t> MayHave;
			std::vector<std::pair<std::chrono::steady_clock::time_point, int>> Bytes;
			int Last = 0;
			int Min = 0x7fffffff;
		};

		struct Pawn
		{
			std::string Account;
			UUID Id{ 0 };
			uint16_t NetId = 0;
			uint32_t AssignedTick = 0;
		};

		struct AccountInput
		{
			std::map<uint32_t, PlayerCommand> ByTick;
			PlayerCommand Last{};
			bool HaveLast = false;
			uint32_t Repeats = 0;
			uint32_t RepeatsAtWalk = 0;
			bool WalkMarked = false;
		};

		struct Predicted
		{
			PlayerCommand Command{};
			glm::vec3 Position{ 0.0f };
			glm::vec3 Velocity{ 0.0f };
			int Ground = 3;
			bool Valid = false;
		};

		// Kind 1 is a named event. Kind 2 is a shot. Reliable is not a wire bit: a reliable
		// event is its own message, and a shot is always unreliable.
		struct WireEvent
		{
			uint16_t NetId = 0;
			uint8_t Kind = 1;
			bool Reliable = false;
			std::string Name;
			std::vector<EventArg> Args;
			float Mx = 0.0f;
			float My = 0.0f;
			float Mz = 0.0f;
			float Dx = 0.0f;
			float Dy = 0.0f;
			float Dz = 0.0f;
			float Speed = 0.0f;
			uint32_t Tick = 0;
		};

		struct FieldWrite
		{
			UUID Id{ 0 };
			std::vector<FieldValue> Fields;
		};

		struct HeldEvent
		{
			WireEvent Event;
			int Waits = 0;
		};

		struct World
		{
			uint32_t Tick = 0;
			uint32_t LastSnapTick = 0;
			uint32_t LastPoseTick = 0;
			uint16_t NextId = 1;
			std::unordered_map<uint64_t, std::string> PrefabOf;
			std::unordered_map<std::string, Ring> Rings;
			std::chrono::steady_clock::time_point NextServerLog{};

			std::unordered_map<uint16_t, Ghost> Ghosts;
			uint32_t LatestTick = 0;
			bool HaveLatest = false;
			// The session LatestTick belongs to, and how many snapshots arrived no newer than it.
			uint64_t LatestSession = 0;
			uint64_t StaleSnapshots = 0;

			// The full state after each of the last 64 snapshots received, applied or not, keyed
			// by transport sequence: any of them can be the next snapshot's baseline. `Applied` is
			// the full state the scene was last brought to.
			struct ClientSnap
			{
				uint16_t Seq = 0;
				std::vector<RepState> State;
			};
			std::vector<ClientSnap> Received;
			std::vector<RepState> Applied;
			uint64_t NoBaseline = 0;
			std::unordered_set<uint16_t> RefusedPrefab;
			std::chrono::steady_clock::time_point LatestAt{};
			float RttMs = 0.0f;
			int LastSnapBytes = 0;
			float SinceFirst = 0.0f;
			bool Warmed = false;
			int Frames = 0;
			int DryFrames = 0;
			float MaxRatio = 0.0f;
			std::chrono::steady_clock::time_point NextClientLog{};

			std::vector<Pawn> Pawns;
			std::unordered_map<std::string, AccountInput> Inputs;

			uint16_t OwnedNetId = 0;
			UUID LocalId{ 0 };
			float Estimated = 0.0f;
			bool HaveEstimate = false;
			uint32_t SimTick = 0;
			bool HaveSim = false;
			// One second after prediction starts. Corrections before it are the settle, and the
			// counters below leave them out.
			uint32_t SettleFrom = 0;
			Predicted History[64]{};
			uint32_t LastReconciled = 0;
			bool HaveReconciled = false;
			int Corrections = 0;
			float MaxCorrectMm = 0.0f;
			int MaxReplay = 0;
			glm::vec3 VisualError{ 0.0f };
			float ViewYaw = 0.0f;
			float ViewPitch = 0.0f;
			bool LookNoted = false;
			bool LoggedInput = false;
			bool LoggedMoved = false;
			uint32_t OfflineTicks = 0;
			std::chrono::steady_clock::time_point NextPredictLog{};

			uint32_t ScriptTick = 0;
			std::vector<WireEvent> Unreliable;
			std::vector<FieldWrite> FieldWrites;
			std::vector<HeldEvent> Held;
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
		void HeaderBits(Stream& stream, uint32_t& tick, uint32_t& baseline, uint32_t& owned, uint32_t& count)
		{
			Bits(stream, tick, 32);
			Bits(stream, baseline, 16);
			Bits(stream, owned, 16);
			Bits(stream, count, 16);
		}

		template<typename Stream>
		void TextBits(Stream& stream, std::string& text, int lenBits, size_t maxLen)
		{
			uint32_t length = static_cast<uint32_t>(std::min(text.size(), maxLen));
			Bits(stream, length, lenBits);
			if constexpr (Stream::Reading)
				text.assign(length, '\0');
			for (uint32_t i = 0; i < length; i++)
			{
				uint32_t ch = 0;
				if constexpr (!Stream::Reading)
					ch = static_cast<uint8_t>(text[i]);
				Bits(stream, ch, 8);
				if constexpr (Stream::Reading)
					text[i] = static_cast<char>(ch);
			}
		}

		template<typename Stream>
		void FloatBits(Stream& stream, float& value)
		{
			uint32_t bits = 0;
			if constexpr (!Stream::Reading)
				std::memcpy(&bits, &value, sizeof(bits));
			Bits(stream, bits, 32);
			if constexpr (Stream::Reading)
				std::memcpy(&value, &bits, sizeof(bits));
		}

		template<typename Stream>
		bool FieldBlock(Stream& stream, std::vector<FieldValue>& fields)
		{
			uint32_t count = static_cast<uint32_t>(std::min<size_t>(fields.size(), 16));
			Bits(stream, count, 5);
			if constexpr (Stream::Reading)
				fields.resize(count);
			for (uint32_t i = 0; i < count; i++)
			{
				FieldValue& field = fields[i];
				uint32_t index = field.Index;
				uint32_t type = field.Type;
				Bits(stream, index, 4);
				Bits(stream, type, 2);
				field.Index = static_cast<uint8_t>(index);
				field.Type = static_cast<uint8_t>(type);
				if (field.Type == kFieldBool)
				{
					uint32_t bit = field.Number ? 1u : 0u;
					Bits(stream, bit, 1);
					field.Number = bit;
				}
				else if (field.Type == kFieldU8)
				{
					uint32_t number = field.Number & 255u;
					Bits(stream, number, 8);
					field.Number = number;
				}
				else if (field.Type == kFieldTick)
				{
					uint32_t number = field.Number;
					Bits(stream, number, 32);
					field.Number = number;
				}
				else if (field.Type == kFieldStr16)
					TextBits(stream, field.Text, 5, 16);
				else
					return false;
			}
			return StreamOk(stream);
		}

		template<typename Stream>
		bool ArgBits(Stream& stream, EventArg& arg)
		{
			TextBits(stream, arg.Key, 4, 12);
			uint32_t type = arg.Type;
			Bits(stream, type, 2);
			arg.Type = static_cast<uint8_t>(type);
			if (arg.Type == kArgBool)
			{
				uint32_t bit = arg.Number != 0.0f ? 1u : 0u;
				Bits(stream, bit, 1);
				arg.Number = bit ? 1.0f : 0.0f;
			}
			else if (arg.Type == kArgNumber)
				FloatBits(stream, arg.Number);
			else if (arg.Type == kArgString)
				TextBits(stream, arg.Text, 5, 16);
			else if (arg.Type == kArgVec3)
			{
				FloatBits(stream, arg.X);
				FloatBits(stream, arg.Y);
				FloatBits(stream, arg.Z);
			}
			else
				return false;
			return StreamOk(stream);
		}

		template<typename Stream>
		bool OneEventBits(Stream& stream, WireEvent& event)
		{
			uint32_t id = event.NetId;
			uint32_t kind = event.Kind;
			Bits(stream, id, 16);
			Bits(stream, kind, 2);
			event.NetId = static_cast<uint16_t>(id);
			event.Kind = static_cast<uint8_t>(kind);
			if (event.Kind == 2)
			{
				FloatBits(stream, event.Mx);
				FloatBits(stream, event.My);
				FloatBits(stream, event.Mz);
				FloatBits(stream, event.Dx);
				FloatBits(stream, event.Dy);
				FloatBits(stream, event.Dz);
				FloatBits(stream, event.Speed);
				uint32_t tick = event.Tick;
				Bits(stream, tick, 32);
				event.Tick = tick;
				return StreamOk(stream);
			}
			if (event.Kind != 1)
				return false;
			TextBits(stream, event.Name, 5, 16);
			uint32_t argc = static_cast<uint32_t>(std::min<size_t>(event.Args.size(), 4));
			Bits(stream, argc, 3);
			if constexpr (Stream::Reading)
				event.Args.resize(argc);
			for (uint32_t i = 0; i < argc; i++)
			{
				if (!ArgBits(stream, event.Args[i]))
					return false;
			}
			return StreamOk(stream);
		}

		template<typename Stream>
		bool TrailerBits(Stream& stream, std::vector<WireEvent>& events)
		{
			uint32_t count = static_cast<uint32_t>(std::min<size_t>(events.size(), 8));
			Bits(stream, count, 4);
			if constexpr (Stream::Reading)
				events.resize(count);
			for (uint32_t i = 0; i < count; i++)
			{
				if (!OneEventBits(stream, events[i]))
					return false;
			}
			return StreamOk(stream);
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
				if (state.HasScript) mask |= 64u;
			}
			Bits(stream, mask, 7);
			if constexpr (Stream::Reading)
			{
				state.HasPos = (mask & 1u) != 0;
				state.HasYaw = (mask & 2u) != 0;
				state.HasEuler = (mask & 4u) != 0;
				state.HasVel = (mask & 8u) != 0;
				state.HasGround = (mask & 16u) != 0;
				state.HasAim = (mask & 32u) != 0;
				state.HasScript = (mask & 64u) != 0;
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
			if (state.HasScript && !FieldBlock(stream, state.Script))
				return false;
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
			if (src.HasScript)
			{
				dst.HasScript = true;
				for (const FieldValue& field : src.Script)
				{
					bool found = false;
					for (FieldValue& have : dst.Script)
					{
						if (have.Index != field.Index)
							continue;
						have = field;
						found = true;
						break;
					}
					if (!found)
						dst.Script.push_back(field);
				}
			}
		}

		const FieldValue* FindField(const std::vector<FieldValue>& fields, uint8_t index)
		{
			for (const FieldValue& field : fields)
			{
				if (field.Index == index)
					return &field;
			}
			return nullptr;
		}

		bool FieldEqual(const FieldValue& a, const FieldValue& b)
		{
			if (a.Type != b.Type)
				return false;
			if (a.Type == kFieldStr16)
				return a.Text == b.Text;
			return a.Number == b.Number;
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
				else if (entity.HasComponent<PrefabInstanceComponent>())
				{
					// A prefab root spawned by Scene.Spawn, or placed in the scene, names its
					// source. The client instantiates the same file with the same root UUID, so
					// its children come out with the same derived UUIDs.
					const AssetMetadata* meta = AssetManager::GetMetadata(entity.GetComponent<PrefabInstanceComponent>().Source);
					if (meta)
						state.Prefab = meta->FilePath;
				}

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

				std::vector<FieldValue> fields;
				if (ScriptEngine::CopyReplicated(entity.GetUUID(), fields))
				{
					state.HasScript = true;
					state.Script = std::move(fields);
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
				if (entity.HasScript)
				{
					for (const FieldValue& field : entity.Script)
					{
						const FieldValue* old = previous->HasScript
							? FindField(previous->Script, field.Index) : nullptr;
						if (!old || !FieldEqual(*old, field))
							update.Script.push_back(field);
					}
					if (!update.Script.empty())
					{
						update.HasScript = true;
						changed = true;
					}
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

		int MeasureAll(const std::vector<RepState>& records, int count, const std::vector<WireEvent>& events)
		{
			BitMeasure measure;
			uint32_t tick = 0;
			uint32_t baseline = 0;
			uint32_t owned = 0;
			uint32_t n = static_cast<uint32_t>(count);
			HeaderBits(measure, tick, baseline, owned, n);
			for (int i = 0; i < count; i++)
			{
				RepState copy = records[static_cast<size_t>(i)];
				if (!RecordBits(measure, copy))
					return kBudget + 1;
			}
			std::vector<WireEvent> trailer = events;
			if (!TrailerBits(measure, trailer))
				return kBudget + 1;
			return measure.Bytes();
		}

		int FitCount(const std::vector<RepState>& records, const std::vector<WireEvent>& events)
		{
			int lo = 0;
			int hi = static_cast<int>(records.size());
			while (lo < hi)
			{
				const int mid = (lo + hi + 1) / 2;
				if (MeasureAll(records, mid, events) <= kBudget)
					lo = mid;
				else
					hi = mid - 1;
			}
			return lo;
		}

		std::vector<uint8_t> WritePacket(uint32_t tick, uint16_t baseline, uint16_t owned,
			const std::vector<RepState>& records, int count, const std::vector<WireEvent>& events)
		{
			std::vector<uint8_t> out;
			BitWrite writer(out);
			uint32_t tickBits = tick;
			uint32_t baselineBits = baseline;
			uint32_t ownedBits = owned;
			uint32_t countBits = static_cast<uint32_t>(count);
			HeaderBits(writer, tickBits, baselineBits, ownedBits, countBits);
			for (int i = 0; i < count; i++)
			{
				RepState copy = records[static_cast<size_t>(i)];
				RecordBits(writer, copy);
			}
			std::vector<WireEvent> trailer = events;
			TrailerBits(writer, trailer);
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

		bool Decode(const uint8_t* data, int length, uint32_t& tick, uint16_t& baseline, uint16_t& owned,
			std::vector<RepState>& records, std::vector<WireEvent>& events)
		{
			BitRead reader(data, length);
			uint32_t baselineBits = 0;
			uint32_t ownedBits = 0;
			uint32_t count = 0;
			HeaderBits(reader, tick, baselineBits, ownedBits, count);
			baseline = static_cast<uint16_t>(baselineBits);
			owned = static_cast<uint16_t>(ownedBits);
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
			events.clear();
			if (!TrailerBits(reader, events))
				return false;
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
			if (record.HasVel)
			{
				sample.HasVel = true;
				sample.Vx = UnquantizeSpeed(record.Vx);
				sample.Vy = UnquantizeSpeed(record.Vy);
				sample.Vz = UnquantizeSpeed(record.Vz);
			}
			if (record.HasGround)
			{
				sample.HasGround = true;
				sample.Ground = record.Ground;
			}
		}

		// The fields that differ from what the scene was last given for this entity. All of them
		// when there was nothing before. A field the script changes locally is not overwritten
		// every snapshot, only when the server's value moves.
		void QueueChangedFields(World& world, UUID id, const RepState& entity, const RepState* before)
		{
			if (!entity.HasScript || entity.Script.empty() || id == UUID{ 0 })
				return;
			FieldWrite write;
			write.Id = id;
			for (const FieldValue& field : entity.Script)
			{
				const FieldValue* old = before && before->HasScript ? FindField(before->Script, field.Index) : nullptr;
				if (!old || !FieldEqual(*old, field))
					write.Fields.push_back(field);
			}
			if (!write.Fields.empty())
				world.FieldWrites.push_back(std::move(write));
		}

		void SortById(std::vector<RepState>& list)
		{
			std::sort(list.begin(), list.end(), [](const RepState& a, const RepState& b) { return a.NetId < b.NetId; });
		}

		const RepState* FindSorted(const std::vector<RepState>& list, uint16_t id)
		{
			auto it = std::lower_bound(list.begin(), list.end(), id,
				[](const RepState& state, uint16_t value) { return state.NetId < value; });
			return it != list.end() && it->NetId == id ? &*it : nullptr;
		}

		void DestroyGhost(Scene& scene, World& world, uint16_t netId)
		{
			auto ghost = world.Ghosts.find(netId);
			if (ghost == world.Ghosts.end())
				return;
			Entity entity = scene.FindEntityByUUID(ghost->second.Id);
			if (entity)
				DestroyTree(scene, entity);
			if (ghost->second.Id == world.LocalId)
				world.LocalId = UUID{ 0 };
			world.Ghosts.erase(ghost);
		}

		// The scene entity for a replicated one the client does not have a ghost for yet: the
		// scene's own entity with that UUID, the named prefab, or a bare entity.
		Entity SpawnGhostEntity(Scene& scene, World& world, const RepState& state)
		{
			Entity entity = scene.FindEntityByUUID(UUID(state.Uuid));
			if (!entity && !state.Prefab.empty())
			{
				if (world.RefusedPrefab.count(state.NetId))
					return {};
				if (!SafePrefabPath(state.Prefab))
				{
					world.RefusedPrefab.insert(state.NetId);
					GE_CORE_WARN("Replicate refused a prefab path");
					return {};
				}
				PrefabSerializer::InstantiateOptions options;
				options.RootUUID = UUID(state.Uuid);
				entity = PrefabSerializer::Instantiate(GetAssetRoot() / state.Prefab, scene, UUID{ 0 }, options);
				if (entity)
					Test::OnClientSpawn(scene, entity, state.Prefab);
			}
			if (!entity)
				entity = scene.CreateEntityWithUUID(UUID(state.Uuid), "replicated");
			if (!entity)
				return {};
			if (!entity.HasComponent<ReplicatedComponent>())
				entity.AddComponent<ReplicatedComponent>();
			entity.GetComponent<ReplicatedComponent>().NetId = state.NetId;
			return entity;
		}

		bool ApplyPacket(Scene& scene, const uint8_t* data, int length, float rttMs,
			std::chrono::steady_clock::time_point at, uint64_t session, uint16_t sequence)
		{
			uint32_t tick = 0;
			uint16_t baselineSeq = 0;
			uint16_t owned = 0;
			std::vector<RepState> records;
			std::vector<WireEvent> events;
			if (!Decode(data, length, tick, baselineSeq, owned, records, events))
				return false;

			World& world = State();
			if (session != world.LatestSession)
			{
				world.LatestSession = session;
				world.HaveLatest = false;
				world.Received.clear();
			}

			// Decode onto exactly the baseline the server named, never onto whatever the client
			// applied last. A field missing from a delta means "as in that baseline", and the
			// latest state can differ from it. A value that returned to its baselined value within
			// a round trip (a position on the way back through a reversal, a flag toggled twice)
			// would otherwise be read as the latest, stale value. StampAfter is the server's own
			// function for what the client holds after a snapshot, so both sides compute the same
			// state from the same records. This is Quake 3's delta rule.
			static const std::vector<RepState> kNothing;
			const std::vector<RepState>* base = baselineSeq == 0 ? &kNothing : nullptr;
			for (const World::ClientSnap& snap : world.Received)
			{
				if (base)
					break;
				if (snap.Seq == baselineSeq)
					base = &snap.State;
			}
			if (!base)
			{
				// Every baseline is a snapshot this client acknowledged, and the server picks
				// among its last 32, so this should not happen. Counted, not guessed around.
				world.NoBaseline++;
				return false;
			}
			std::vector<RepState> full = StampAfter(*base, records, static_cast<int>(records.size()));
			SortById(full);

			// Kept whether or not it is applied below: a snapshot that arrives late is still a
			// state the server may name as a baseline.
			bool known = false;
			for (const World::ClientSnap& snap : world.Received)
				known = known || snap.Seq == sequence;
			if (!known)
			{
				world.Received.push_back({ sequence, full });
				if (world.Received.size() > 64)
					world.Received.erase(world.Received.begin());
			}

			// Snapshots apply in tick order or not at all. UDP reorders and duplicates, and an old
			// snapshot applied after a newer one would move LatestTick backwards and put older
			// state on the scene. Nothing is lost by not applying it: the newer snapshot's state
			// already includes whatever this one changed.
			if (world.HaveLatest && tick <= world.LatestTick)
			{
				world.StaleSnapshots++;
				return false;
			}

			for (WireEvent& event : events)
			{
				HeldEvent held;
				held.Event = std::move(event);
				world.Held.push_back(std::move(held));
			}
			world.OwnedNetId = owned;
			world.LatestTick = tick;
			world.LatestAt = at;
			world.RttMs = rttMs;
			world.LastSnapBytes = length;
			world.HaveLatest = true;

			// The scene follows the full state: what left it is destroyed, what joined it is
			// spawned, and every entity gets a sample of its whole state at this tick.
			for (const RepState& old : world.Applied)
			{
				if (!FindSorted(full, old.NetId))
					DestroyGhost(scene, world, old.NetId);
			}
			for (const RepState& entity : full)
			{
				const RepState* before = FindSorted(world.Applied, entity.NetId);
				auto ghost = world.Ghosts.find(entity.NetId);
				if (ghost == world.Ghosts.end())
				{
					Entity created = SpawnGhostEntity(scene, world, entity);
					if (!created)
						continue;
					world.Ghosts[entity.NetId].Id = created.GetUUID();
					ghost = world.Ghosts.find(entity.NetId);
					before = nullptr;
				}
				Sample sample;
				sample.Tick = tick;
				OverlaySample(sample, entity);
				ghost->second.Latest = sample;
				PushSample(ghost->second, sample);
				QueueChangedFields(world, ghost->second.Id, entity, before);
			}
			world.Applied = std::move(full);

			if (world.OwnedNetId != 0)
			{
				auto ownedGhost = world.Ghosts.find(world.OwnedNetId);
				if (ownedGhost != world.Ghosts.end())
					world.LocalId = ownedGhost->second.Id;
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

		int LeadTicks(float rttMs, float fixedDt)
		{
			// T + (now - recv) / dt is the snapshot's tick plus how long that packet has
			// sat on the client. The flight is not in it, so at receipt the estimate is
			// about half a round trip behind the server. The command spends the other
			// half on the way back. Leading by the whole round trip is half a round trip
			// ahead of the server, which is the tick that command arrives for. Half a
			// round trip only catches the server's present, and the command is late.
			//
			// That is the tick the command is due on, with nothing to spare, and a command a tick
			// late is not used: the server repeats the previous one, and at a change of direction
			// that is a correction. The margin covers what the round trip does not show. The
			// server steps several ticks per frame, so a command has to arrive before the frame
			// that simulates its tick, not before the tick itself. The client sends at the end of
			// its own frame. And the RTT is a smoothed average, not the slow packets.
			const float ms = fixedDt > 0.0f ? fixedDt * 1000.0f : (1000.0f / 60.0f);
			int ticks = static_cast<int>(std::ceil(rttMs / ms)) + kLeadMarginTicks;
			if (ticks < 1)
				ticks = 1;
			if (ticks > 32)
				ticks = 32;
			return ticks;
		}

		int16_t QuantizeStick(float value)
		{
			value = std::clamp(value, -1.0f, 1.0f);
			return static_cast<int16_t>(std::lround(value * 32767.0f));
		}

		float DequantizeStick(int16_t value)
		{
			return static_cast<float>(value) / 32767.0f;
		}

		void AppendCommand(std::vector<uint8_t>& out, const PlayerCommand& cmd)
		{
			const uint32_t tick = cmd.Tick;
			out.push_back(static_cast<uint8_t>(tick));
			out.push_back(static_cast<uint8_t>(tick >> 8));
			out.push_back(static_cast<uint8_t>(tick >> 16));
			out.push_back(static_cast<uint8_t>(tick >> 24));
			const auto put16 = [&](uint16_t value)
			{
				out.push_back(static_cast<uint8_t>(value));
				out.push_back(static_cast<uint8_t>(value >> 8));
			};
			put16(static_cast<uint16_t>(QuantizeStick(cmd.MoveX)));
			put16(static_cast<uint16_t>(QuantizeStick(cmd.MoveZ)));
			put16(QuantizeYaw(cmd.Yaw));
			put16(QuantizeSignedAngle(cmd.Pitch));
			out.push_back(cmd.PackButtons());
		}

		bool ReadCommandAt(const uint8_t* data, int length, int& cursor, PlayerCommand& cmd)
		{
			if (cursor + 13 > length)
				return false;
			const auto u16 = [&]()
			{
				const uint16_t value = static_cast<uint16_t>(data[cursor] | (data[cursor + 1] << 8));
				cursor += 2;
				return value;
			};
			const uint32_t tick = static_cast<uint32_t>(data[cursor])
				| (static_cast<uint32_t>(data[cursor + 1]) << 8)
				| (static_cast<uint32_t>(data[cursor + 2]) << 16)
				| (static_cast<uint32_t>(data[cursor + 3]) << 24);
			cursor += 4;
			cmd.Tick = tick;
			cmd.MoveX = DequantizeStick(static_cast<int16_t>(u16()));
			cmd.MoveZ = DequantizeStick(static_cast<int16_t>(u16()));
			cmd.Yaw = UnquantizeYaw(u16());
			cmd.Pitch = UnquantizeSignedAngle(u16());
			cmd.UnpackButtons(data[cursor++]);
			return true;
		}

		void IngestCommands(World& world)
		{
			const std::vector<CommandDatagram> packets = DrainCommands();
			for (const CommandDatagram& packet : packets)
			{
				if (packet.Bytes.empty())
					continue;
				const int count = packet.Bytes[0];
				if (count < 1 || count > 4)
					continue;
				int cursor = 1;
				AccountInput& input = world.Inputs[packet.Account];
				for (int i = 0; i < count; i++)
				{
					PlayerCommand cmd;
					if (!ReadCommandAt(packet.Bytes.data(), static_cast<int>(packet.Bytes.size()), cursor, cmd))
						break;
					if (world.Tick > 128 && cmd.Tick + 128 < world.Tick)
						continue;
					input.ByTick[cmd.Tick] = cmd;
				}
				while (input.ByTick.size() > 128)
					input.ByTick.erase(input.ByTick.begin());
			}
		}

		PlayerCommand TakeCommand(World& world, const std::string& account, uint32_t tick)
		{
			AccountInput& input = world.Inputs[account];
			PlayerCommand cmd;
			const auto it = input.ByTick.find(tick);
			if (it != input.ByTick.end())
			{
				cmd = it->second;
				input.Last = cmd;
				input.HaveLast = true;
			}
			else
			{
				cmd = input.HaveLast ? input.Last : PlayerCommand{};
				cmd.Tick = tick;
				input.Repeats++;
			}
			if (!input.WalkMarked && (cmd.MoveX != 0.0f || cmd.MoveZ != 0.0f))
			{
				input.WalkMarked = true;
				input.RepeatsAtWalk = input.Repeats;
			}
			while (!input.ByTick.empty() && input.ByTick.begin()->first + 8 < tick)
				input.ByTick.erase(input.ByTick.begin());
			return cmd;
		}

		void NoteLook(World& world)
		{
			if (world.LookNoted || Test::Active())
				return;
			world.LookNoted = true;
			// Look only while the cursor is captured. A game that releases the cursor (Escape, a
			// menu) must be able to move the mouse without turning the player.
			if (Input::GetCursorMode() != CursorMode::Locked)
				return;
			const glm::vec2 mouse = Input::GetGameMouseDelta();
			world.ViewYaw -= mouse.x * 0.002f;
			world.ViewPitch -= mouse.y * 0.002f;
			world.ViewPitch = std::clamp(world.ViewPitch, -1.4f, 1.4f);
		}

		PlayerCommand SampleCommand(World& world, uint32_t tick)
		{
			NoteLook(world);
			PlayerCommand cmd;
			cmd.Tick = tick;
			cmd.Yaw = world.ViewYaw;
			cmd.Pitch = world.ViewPitch;
			if (Test::DrivesInput())
			{
				Test::Stick(cmd, world.SettleFrom);
				return cmd;
			}

			if (Input::IsGameKeyPressed(Key::A))
				cmd.MoveX -= 1.0f;
			if (Input::IsGameKeyPressed(Key::D))
				cmd.MoveX += 1.0f;
			if (Input::IsGameKeyPressed(Key::W))
				cmd.MoveZ -= 1.0f;
			if (Input::IsGameKeyPressed(Key::S))
				cmd.MoveZ += 1.0f;
			const float mag = std::sqrt(cmd.MoveX * cmd.MoveX + cmd.MoveZ * cmd.MoveZ);
			if (mag > 1.0f)
			{
				cmd.MoveX /= mag;
				cmd.MoveZ /= mag;
			}
			if (Input::IsGameKeyPressed(Key::Space))
				cmd.Jump = true;
			if (Input::IsGameMouseButtonPressed(Mouse::Button0))
				cmd.Fire = true;
			if (Input::IsGameMouseButtonPressed(Mouse::Button1))
				cmd.Melee = true;
			if (Input::IsGameKeyPressed(Key::E))
				cmd.Interact = true;
			for (int i = 0; i < 10; i++)
			{
				const KeyCode key = static_cast<KeyCode>(Key::D0 + i);
				if (Input::IsGameKeyPressed(key))
					cmd.Equip = static_cast<uint8_t>(i);
			}
			return cmd;
		}

		Predicted* FindHistory(World& world, uint32_t tick)
		{
			Predicted& slot = world.History[tick & 63];
			if (!slot.Valid || slot.Command.Tick != tick)
				return nullptr;
			return &slot;
		}

		void StoreHistory(World& world, const PlayerCommand& cmd, const glm::vec3& position,
			const glm::vec3& velocity, int ground)
		{
			Predicted& slot = world.History[cmd.Tick & 63];
			slot.Command = cmd;
			slot.Position = position;
			slot.Velocity = velocity;
			slot.Ground = ground;
			slot.Valid = true;
		}

		void TickOne(Scene& scene, Entity entity, uint32_t tick, const PlayerCommand& cmd)
		{
			if (entity.HasComponent<ScriptComponent>())
				ScriptEngine::Tick(static_cast<entt::entity>(entity), tick, cmd);
			if (entity.HasComponent<NativeScriptComponent>())
			{
				ScriptableEntity* instance = entity.GetComponent<NativeScriptComponent>().Instance;
				if (instance)
					instance->DispatchTick(tick, cmd);
			}
		}

		void TickScripts(Scene& scene, World& world, uint32_t tick, const PlayerCommand& local)
		{
			ScriptEngine::SetSceneContext(&scene);
			const bool server = scene.GetRole() == SceneRole::Server;
			const auto each = [&](Entity entity)
			{
				PlayerCommand cmd = local;
				cmd.Tick = tick;
				if (server)
				{
					bool owned = false;
					for (const Pawn& pawn : world.Pawns)
					{
						if (pawn.Id != entity.GetUUID())
							continue;
						cmd = TakeCommand(world, pawn.Account, tick);
						owned = true;
						break;
					}
					if (!owned)
					{
						cmd = {};
						cmd.Tick = tick;
					}
				}
				TickOne(scene, entity, tick, cmd);
			};

			for (auto handle : scene.Reg().view<ScriptComponent>())
				each(Entity{ handle, &scene });
			for (auto handle : scene.Reg().view<NativeScriptComponent>())
			{
				Entity entity{ handle, &scene };
				if (entity.HasComponent<ScriptComponent>())
					continue;
				each(entity);
			}
		}

		void SendRecent(World& world)
		{
			PlayerCommand recent[4];
			int count = 0;
			for (int back = 3; back >= 0; --back)
			{
				if (world.SimTick < static_cast<uint32_t>(back))
					continue;
				Predicted* slot = FindHistory(world, world.SimTick - static_cast<uint32_t>(back));
				if (!slot)
					continue;
				recent[count++] = slot->Command;
			}
			if (count == 0)
				return;
			std::vector<uint8_t> bytes;
			bytes.push_back(static_cast<uint8_t>(count));
			for (int i = 0; i < count; i++)
				AppendCommand(bytes, recent[i]);
			SendCommands(bytes.data(), static_cast<int>(bytes.size()));
		}

		bool Reconcile(Scene& scene, PhysicsScene* physics, World& world, Entity entity, glm::vec3& drawn)
		{
			if (world.OwnedNetId == 0)
				return false;
			auto ghost = world.Ghosts.find(world.OwnedNetId);
			if (ghost == world.Ghosts.end())
				return false;

			const Sample* auth = nullptr;
			for (const Sample& sample : ghost->second.Buffer)
			{
				if (!sample.HasPos || !sample.HasVel || !sample.HasGround)
					continue;
				if (sample.Tick > world.SimTick)
					continue;
				if (!auth || sample.Tick > auth->Tick)
					auth = &sample;
			}
			if (!auth)
				return false;

			// A later snapshot that omits this entity is still authoritative: the
			// pose did not change. Comparing only the tick the fields last appeared
			// on would let a diverged prediction stand once the server stopped moving.
			uint32_t authTick = auth->Tick;
			if (world.HaveLatest && world.LatestTick > authTick && world.LatestTick <= world.SimTick)
				authTick = world.LatestTick;
			if (world.HaveReconciled && authTick <= world.LastReconciled)
				return false;

			world.LastReconciled = authTick;
			world.HaveReconciled = true;
			Predicted* predicted = FindHistory(world, authTick);
			if (!predicted)
				return false;

			const glm::vec3 serverPos = SamplePos(*auth);
			const glm::vec3 serverVel{ auth->Vx, auth->Vy, auth->Vz };
			const float posErr = Distance(predicted->Position, serverPos);
			const float velErr = Distance(predicted->Velocity, serverVel);
			const bool groundMiss = predicted->Ground != auth->Ground;
			if (posErr <= 0.02f && velErr <= 0.10f && !groundMiss)
				return false;

			drawn = entity.GetComponent<TransformComponent>().Translation;
			physics->RestoreCharacter(world.LocalId, serverPos, serverVel, auth->Ground);
			predicted->Position = serverPos;
			predicted->Velocity = serverVel;
			predicted->Ground = auth->Ground;

			uint32_t replay = 0;
			const float fixedDt = FixedDt(scene);
			for (uint32_t tick = authTick + 1; tick <= world.SimTick; tick++)
			{
				Predicted* step = FindHistory(world, tick);
				if (!step)
					break;
				TickOne(scene, entity, tick, step->Command);
				physics->ReplayCharacter(world.LocalId, fixedDt);
				glm::vec3 position{ 0.0f };
				glm::vec3 velocity{ 0.0f };
				int ground = 3;
				if (physics->SampleCharacter(world.LocalId, position, velocity, ground))
				{
					step->Position = position;
					step->Velocity = velocity;
					step->Ground = ground;
				}
				replay++;
			}

			const bool walking = world.SettleFrom != 0 && authTick >= world.SettleFrom;
			const float mm = posErr * 1000.0f;
			if (walking)
			{
				world.Corrections++;
				if (mm > world.MaxCorrectMm)
					world.MaxCorrectMm = mm;
				if (static_cast<int>(replay) > world.MaxReplay)
					world.MaxReplay = static_cast<int>(replay);
			}
			GE_CORE_INFO("Predict correct tick={0} sizeMm={1:.0f} replay={2} settle={3}",
				authTick, mm, replay, walking ? 0 : 1);
			return true;
		}

	}

	void PresentWorld(Scene& scene, float frameDt)
	{
		if (scene.GetRole() != SceneRole::Client)
			return;

		World& world = State();
		const auto packets = DrainSnapshots();
		for (const SnapshotDatagram& packet : packets)
			ApplyPacket(scene, packet.Bytes.data(), static_cast<int>(packet.Bytes.size()), packet.RttMs, packet.At,
				packet.Session, packet.Sequence);

		if (!world.HaveLatest)
			return;

		const float fixedDt = FixedDt(scene);
		const float elapsed = std::chrono::duration<float>(std::chrono::steady_clock::now() - world.LatestAt).count();
		const float estimated = static_cast<float>(world.LatestTick) + elapsed / fixedDt;
		world.Estimated = estimated;
		world.HaveEstimate = true;
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
			// Until prediction starts, the spawn pose has to land on the transform or the
			// body is created at the prefab origin. After that, interpolation would
			// fight the prediction.
			if (Net::Controls(ghost.Id) && world.HaveSim)
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
				"Replicate client tick={0} est={1:.1f} lead={2:.0f} snap={3} depth={4} dry={5:.2f} step={6:.2f} entities={7} registry={8} ghosts={9} frames={10} stale={11} nobase={12}",
				static_cast<uint32_t>(renderTick), estimated, world.RttMs * 0.5f, world.LastSnapBytes, depth,
				dryPct, world.MaxRatio, static_cast<int>(scene.Reg().view<ReplicatedComponent>().size()),
				RegistryCount(scene), static_cast<int>(world.Ghosts.size()), world.Frames, world.StaleSnapshots,
				world.NoBaseline);
		}
	}

	void NoteSimulatedStep(Scene& scene)
	{
		if (scene.GetRole() != SceneRole::Server)
			return;
		State().Tick++;
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
		for (Pawn& pawn : world.Pawns)
		{
			Entity entity = scene.FindEntityByUUID(pawn.Id);
			if (entity && entity.HasComponent<ReplicatedComponent>())
				pawn.NetId = entity.GetComponent<ReplicatedComponent>().NetId;
		}
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

		// One copy for every peer. Unreliable events are not in the baseline and are not
		// retried: a client that misses this snapshot misses them. Records keep the budget.
		std::vector<WireEvent> riding;
		riding.swap(world.Unreliable);

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

			// A destroy is settled once a snapshot that carried it has been acknowledged.
			for (Ring::Snap& snap : ring.Snaps)
			{
				if (snap.Confirmed || !SnapshotAcked(account, snap.Seq))
					continue;
				snap.Confirmed = true;
				for (uint16_t id : snap.Destroys)
					ring.MayHave.erase(id);
			}

			std::vector<RepState> records = Diff(baseline, current);
			// A spawn the client applied whose ack we have not seen is not in the acked
			// baseline, so if the entity is already gone the diff does not mention it. The
			// client may still hold it, and keeps it for good unless it is told. So anything
			// a spawn went out for and that is gone gets a destroy here, again in every
			// snapshot, until one of them is acknowledged. Checking only the last stamp is
			// not enough: when the one snapshot carrying the destroy is lost, the next stamp
			// no longer lists the entity, and nothing would ever say it again.
			for (uint16_t id : ring.MayHave)
			{
				if (FindId(current, id) || FindId(records, id))
					continue;
				RepState destroyed;
				destroyed.Op = kDestroy;
				destroyed.NetId = id;
				records.push_back(destroyed);
			}
			std::stable_partition(records.begin(), records.end(), [](const RepState& record)
			{
				return record.Op == kDestroy;
			});
			std::vector<WireEvent> events = riding;
			while (!events.empty() && MeasureAll(records, 0, events) > kBudget)
			{
				GE_CORE_WARN("Dropped unreliable script event '{0}'; the records filled the datagram",
					events.back().Name);
				events.pop_back();
			}
			const int fit = FitCount(records, events);
			if (!records.empty() && fit == 0)
				GE_CORE_WARN("Replicate record does not fit in one datagram");
			const std::vector<RepState> stamp = StampAfter(baseline, records, fit);
			uint16_t owned = 0;
			for (const Pawn& pawn : world.Pawns)
			{
				if (pawn.Account == account)
					owned = pawn.NetId;
			}
			const std::vector<uint8_t> bytes = WritePacket(world.Tick, baselineSeq, owned, records, fit, events);
			const uint16_t sequence = SendSnapshot(account, bytes.data(), static_cast<int>(bytes.size()));
			if (sequence == 0)
				continue;

			Ring::Snap sent;
			sent.Seq = sequence;
			sent.State = stamp;
			for (int i = 0; i < fit; i++)
			{
				const RepState& record = records[static_cast<size_t>(i)];
				if (record.Op == kSpawn)
					ring.MayHave.insert(record.NetId);
				else if (record.Op == kDestroy)
					sent.Destroys.push_back(record.NetId);
			}
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

	bool Controls(UUID id)
	{
		return id != UUID{ 0 } && State().LocalId == id;
	}

	void PredictLocal(Scene& scene, PhysicsScene* physics)
	{
		if (scene.GetRole() != SceneRole::Client || !physics)
			return;

		World& world = State();
		if (!world.HaveEstimate || world.LocalId == UUID{ 0 })
			return;
		Entity entity = scene.FindEntityByUUID(world.LocalId);
		if (!entity || !entity.HasComponent<CharacterControllerComponent>())
			return;

		glm::vec3 position{ 0.0f };
		glm::vec3 velocity{ 0.0f };
		int ground = 3;
		if (!physics->SampleCharacter(world.LocalId, position, velocity, ground))
			return;

		const float fixedDt = FixedDt(scene);
		if (!world.HaveSim)
		{
			world.SimTick = world.LatestTick;
			world.HaveSim = true;
			world.SettleFrom = world.LatestTick + 60;
			GE_CORE_INFO("Predict own net={0} tick={1}", world.OwnedNetId, world.LatestTick);
		}

		glm::vec3 drawn{ 0.0f };
		const bool corrected = Reconcile(scene, physics, world, entity, drawn);

		const int lead = LeadTicks(world.RttMs, fixedDt);
		uint32_t target = static_cast<uint32_t>(std::floor(world.Estimated)) + static_cast<uint32_t>(lead);
		int steps = 0;
		while (world.SimTick < target && steps < 8)
		{
			world.SimTick++;
			const PlayerCommand cmd = SampleCommand(world, world.SimTick);
			glm::vec3 before{ 0.0f };
			glm::vec3 ignored{ 0.0f };
			int ignoredGround = 3;
			physics->SampleCharacter(world.LocalId, before, ignored, ignoredGround);
			TickOne(scene, entity, world.SimTick, cmd);
			physics->ReplayCharacter(world.LocalId, fixedDt);
			if (!physics->SampleCharacter(world.LocalId, position, velocity, ground))
				break;
			StoreHistory(world, cmd, position, velocity, ground);

			const bool moving = cmd.MoveX != 0.0f || cmd.MoveZ != 0.0f;
			if (moving && !world.LoggedInput)
			{
				world.LoggedInput = true;
				GE_CORE_INFO("Predict input tick={0}", world.SimTick);
			}
			if (world.LoggedInput && !world.LoggedMoved && Distance(before, position) > 0.001f)
			{
				world.LoggedMoved = true;
				GE_CORE_INFO("Predict moved tick={0}", world.SimTick);
			}
			SendRecent(world);
			steps++;
		}

		if (corrected)
		{
			if (physics->SampleCharacter(world.LocalId, position, velocity, ground))
				world.VisualError = drawn - position;
		}

		const auto now = std::chrono::steady_clock::now();
		if (now >= world.NextPredictLog)
		{
			world.NextPredictLog = now + std::chrono::seconds(1);
			GE_CORE_INFO("Predict client tick={0} corrections={1} maxMm={2:.0f} replay={3} leadTicks={4}",
				world.SimTick, world.Corrections, world.MaxCorrectMm, world.MaxReplay, lead);
		}

		Test::PredictProgress progress;
		progress.SimTick = world.SimTick;
		progress.SettleFrom = world.SettleFrom;
		progress.Corrections = world.Corrections;
		progress.MaxCorrectMm = world.MaxCorrectMm;
		progress.MaxReplay = world.MaxReplay;
		Test::AfterPredict(progress);
	}

	void BeginStep(Scene& scene, PhysicsScene* physics)
	{
		if (scene.GetRole() == SceneRole::Client)
			return;

		World& world = State();
		IngestCommands(world);

		uint32_t tick = 0;
		if (scene.GetRole() == SceneRole::Server)
			tick = world.Tick + 1;
		else
			tick = ++world.OfflineTicks;
		world.ScriptTick = tick;

		Test::BeforeScripts(scene, physics, tick);

		const PlayerCommand local = scene.GetRole() == SceneRole::Server
			? PlayerCommand{} : SampleCommand(world, tick);
		TickScripts(scene, world, tick, local);

		if (scene.GetRole() == SceneRole::Server)
			Test::AfterServerStep(scene, world.Tick);
		else
			Test::AfterStandaloneStep(scene, tick);
	}

	void ApplyVisualError(Scene& scene, float frameDt)
	{
		World& world = State();
		world.LookNoted = false;
		if (scene.GetRole() != SceneRole::Client || world.LocalId == UUID{ 0 })
			return;
		if (glm::dot(world.VisualError, world.VisualError) < 1.0e-6f)
		{
			world.VisualError = glm::vec3{ 0.0f };
			return;
		}
		Entity entity = scene.FindEntityByUUID(world.LocalId);
		if (!entity || !entity.HasComponent<TransformComponent>())
			return;
		entity.GetComponent<TransformComponent>().Translation += world.VisualError;
		scene.MarkChanged<TransformComponent>(entity);
		world.VisualError *= std::exp(-frameDt / 0.1f);
	}

	namespace {

		std::vector<uint8_t> PackReliable(const WireEvent& event)
		{
			std::vector<uint8_t> body;
			BitWrite writer(body);
			WireEvent copy = event;
			if (!OneEventBits(writer, copy))
				return {};
			if (body.size() + 1 > 256)
				return {};
			body.insert(body.begin(), 3);
			return body;
		}

		void TrimEvent(WireEvent& event)
		{
			if (event.Name.size() > 16)
				event.Name.resize(16);
			if (event.Args.size() > 4)
				event.Args.resize(4);
			for (EventArg& arg : event.Args)
			{
				if (arg.Key.size() > 12)
					arg.Key.resize(12);
				if (arg.Text.size() > 16)
					arg.Text.resize(16);
			}
		}

	}

	void DeliverScriptTraffic(Scene& scene)
	{
		if (scene.GetRole() != SceneRole::Client)
			return;

		World& world = State();
		for (const FieldWrite& write : world.FieldWrites)
			ScriptEngine::ApplyReplicated(write.Id, write.Fields);
		world.FieldWrites.clear();

		std::vector<HeldEvent> kept;
		for (HeldEvent& held : world.Held)
		{
			auto ghost = world.Ghosts.find(held.Event.NetId);
			if (ghost == world.Ghosts.end())
			{
				if (!held.Event.Reliable)
					continue;
				held.Waits++;
				if (held.Waits < 60)
					kept.push_back(std::move(held));
				else
					GE_CORE_WARN("Reliable script event '{0}' dropped; net {1} never spawned",
						held.Event.Name, held.Event.NetId);
				continue;
			}

			Entity entity = scene.FindEntityByUUID(ghost->second.Id);
			if (!entity)
				continue;

			if (held.Event.Kind == 2)
			{
				if (entity.IsLocallyControlled())
				{
					GE_CORE_INFO("Shot echo suppressed net={0}", held.Event.NetId);
					continue;
				}
				const WireEvent& shot = held.Event;
				ScriptEngine::DeliverShot(entity.GetUUID(), shot.Mx, shot.My, shot.Mz,
					shot.Dx, shot.Dy, shot.Dz, shot.Speed, shot.Tick);
				continue;
			}

			ScriptEngine::DeliverNetEvent(entity.GetUUID(), held.Event.Name, held.Event.Args);
		}
		world.Held = std::move(kept);
	}

	UUID SpawnReplicatedPrefab(Scene& scene, const std::string& relative)
	{
		PrefabSerializer::InstantiateOptions options;
		options.RootUUID = UUID();
		Entity root = PrefabSerializer::Instantiate(GetAssetRoot() / relative, scene, UUID{ 0 }, options);
		if (!root)
		{
			GE_CORE_ERROR("Replicate could not spawn {0}", relative);
			return UUID{ 0 };
		}
		if (!root.HasComponent<ReplicatedComponent>())
			root.AddComponent<ReplicatedComponent>();
		State().PrefabOf[static_cast<uint64_t>(root.GetUUID())] = relative;
		return root.GetUUID();
	}

	void AssignPawn(const std::string& account, UUID id)
	{
		World& world = State();
		for (Pawn& pawn : world.Pawns)
		{
			if (pawn.Account != account)
				continue;
			pawn.Id = id;
			pawn.NetId = 0;
			pawn.AssignedTick = world.Tick;
			return;
		}
		Pawn pawn;
		pawn.Account = account;
		pawn.Id = id;
		pawn.AssignedTick = world.Tick;
		world.Pawns.push_back(std::move(pawn));
	}

	bool HasPawn(const std::string& account)
	{
		for (const Pawn& pawn : State().Pawns)
		{
			if (pawn.Account == account)
				return true;
		}
		return false;
	}

	std::vector<PawnInfo> Pawns()
	{
		std::vector<PawnInfo> out;
		for (const Pawn& pawn : State().Pawns)
			out.push_back({ pawn.Account, pawn.Id, pawn.AssignedTick });
		return out;
	}

	uint32_t ServerTick()
	{
		return State().Tick;
	}

	namespace {
		std::string& PendingMatchEnd()
		{
			static std::string outcome;
			return outcome;
		}
	}

	bool RequestMatchEnd(const std::string& outcome)
	{
		if (outcome != "victory" && outcome != "defeat")
			return false;
		std::string& pending = PendingMatchEnd();
		if (pending.empty())
			pending = outcome;
		return true;
	}

	bool TakeMatchEnd(std::string& outcome)
	{
		std::string& pending = PendingMatchEnd();
		if (pending.empty())
			return false;
		outcome = pending;
		return true;
	}

	InputStats GetInputStats()
	{
		InputStats stats;
		for (const auto& input : State().Inputs)
		{
			stats.Repeats += input.second.Repeats;
			if (input.second.WalkMarked)
				stats.RepeatsSinceMove += input.second.Repeats - input.second.RepeatsAtWalk;
		}
		return stats;
	}

	uint16_t EnsureNetId(Scene& scene, UUID id)
	{
		Entity entity = scene.FindEntityByUUID(id);
		if (!entity || !entity.HasComponent<ReplicatedComponent>())
			return 0;
		auto& replicated = entity.GetComponent<ReplicatedComponent>();
		if (replicated.NetId != 0)
			return replicated.NetId;
		World& world = State();
		replicated.NetId = world.NextId++;
		if (world.NextId == 0)
			world.NextId = 1;
		return replicated.NetId;
	}

	uint32_t CurrentScriptTick()
	{
		return State().ScriptTick;
	}

	void BroadcastEvent(uint16_t netId, std::string name, std::vector<EventArg> args, bool reliable)
	{
		if (netId == 0 || name.empty())
			return;
		WireEvent event;
		event.NetId = netId;
		event.Kind = 1;
		event.Reliable = reliable;
		event.Name = std::move(name);
		event.Args = std::move(args);
		TrimEvent(event);
		if (event.Name.empty())
			return;
		if (reliable)
		{
			const std::vector<uint8_t> bytes = PackReliable(event);
			if (bytes.empty())
			{
				GE_CORE_WARN("Reliable script event '{0}' did not fit", event.Name);
				return;
			}
			SendReliable(bytes.data(), static_cast<int>(bytes.size()));
			return;
		}
		World& world = State();
		if (world.Unreliable.size() >= 8)
		{
			GE_CORE_WARN("Dropped unreliable script event '{0}'", event.Name);
			return;
		}
		world.Unreliable.push_back(std::move(event));
	}

	void BroadcastShot(uint16_t netId, float muzzleX, float muzzleY, float muzzleZ,
		float dirX, float dirY, float dirZ, float speed, uint32_t tick)
	{
		if (netId == 0)
			return;
		WireEvent event;
		event.NetId = netId;
		event.Kind = 2;
		event.Mx = muzzleX;
		event.My = muzzleY;
		event.Mz = muzzleZ;
		event.Dx = dirX;
		event.Dy = dirY;
		event.Dz = dirZ;
		event.Speed = speed;
		event.Tick = tick;
		World& world = State();
		if (world.Unreliable.size() >= 8)
		{
			GE_CORE_WARN("Dropped a shot event");
			return;
		}
		world.Unreliable.push_back(std::move(event));
	}

	void IngestReliable(const uint8_t* data, int length)
	{
		if (!data || length < 2 || data[0] != 3)
			return;
		BitRead reader(data + 1, length - 1);
		WireEvent event;
		event.Reliable = true;
		if (!OneEventBits(reader, event) || !reader.Ok())
		{
			GE_CORE_WARN("Reliable script event did not decode");
			return;
		}
		HeldEvent held;
		held.Event = std::move(event);
		State().Held.push_back(std::move(held));
	}

}

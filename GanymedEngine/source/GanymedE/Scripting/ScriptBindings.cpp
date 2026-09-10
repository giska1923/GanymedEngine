#include "gepch.h"
#include "GanymedE/Scripting/ScriptBindings.h"
#include "GanymedE/Scripting/ScriptEngine.h"

#include "GanymedE/Assets/AssetPaths.h"
#include "GanymedE/Audio/AudioEngine.h"
#include "GanymedE/Core/Input.h"
#include "GanymedE/Core/KeyCodes.h"
#include "GanymedE/Core/MouseButtonCodes.h"
#include "GanymedE/ECS/System.h"
#include "GanymedE/Physics/PhysicsScene.h"
#include "GanymedE/Scene/Components.h"
#include "GanymedE/Scene/Entity.h"
#include "GanymedE/Scene/Scene.h"
#include "GanymedE/Scene/Systems/AudioSystem.h"
#include "GanymedE/Scene/Systems/PhysicsSystem.h"
#include "GanymedE/UI/UIEngine.h"

// The entire binding surface, in one file on purpose: scripts-src/types/ganymed.d.ts is the
// hand-written TypeScript mirror of it, and one file is one thing to keep in sync.
//
// THE RULE THAT MATTERS HERE: component data is bound BY VALUE, with explicit setters. Nothing
// ever hands Lua a reference into entt storage. Two independent reasons, either one sufficient:
//
//   1. TransformComponent is change-tracked. A write through a bound reference is invisible to the
//      change log, so TransformSystem never refreshes the world-transform cache and the entity
//      VISIBLY DOES NOT MOVE even though the component data changed. The setters below pair every
//      write with Scene::MarkChanged, which is the whole reason movement works at all.
//   2. A reference into a component pool dangles the moment that pool reallocates, and Lua has no
//      idea when that happens.
//
// Copying nine floats to avoid both is not a tradeoff worth agonising over at script call rates.

namespace GanymedE {

	namespace {

		// Every transform setter needs this. Returns null when no scene context is set - which
		// should not happen while scripts run, but a null check beats a crash in the editor.
		Scene* Context()
		{
			return ScriptEngine::GetSceneContext();
		}

		void MarkTransformChanged(Entity entity)
		{
			if (Scene* scene = Context())
				scene->MarkChanged<TransformComponent>(entity);
		}

		// The live Jolt world, or null outside play. Reached through the system
		// rather than held, because PhysicsScene exists only between play and stop.
		PhysicsScene* Physics()
		{
			Scene* scene = Context();
			if (!scene)
				return nullptr;

			PhysicsSystem* system = scene->Systems().Get<PhysicsSystem>();
			return system ? system->GetPhysicsScene() : nullptr;
		}

		ParticleEmitterComponent* ParticleEmitter(Entity& e)
		{
			return e.HasComponent<ParticleEmitterComponent>()
				? &e.GetComponent<ParticleEmitterComponent>()
				: nullptr;
		}

		template<typename T>
		T ParticleGet(Entity& e, T ParticleEmitterComponent::* field, T fallback)
		{
			if (ParticleEmitterComponent* p = ParticleEmitter(e))
				return p->*field;
			return fallback;
		}

		template<typename T>
		void ParticleSet(Entity& e, T ParticleEmitterComponent::* field, T value)
		{
			if (ParticleEmitterComponent* p = ParticleEmitter(e))
				p->*field = value;
		}

		// The five min/max fields became `RangeF` members when the emitter's inspector was made
		// generic. These two overloads exist so **every Lua name stays exactly what it was** -
		// `GetParticleLifetimeMin` still reads the same value it always did. A C++ refactor must
		// not silently rewrite a scripting API that shipped.
		float ParticleGet(Entity& e, RangeF ParticleEmitterComponent::* range,
			float RangeF::* half, float fallback)
		{
			if (ParticleEmitterComponent* p = ParticleEmitter(e))
				return (p->*range).*half;
			return fallback;
		}

		void ParticleSet(Entity& e, RangeF ParticleEmitterComponent::* range,
			float RangeF::* half, float value)
		{
			if (ParticleEmitterComponent* p = ParticleEmitter(e))
				(p->*range).*half = value;
		}

		// The system that owns every voice. Same shape as Physics() above, and for the same
		// reason: nothing outside AudioSystem may create or destroy a voice.
		AudioSystem* Audio()
		{
			Scene* scene = Context();
			return scene ? scene->Systems().Get<AudioSystem>() : nullptr;
		}

		void RegisterVec3(sol::state& lua)
		{
			lua.new_usertype<glm::vec3>("Vec3",
				sol::call_constructor, sol::constructors<
					glm::vec3(),
					glm::vec3(float),
					glm::vec3(float, float, float)>(),

				"x", &glm::vec3::x,
				"y", &glm::vec3::y,
				"z", &glm::vec3::z,

				sol::meta_function::addition,    [](const glm::vec3& a, const glm::vec3& b) { return a + b; },
				sol::meta_function::subtraction, [](const glm::vec3& a, const glm::vec3& b) { return a - b; },
				sol::meta_function::unary_minus, [](const glm::vec3& v) { return -v; },
				sol::meta_function::multiplication, sol::overload(
					[](const glm::vec3& v, float s) { return v * s; },
					[](float s, const glm::vec3& v) { return s * v; },
					[](const glm::vec3& a, const glm::vec3& b) { return a * b; }),
				sol::meta_function::equal_to, [](const glm::vec3& a, const glm::vec3& b) { return a == b; },
				sol::meta_function::to_string, [](const glm::vec3& v)
				{
					return "Vec3(" + std::to_string(v.x) + ", " + std::to_string(v.y)
						+ ", " + std::to_string(v.z) + ")";
				},

				"Length", [](const glm::vec3& v) { return glm::length(v); },
				// Guarded: glm::normalize of a zero vector is a division by zero, which reaches Lua
				// as a silent NaN rather than an error.
				"Normalized", [](const glm::vec3& v)
				{
					const float lengthSquared = glm::dot(v, v);
					return lengthSquared > 0.0f ? v / glm::sqrt(lengthSquared) : glm::vec3(0.0f);
				},
				"Dot",   [](const glm::vec3& a, const glm::vec3& b) { return glm::dot(a, b); },
				"Cross", [](const glm::vec3& a, const glm::vec3& b) { return glm::cross(a, b); }
			);
		}

		void RegisterEntity(sol::state& lua)
		{
			lua.new_usertype<Entity>("Entity",
				sol::no_constructor,   // entities come from the engine, never from `Entity()` in Lua

				"GetName", [](Entity& e) { return e.GetName(); },
				"GetUUID", [](Entity& e) { return static_cast<uint64_t>(e.GetUUID()); },
				"IsValid", [](Entity& e) { return static_cast<bool>(e); },

				// Direct children only. Scene.FindEntityByName is a global first-match; two
				// boxes both parenting a child tagged "Sparks" cannot use it.
				"GetChildByName", [](Entity& e, const std::string& name) -> sol::optional<Entity>
				{
					Scene* scene = Context();
					if (!scene || !e || !e.HasComponent<RelationshipComponent>())
						return sol::nullopt;

					for (UUID childID : e.GetComponent<RelationshipComponent>().Children)
					{
						Entity child = scene->FindEntityByUUID(childID);
						if (child && child.GetName() == name)
							return child;
					}
					return sol::nullopt;
				},

				// --- Transform: get a copy, mutate it, set it back ---
				"GetTranslation", [](Entity& e) { return e.GetComponent<TransformComponent>().Translation; },
				"SetTranslation", [](Entity& e, const glm::vec3& value)
				{
					e.GetComponent<TransformComponent>().Translation = value;
					MarkTransformChanged(e);
				},

				// Euler angles, in radians, matching TransformComponent's storage.
				"GetRotation", [](Entity& e) { return e.GetComponent<TransformComponent>().Rotation; },
				"SetRotation", [](Entity& e, const glm::vec3& value)
				{
					e.GetComponent<TransformComponent>().Rotation = value;
					MarkTransformChanged(e);
				},

				"GetScale", [](Entity& e) { return e.GetComponent<TransformComponent>().Scale; },
				"SetScale", [](Entity& e, const glm::vec3& value)
				{
					e.GetComponent<TransformComponent>().Scale = value;
					MarkTransformChanged(e);
				},

				"HasRigidBody",      [](Entity& e) { return e.HasComponent<RigidBodyComponent>(); },
				"HasAnimator",       [](Entity& e) { return e.HasComponent<AnimatorComponent>(); },
				"HasAudioSource",    [](Entity& e) { return e.HasComponent<AudioSourceComponent>(); },
				"HasParticleEmitter", [](Entity& e) { return e.HasComponent<ParticleEmitterComponent>(); },

				// --- Animation ---
				// AnimatorComponent is untracked, so unlike the transform setters these need no
				// MarkChanged: AnimationSystem reads the component every frame regardless.
				//
				// Every one of these no-ops on an entity without an animator rather than
				// asserting, matching the physics bindings. Entity::GetComponent asserts on a
				// missing component, and in Release that assert is gone and the read is UB.
				//
				// No clip-name validation here on purpose. AnimationSystem already warns once per
				// distinct bad name and holds the bind pose; a second check would report the same
				// typo twice, and it cannot even do so reliably because the mesh asset may not be
				// resident yet.
				"PlayAnimation", [](Entity& e, const std::string& name)
				{
					if (!e.HasComponent<AnimatorComponent>())
						return;

					auto& animator = e.GetComponent<AnimatorComponent>();

					// Restarting only on an actual clip CHANGE is what makes the obvious idiom
					// work: a script that calls PlayAnimation every frame from a branch
					// ("grounded and PlayAnimation('Idle') or PlayAnimation('Run')") would
					// otherwise pin Time at 0 forever, because the script systems run before
					// AnimationSystem and would undo each frame's advance. Re-playing the current
					// clip therefore resumes it; switching clips is the documented hard cut.
					if (animator.Clip != name)
					{
						animator.Clip = name;
						animator.Time = 0.0f;
					}

					animator.Playing = true;
				},
				// Freezes on the pose currently posed rather than rewinding to the clip start:
				// Playing = false only stops the clock, and AnimationSystem keeps building the
				// palette from the unchanged Time. Rewinding would snap the character on a call
				// that reads like "pause", and PlayAnimation already covers restart-from-zero.
				"StopAnimation", [](Entity& e)
				{
					if (e.HasComponent<AnimatorComponent>())
						e.GetComponent<AnimatorComponent>().Playing = false;
				},
				// Time multiplier. Negative rewinds; 0 freezes without clearing Playing.
				"SetAnimationSpeed", [](Entity& e, float speed)
				{
					if (e.HasComponent<AnimatorComponent>())
						e.GetComponent<AnimatorComponent>().Speed = speed;
				},
				"SetAnimationLooping", [](Entity& e, bool loop)
				{
					if (e.HasComponent<AnimatorComponent>())
						e.GetComponent<AnimatorComponent>().Loop = loop;
				},
				"IsAnimationPlaying", [](Entity& e)
				{
					return e.HasComponent<AnimatorComponent>()
						&& e.GetComponent<AnimatorComponent>().Playing;
				},
				// The name the animator is set to, which is not proof the mesh has a clip by that
				// name - an unresolved reference keeps its name and holds the bind pose.
				"GetCurrentAnimation", [](Entity& e)
				{
					return e.HasComponent<AnimatorComponent>()
						? e.GetComponent<AnimatorComponent>().Clip
						: std::string{};
				},

				// --- Physics: routed through PhysicsScene, never through transforms ---
				// Writing the transform of a dynamic body does nothing visible:
				// SyncTransforms overwrites it from the simulation every step. These go
				// to Jolt, so kinematic and dynamic bodies behave as their type says.
				// All no-op when the entity has no body or play is not running.
				"GetLinearVelocity", [](Entity& e)
				{
					PhysicsScene* physics = Physics();
					return physics ? physics->GetLinearVelocity(e.GetUUID()) : glm::vec3(0.0f);
				},
				"SetLinearVelocity", [](Entity& e, const glm::vec3& velocity)
				{
					if (PhysicsScene* physics = Physics())
						physics->SetLinearVelocity(e.GetUUID(), velocity);
				},
				// One-shot change in momentum.
				"AddImpulse", [](Entity& e, const glm::vec3& impulse)
				{
					if (PhysicsScene* physics = Physics())
						physics->AddImpulse(e.GetUUID(), impulse);
				},
				// Consumed by the next step; call it every frame while the push lasts.
				"AddForce", [](Entity& e, const glm::vec3& force)
				{
					if (PhysicsScene* physics = Physics())
						physics->AddForce(e.GetUUID(), force);
				},

				// --- Audio ---
				// Split across two routes, and the split is not arbitrary. Play/Stop/IsPlaying
				// touch the live VOICE, which AudioSystem owns, so they go through the system -
				// the physics-binding shape. Volume/Pitch/Looping are AUTHORED fields the
				// system re-pushes every frame, so they write the component directly - the
				// animation-binding shape, and no MarkChanged because AudioSourceComponent is
				// untracked. Writing the voice for those instead would be silently undone by
				// the next update.
				//
				// PlaySound on an already-playing source is a no-op, not a restart: scripts
				// call this every frame from a branch, and restarting each tick would hold the
				// sound at its first sample forever (LuaScriptSystem runs before AudioSystem).
				// Restart is StopSound() then PlaySound().
				"PlaySound", [](Entity& e)
				{
					if (AudioSystem* audio = Audio())
						audio->PlaySound(static_cast<entt::entity>(e));
				},
				// Pause, keeping the position - PlaySound resumes from it.
				"StopSound", [](Entity& e)
				{
					if (AudioSystem* audio = Audio())
						audio->StopSound(static_cast<entt::entity>(e));
				},
				"IsSoundPlaying", [](Entity& e)
				{
					AudioSystem* audio = Audio();
					return audio && audio->IsSoundPlaying(static_cast<entt::entity>(e));
				},
				"SetSoundVolume", [](Entity& e, float volume)
				{
					if (e.HasComponent<AudioSourceComponent>())
						e.GetComponent<AudioSourceComponent>().Volume = volume;
				},
				"SetSoundPitch", [](Entity& e, float pitch)
				{
					if (e.HasComponent<AudioSourceComponent>())
						e.GetComponent<AudioSourceComponent>().Pitch = pitch;
				},
				"SetSoundLooping", [](Entity& e, bool loop)
				{
					if (e.HasComponent<AudioSourceComponent>())
						e.GetComponent<AudioSourceComponent>().Loop = loop;
				},

				// --- Particles ---
				// Component-direct, the animation shape: the pool lives on
				// ParticleEmitterComponent (not a system-owned voice). ParticleEmitterComponent
				// is untracked — ParticleSystem and RenderSystem poll it every frame — so these
				// writes need no MarkChanged. No-op without the component.
				//
				// PlayParticles on an already-playing emitter is a no-op, not a restart: scripts
				// call this every frame from a branch, and LuaScriptSystem runs before
				// ParticleSystem. A restart would ResetRuntime 60×/sec. EmitBurst accumulates
				// (two calls in one frame = one bigger burst) and does not auto-play; consume
				// happens this tick while Playing, even past Duration. Curves and assets are
				// not scriptable in v1.
				"PlayParticles", [](Entity& e)
				{
					if (ParticleEmitterComponent* p = ParticleEmitter(e))
						p->PlayPreview(e.GetUUID());
				},
				"StopParticles", [](Entity& e)
				{
					if (ParticleEmitterComponent* p = ParticleEmitter(e))
						p->StopPreview();
				},
				"IsParticlesPlaying", [](Entity& e)
				{
					if (ParticleEmitterComponent* p = ParticleEmitter(e))
						return p->Playing;
					return false;
				},
				"EmitBurst", [](Entity& e, double count)
				{
					// double, not int: ScriptComponent properties are always Lua floats
					// (the "all numbers are floats" rule). sol2 rejects a 24.0 into int.
					if (count <= 0.0)
						return;
					if (ParticleEmitterComponent* p = ParticleEmitter(e))
						p->BurstPending += static_cast<uint32_t>(count);
				},

				"GetParticleRateOverTime", [](Entity& e) { return ParticleGet(e, &ParticleEmitterComponent::RateOverTime, 0.0f); },
				"SetParticleRateOverTime", [](Entity& e, float v) { ParticleSet(e, &ParticleEmitterComponent::RateOverTime, v); },
				"GetParticleMaxParticles", [](Entity& e) -> int
				{
					if (ParticleEmitterComponent* p = ParticleEmitter(e))
						return static_cast<int>(p->MaxParticles);
					return 0;
				},
				"SetParticleMaxParticles", [](Entity& e, double v)
				{
					if (v < 0.0)
						return;
					if (ParticleEmitterComponent* p = ParticleEmitter(e))
						p->MaxParticles = static_cast<uint32_t>(v);
				},
				"GetParticleLooping", [](Entity& e) { return ParticleGet(e, &ParticleEmitterComponent::Looping, false); },
				"SetParticleLooping", [](Entity& e, bool v) { ParticleSet(e, &ParticleEmitterComponent::Looping, v); },
				"GetParticleDuration", [](Entity& e) { return ParticleGet(e, &ParticleEmitterComponent::Duration, 0.0f); },
				"SetParticleDuration", [](Entity& e, float v) { ParticleSet(e, &ParticleEmitterComponent::Duration, v); },
				"GetParticlePlayOnStart", [](Entity& e) { return ParticleGet(e, &ParticleEmitterComponent::PlayOnStart, false); },
				"SetParticlePlayOnStart", [](Entity& e, bool v) { ParticleSet(e, &ParticleEmitterComponent::PlayOnStart, v); },
				"GetParticleLifetimeMin", [](Entity& e) { return ParticleGet(e, &ParticleEmitterComponent::Lifetime, &RangeF::Min, 0.0f); },
				"SetParticleLifetimeMin", [](Entity& e, float v) { ParticleSet(e, &ParticleEmitterComponent::Lifetime, &RangeF::Min, v); },
				"GetParticleLifetimeMax", [](Entity& e) { return ParticleGet(e, &ParticleEmitterComponent::Lifetime, &RangeF::Max, 0.0f); },
				"SetParticleLifetimeMax", [](Entity& e, float v) { ParticleSet(e, &ParticleEmitterComponent::Lifetime, &RangeF::Max, v); },
				"GetParticleSpeedMin", [](Entity& e) { return ParticleGet(e, &ParticleEmitterComponent::Speed, &RangeF::Min, 0.0f); },
				"SetParticleSpeedMin", [](Entity& e, float v) { ParticleSet(e, &ParticleEmitterComponent::Speed, &RangeF::Min, v); },
				"GetParticleSpeedMax", [](Entity& e) { return ParticleGet(e, &ParticleEmitterComponent::Speed, &RangeF::Max, 0.0f); },
				"SetParticleSpeedMax", [](Entity& e, float v) { ParticleSet(e, &ParticleEmitterComponent::Speed, &RangeF::Max, v); },
				"GetParticleConeAngle", [](Entity& e) { return ParticleGet(e, &ParticleEmitterComponent::ConeAngle, 0.0f); },
				"SetParticleConeAngle", [](Entity& e, float v) { ParticleSet(e, &ParticleEmitterComponent::ConeAngle, v); },
				"GetParticleStartSizeMin", [](Entity& e) { return ParticleGet(e, &ParticleEmitterComponent::StartSize, &RangeF::Min, 0.0f); },
				"SetParticleStartSizeMin", [](Entity& e, float v) { ParticleSet(e, &ParticleEmitterComponent::StartSize, &RangeF::Min, v); },
				"GetParticleStartSizeMax", [](Entity& e) { return ParticleGet(e, &ParticleEmitterComponent::StartSize, &RangeF::Max, 0.0f); },
				"SetParticleStartSizeMax", [](Entity& e, float v) { ParticleSet(e, &ParticleEmitterComponent::StartSize, &RangeF::Max, v); },
				"GetParticleStartRotationMin", [](Entity& e) { return ParticleGet(e, &ParticleEmitterComponent::StartRotation, &RangeF::Min, 0.0f); },
				"SetParticleStartRotationMin", [](Entity& e, float v) { ParticleSet(e, &ParticleEmitterComponent::StartRotation, &RangeF::Min, v); },
				"GetParticleStartRotationMax", [](Entity& e) { return ParticleGet(e, &ParticleEmitterComponent::StartRotation, &RangeF::Max, 0.0f); },
				"SetParticleStartRotationMax", [](Entity& e, float v) { ParticleSet(e, &ParticleEmitterComponent::StartRotation, &RangeF::Max, v); },
				"GetParticleRotationSpeedMin", [](Entity& e) { return ParticleGet(e, &ParticleEmitterComponent::RotationSpeed, &RangeF::Min, 0.0f); },
				"SetParticleRotationSpeedMin", [](Entity& e, float v) { ParticleSet(e, &ParticleEmitterComponent::RotationSpeed, &RangeF::Min, v); },
				"GetParticleRotationSpeedMax", [](Entity& e) { return ParticleGet(e, &ParticleEmitterComponent::RotationSpeed, &RangeF::Max, 0.0f); },
				"SetParticleRotationSpeedMax", [](Entity& e, float v) { ParticleSet(e, &ParticleEmitterComponent::RotationSpeed, &RangeF::Max, v); },
				"GetParticleGravityModifier", [](Entity& e) { return ParticleGet(e, &ParticleEmitterComponent::GravityModifier, 0.0f); },
				"SetParticleGravityModifier", [](Entity& e, float v) { ParticleSet(e, &ParticleEmitterComponent::GravityModifier, v); },
				"GetParticleWorldSpace", [](Entity& e) { return ParticleGet(e, &ParticleEmitterComponent::WorldSpace, false); },
				"SetParticleWorldSpace", [](Entity& e, bool v) { ParticleSet(e, &ParticleEmitterComponent::WorldSpace, v); },
				"GetParticleSeed", [](Entity& e) -> int
				{
					if (ParticleEmitterComponent* p = ParticleEmitter(e))
						return static_cast<int>(p->Seed);
					return 0;
				},
				"SetParticleSeed", [](Entity& e, double v)
				{
					if (v < 0.0)
						return;
					if (ParticleEmitterComponent* p = ParticleEmitter(e))
						p->Seed = static_cast<uint32_t>(v);
				},

				sol::meta_function::equal_to, [](const Entity& a, const Entity& b) { return a == b; },
				sol::meta_function::to_string, [](Entity& e) { return "Entity(" + e.GetName() + ")"; }
			);
		}

		void RegisterInput(sol::state& lua)
		{
			sol::table input = lua.create_named_table("Input");
			input["IsKeyPressed"]         = [](int key) { return Input::IsKeyPressed(static_cast<KeyCode>(key)); };
			input["IsMouseButtonPressed"] = [](int button) { return Input::IsMouseButtonPressed(static_cast<MouseCode>(button)); };
			// Two returns rather than a Vec3: mouse position is 2D, and TSTL models this as
			// LuaMultiReturn<[number, number]>.
			input["GetMousePosition"]     = []() { const glm::vec2 p = Input::GetMousePosition(); return std::make_tuple(p.x, p.y); };
		}

		void RegisterKeyCodes(sol::state& lua)
		{
			sol::table key = lua.create_named_table("Key");

			auto bind = [&key](const char* name, KeyCode code) { key[name] = static_cast<int>(code); };

			bind("Space", Key::Space);       bind("Apostrophe", Key::Apostrophe);
			bind("Comma", Key::Comma);       bind("Minus", Key::Minus);
			bind("Period", Key::Period);     bind("Slash", Key::Slash);
			bind("Semicolon", Key::Semicolon); bind("Equal", Key::Equal);

			bind("D0", Key::D0); bind("D1", Key::D1); bind("D2", Key::D2); bind("D3", Key::D3);
			bind("D4", Key::D4); bind("D5", Key::D5); bind("D6", Key::D6); bind("D7", Key::D7);
			bind("D8", Key::D8); bind("D9", Key::D9);

			bind("A", Key::A); bind("B", Key::B); bind("C", Key::C); bind("D", Key::D);
			bind("E", Key::E); bind("F", Key::F); bind("G", Key::G); bind("H", Key::H);
			bind("I", Key::I); bind("J", Key::J); bind("K", Key::K); bind("L", Key::L);
			bind("M", Key::M); bind("N", Key::N); bind("O", Key::O); bind("P", Key::P);
			bind("Q", Key::Q); bind("R", Key::R); bind("S", Key::S); bind("T", Key::T);
			bind("U", Key::U); bind("V", Key::V); bind("W", Key::W); bind("X", Key::X);
			bind("Y", Key::Y); bind("Z", Key::Z);

			bind("LeftBracket", Key::LeftBracket);   bind("Backslash", Key::Backslash);
			bind("RightBracket", Key::RightBracket); bind("GraveAccent", Key::GraveAccent);

			bind("Escape", Key::Escape);       bind("Enter", Key::Enter);
			bind("Tab", Key::Tab);             bind("Backspace", Key::Backspace);
			bind("Insert", Key::Insert);       bind("Delete", Key::Delete);
			bind("Right", Key::Right);         bind("Left", Key::Left);
			bind("Down", Key::Down);           bind("Up", Key::Up);
			bind("PageUp", Key::PageUp);       bind("PageDown", Key::PageDown);
			bind("Home", Key::Home);           bind("End", Key::End);
			bind("CapsLock", Key::CapsLock);   bind("ScrollLock", Key::ScrollLock);
			bind("NumLock", Key::NumLock);     bind("PrintScreen", Key::PrintScreen);
			bind("Pause", Key::Pause);

			bind("F1", Key::F1);   bind("F2", Key::F2);   bind("F3", Key::F3);
			bind("F4", Key::F4);   bind("F5", Key::F5);   bind("F6", Key::F6);
			bind("F7", Key::F7);   bind("F8", Key::F8);   bind("F9", Key::F9);
			bind("F10", Key::F10); bind("F11", Key::F11); bind("F12", Key::F12);

			bind("LeftShift", Key::LeftShift);     bind("LeftControl", Key::LeftControl);
			bind("LeftAlt", Key::LeftAlt);         bind("LeftSuper", Key::LeftSuper);
			bind("RightShift", Key::RightShift);   bind("RightControl", Key::RightControl);
			bind("RightAlt", Key::RightAlt);       bind("RightSuper", Key::RightSuper);
			bind("Menu", Key::Menu);

			sol::table mouse = lua.create_named_table("Mouse");
			mouse["ButtonLeft"]   = static_cast<int>(Mouse::ButtonLeft);
			mouse["ButtonRight"]  = static_cast<int>(Mouse::ButtonRight);
			mouse["ButtonMiddle"] = static_cast<int>(Mouse::ButtonMiddle);
		}

		void RegisterLog(sol::state& lua)
		{
			// The CLIENT logger, not the core one: script output is game output, and keeping the
			// two apart is what lets the core log stay readable.
			sol::table log = lua.create_named_table("Log");
			log["Trace"] = [](const std::string& message) { GE_TRACE("{0}", message); };
			log["Info"]  = [](const std::string& message) { GE_INFO("{0}", message); };
			log["Warn"]  = [](const std::string& message) { GE_WARN("{0}", message); };
			log["Error"] = [](const std::string& message) { GE_ERROR("{0}", message); };
		}

		void RegisterScene(sol::state& lua)
		{
			sol::table scene = lua.create_named_table("Scene");

			// Linear scan over TagComponent. Fine at the rate scripts actually call this (setup,
			// not per-frame); if that stops being true the answer is a name index on Scene, not a
			// cleverer binding.
			// sol::optional rather than sol::object: sol2 maps nullopt to nil on the way out, so a
			// miss reads as `if entity then` in Lua without any state plumbing here.
			scene["FindEntityByName"] = [](const std::string& name) -> sol::optional<Entity>
			{
				Scene* context = Context();
				if (!context)
					return sol::nullopt;

				auto view = context->Reg().view<TagComponent>();
				for (auto entity : view)
				{
					if (view.get<TagComponent>(entity).Tag == name)
						return Entity{ entity, context };
				}
				return sol::nullopt;
			};
		}

		void RegisterAudio(sol::state& lua)
		{
			// Deliberately four functions. Per-entity sound is the Entity methods above; this
			// table is for the two things that have no entity to hang off - fire-and-forget
			// one-shots and the mixer.
			//
			// No clip swapping (author it on the component) and no VoiceId in Lua: a script
			// holding a handle to a live engine resource is a lifetime problem the engine
			// would then have to police.
			sol::table audio = lua.create_named_table("Audio");

			// Paths are relative to assets/, the same currency the asset registry and the
			// content browser speak. A one-shot needs no registry entry - AudioEngine takes
			// paths, which is the point of Audio being path-resolved (docs/engine/audio.md).
			//
			// The positional overload takes a Vec3 rather than three floats, because every
			// other position in these bindings is a Vec3 and the common call is
			// Audio.PlayOneShot(path, entity:GetTranslation()).
			audio["PlayOneShot"] = sol::overload(
				[](const std::string& path)
				{
					AudioEngine::PlayOneShot(GetAssetRoot() / path, AudioGroup::SFX, nullptr, 1.0f);
				},
				[](const std::string& path, const glm::vec3& position)
				{
					AudioEngine::PlayOneShot(GetAssetRoot() / path, AudioGroup::SFX, &position, 1.0f);
				});

			audio["SetMasterVolume"] = [](float volume)
			{
				AudioEngine::SetGroupVolume(AudioGroup::Master, volume);
			};

			// "Master" | "Music" | "SFX". An unknown name warns and falls back to SFX rather
			// than throwing - AudioGroupFromString is the same parser the scene serializer uses.
			audio["SetGroupVolume"] = [](const std::string& group, float volume)
			{
				AudioEngine::SetGroupVolume(AudioGroupFromString(group), volume);
			};
		}

		void RegisterUI(sol::state& lua)
		{
			// The gameplay-facing half of the RmlUi data model. Setting a value here
			// writes the bound C++ variable and marks it dirty, which is what makes
			// RmlUi re-evaluate the {{expressions}} referencing it.
			//
			// Fixed setters rather than UI.Set(name, value): RmlUi data models bind
			// to real C++ addresses declared before any document loads, so a generic
			// bag would need a different mechanism entirely (see docs/engine/ui.md).
			sol::table ui = lua.create_named_table("UI");
			ui["SetHealth"] = [](float health) { UIEngine::SetHudHealth(health); };
			ui["SetScore"]  = [](int score)    { UIEngine::SetHudScore(score); };
			ui["GetHealth"] = []() { return UIEngine::GetHudHealth(); };
			ui["GetScore"]  = []() { return UIEngine::GetHudScore(); };
		}
	}

	void RegisterScriptGlobals(sol::state& lua)
	{
		RegisterInput(lua);
		RegisterKeyCodes(lua);
		RegisterLog(lua);
		RegisterScene(lua);
		RegisterAudio(lua);
		RegisterUI(lua);
	}

	void RegisterScriptBindings(sol::state& lua)
	{
		// Usertypes first, then the tables. Only the tables are re-installable;
		// re-registering a usertype would rebuild metatables live objects point at.
		RegisterVec3(lua);
		RegisterEntity(lua);
		RegisterScriptGlobals(lua);
	}
}

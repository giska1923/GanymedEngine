#include "gepch.h"
#include "GanymedE/Scripting/ScriptBindings.h"
#include "GanymedE/Scripting/ScriptEngine.h"

#include "GanymedE/Core/Input.h"
#include "GanymedE/Core/KeyCodes.h"
#include "GanymedE/Core/MouseButtonCodes.h"
#include "GanymedE/ECS/System.h"
#include "GanymedE/Physics/PhysicsScene.h"
#include "GanymedE/Scene/Components.h"
#include "GanymedE/Scene/Entity.h"
#include "GanymedE/Scene/Scene.h"
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

				"HasRigidBody", [](Entity& e) { return e.HasComponent<RigidBodyComponent>(); },

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

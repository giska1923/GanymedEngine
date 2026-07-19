#pragma once

#include "GanymedE/Assets/AssetTypes.h"
#include "GanymedE/Core/Timestep.h"

#include <entt/entt.hpp>

namespace GanymedE {

	class Entity;
	class Scene;

	// Owner of the one Lua VM the whole runtime shares.
	//
	// Split of responsibility with LuaScriptSystem: the system owns *when* things happen (it reads
	// the reactive views and calls in here); ScriptEngine owns *what* happens (the sol::state, the
	// class table per script asset, the instance table per entity). The system's header therefore
	// stays free of sol2, and so does this one - every sol:: type is confined to Scripting/*.cpp,
	// because sol2's templates are expensive enough that leaking them into a widely-included header
	// is measurable in build time.
	//
	// One VM, not one per scene: RmlUi's Lua plugin will run on this same lua_State (see
	// GetLuaState), so UI and gameplay scripts share globals and can call each other.
	class ScriptEngine
	{
	public:
		static void Init();
		static void Shutdown();

		static bool IsInitialized();

		// Which scene the bindings resolve entities against. Set by LuaScriptSystem before it
		// touches anything here, because the editor holds two Scenes (the edit scene and the
		// play-mode copy) and each has its own LuaScriptSystem.
		static void SetSceneContext(Scene* scene);
		static Scene* GetSceneContext();

		// Idempotent: LuaScriptSystem::OnRuntimeStart sweeps every live script, and the InitView's
		// first-ever read then reports those same entities again.
		static void Instantiate(Entity entity, AssetHandle script);

		// Takes the raw id rather than an Entity because a Fini event may mean the entity is
		// already destroyed - there is nothing left to resolve a UUID from. See s_EntityToUUID.
		static void DestroyInstance(entt::entity entity);
		static void DestroyAllInstances();

		static void Update(entt::entity entity, Timestep ts);

		static void OnCollisionEnter(Entity entity, Entity other);
		static void OnCollisionExit(Entity entity, Entity other);

		// Raw lua_State*, for initialising the RmlUi Lua plugin on the shared VM.
		// Returned as void* so this header need not pull in the Lua C headers.
		static void* GetLuaState();
	};
}

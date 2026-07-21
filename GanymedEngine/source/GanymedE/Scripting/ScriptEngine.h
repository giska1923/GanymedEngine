#pragma once

#include "GanymedE/Assets/AssetTypes.h"
#include "GanymedE/Core/Timestep.h"
#include "GanymedE/Scene/Components.h"

#include <entt/entt.hpp>

#include <string>
#include <vector>

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
		//
		// Takes the whole component rather than just the handle because the per-entity property
		// overrides live on it, and they have to be applied before OnCreate runs.
		static void Instantiate(Entity entity, const ScriptComponent& component);

		// Takes the raw id rather than an Entity because a Fini event may mean the entity is
		// already destroyed - there is nothing left to resolve a UUID from. See s_EntityToUUID.
		static void DestroyInstance(entt::entity entity);

		// Tears down one scene's instances. Scoped rather than global because Scene::~Scene calls
		// OnRuntimeStop, and the editor routinely holds several Scenes at once - a throwaway one
		// being destroyed must not take the playing scene's instances with it.
		static void DestroySceneInstances(Scene* scene);

		// Every scene's. Only for engine shutdown.
		static void DestroyAllInstances();

		static void Update(entt::entity entity, Timestep ts);

		// One tunable declared by a script, with the default it declared.
		//
		// The type is carried by the variant rather than named separately: a script writes
		// `Properties = { speed = 5.0 }`, and the default's Lua type IS the declaration. One
		// source of truth, and nothing to keep in sync.
		struct ScriptField
		{
			std::string Name;
			ScriptFieldValue Default;
		};

		// The fields a script declares, sorted by name, or empty if it declares none.
		//
		// Usable in edit mode, before anything is instantiated - that is the whole point, since
		// the inspector has to draw these with no runtime scene. Note the cost: answering this
		// EXECUTES the script's top-level chunk (cached afterwards), so a script doing real work
		// at file scope does that work in the editor. Lifecycle methods are not called.
		static const std::vector<ScriptField>& GetScriptFields(AssetHandle script);

		// Re-runs any script whose file changed on disk and re-points live instances at
		// the new methods, keeping their accumulated state. Throttled internally; call
		// once per update. Play-stop already re-reads everything, so this only matters
		// for editing a script WHILE the game runs.
		static void PollHotReload(Timestep ts);

		static void OnCollisionEnter(Entity entity, Entity other);
		static void OnCollisionExit(Entity entity, Entity other);

		// Raw lua_State*, for initialising the RmlUi Lua plugin on the shared VM.
		// Returned as void* so this header need not pull in the Lua C headers.
		static void* GetLuaState();

		// Re-installs the engine's global tables, letting them win a name collision.
		//
		// The price of one shared VM: RmlUi's Lua plugin registers globals of its own,
		// and one is called `Log` - so `Rml::Lua::Initialise` running after
		// ScriptEngine::Init silently replaced the engine's Log with RmlUi's, whose
		// interface is Log.Message(Log.logtype.info, ...). Every script calling
		// Log.Info then died on "attempt to call a nil value". UIEngine calls this
		// straight after loading the plugin so engine names take precedence.
		static void ReinstallGlobals();
	};
}

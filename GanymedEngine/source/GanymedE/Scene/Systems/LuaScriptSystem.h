#pragma once

#include "GanymedE/ECS/System.h"
#include "GanymedE/ECS/Views.h"
#include "GanymedE/Scene/Components.h"

namespace GanymedE {

	// Drives the lifecycle of Lua script instances, mirroring NativeScriptSystem almost exactly.
	// The instances themselves live in ScriptEngine, keyed by UUID; this system owns only the
	// timing, which is why the header stays free of sol2 (see ScriptEngine.h for why that matters).
	class LuaScriptSystem : public ECS::System<LuaScriptSystem>
	{
	public:
		// ReactRO, not ReactRW: FiniView statically requires ReadTypes == ReactTypes ("may only
		// access its React types"), because on an entity destruction every other component is
		// already gone. RO is all that is wanted anyway - the buried handle is read for logging.
		using ScriptInitView = ECS::InitView<ECS::EntityId, ECS::ReactRO<ScriptComponent>>;
		using ScriptFiniView = ECS::FiniView<ECS::EntityId, ECS::ReactRO<ScriptComponent>>;
		using ScriptView     = ECS::IterView<ECS::EntityId, ECS::RO<ScriptComponent>>;

		// Never accessed through this view. It is declared so ValidateOrdering knows script code
		// writes transforms (through the bindings, outside any view) and that TransformSystem must
		// therefore run after this system. Same trick PhysicsSystem uses for SyncTransforms.
		using TransformWrite = ECS::AccessView<ECS::RW<TransformComponent>>;

		using Views = TypeList<ScriptInitView, ScriptFiniView, ScriptView, TransformWrite>;

		using ECS::System<LuaScriptSystem>::System;

		void OnRuntimeStart() override;
		void OnUpdate(Timestep ts) override;
		void OnUpdateEditor(Timestep ts) override;
		void OnRuntimeStop() override;
		const char* Name() const override { return "LuaScriptSystem"; }

	private:
		// Reactive views must be read every update or their events are lost, so both the runtime
		// and editor paths drain them. Only the runtime path instantiates what it finds.
		void DrainReactiveViews(bool instantiate);

		// The editor holds two Scenes - the edit scene and the play-mode copy - each with its own
		// LuaScriptSystem, while ScriptEngine is a single global VM. Pointing the engine at this
		// system's scene before every call keeps the bindings resolving against the right one.
		void BindSceneContext();
	};
}

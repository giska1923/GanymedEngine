#pragma once

#include "GanymedE/ECS/System.h"
#include "GanymedE/ECS/Views.h"
#include "GanymedE/Scene/Components.h"

namespace GanymedE {

	// Owns the lifecycle of native script instances: creation on play (or lazily on first update
	// for entities that gained a script mid-run), per-frame OnUpdate, and destruction on stop.
	// Phase 6 replaces the "instantiate if not yet instantiated" check with an InitView, and the
	// teardown loop with a FiniView.
	class NativeScriptSystem : public ECS::System<NativeScriptSystem>
	{
	public:
		// NativeScriptComponent is untracked, so the RW slot is a plain reference.
		using ScriptView = ECS::IterView<ECS::EntityId, ECS::RW<NativeScriptComponent>>;
		using Views = TypeList<ScriptView>;

		using ECS::System<NativeScriptSystem>::System;

		void OnRuntimeStart() override;
		void OnUpdate(Timestep ts) override;
		void OnRuntimeStop() override;

	private:
		void InstantiateScripts();
	};
}

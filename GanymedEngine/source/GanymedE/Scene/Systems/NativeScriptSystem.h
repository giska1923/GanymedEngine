#pragma once

#include "GanymedE/ECS/System.h"
#include "GanymedE/ECS/Views.h"
#include "GanymedE/Scene/Components.h"

namespace GanymedE {

	// Owns the lifecycle of native script instances.
	//
	// Creation and teardown are expressed declaratively rather than mirrored by hand in several
	// places (audit item A9): InitView reports scripts that appeared since the last update,
	// FiniView reports scripts that went away — including ones whose entity was destroyed, which
	// the old code missed entirely, leaking the instance and never calling OnDestroy.
	class NativeScriptSystem : public ECS::System<NativeScriptSystem>
	{
	public:
		// NativeScriptComponent is untracked, so the RW slots are plain references.
		using ScriptInitView = ECS::InitView<ECS::EntityId, ECS::ReactRW<NativeScriptComponent>>;
		using ScriptFiniView = ECS::FiniView<ECS::EntityId, ECS::ReactRW<NativeScriptComponent>>;
		using ScriptView     = ECS::IterView<ECS::EntityId, ECS::RW<NativeScriptComponent>>;

		using Views = TypeList<ScriptInitView, ScriptFiniView, ScriptView>;

		using ECS::System<NativeScriptSystem>::System;

		void OnRuntimeStart() override;
		void OnUpdate(Timestep ts) override;
		void OnUpdateEditor(Timestep ts) override;
		void OnRuntimeStop() override;

	private:
		void InstantiateScript(entt::entity entity, NativeScriptComponent& script);
		static void DestroyScript(NativeScriptComponent& script);

		// Reactive views must be read every update or their events are lost, so both the runtime
		// and editor paths drain them. Only the runtime path acts on what it finds.
		void DrainReactiveViews(bool instantiate);
	};
}

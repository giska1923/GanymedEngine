#pragma once

#include "GanymedE/ECS/System.h"
#include "GanymedE/ECS/Views.h"
#include "GanymedE/Physics/PhysicsScene.h"
#include "GanymedE/Scene/Components.h"

namespace GanymedE {

	// Owns the Jolt scene, the fixed-timestep accumulator, and collision event dispatch — all of
	// which used to be inline in Scene::OnUpdateRuntime.
	//
	// PhysicsScene itself still walks the registry directly; converting its internals to views is
	// optional and deliberately not part of this phase.
	class PhysicsSystem : public ECS::System<PhysicsSystem>
	{
	public:
		// Collision dispatch needs random access to script instances by entity, not iteration.
		using ScriptAccess = ECS::AccessView<ECS::RW<NativeScriptComponent>>;

		// PhysicsScene::SyncTransforms writes TransformComponent directly, outside any view.
		// Declaring it here keeps the access metadata honest, so ordering validation knows
		// TransformSystem has to run after this system rather than before it.
		using TransformWrite = ECS::AccessView<ECS::RW<TransformComponent>>;

		using Views = TypeList<ScriptAccess, TransformWrite>;

		using ECS::System<PhysicsSystem>::System;

		void OnRuntimeStart() override;
		void OnUpdate(Timestep ts) override;
		void OnRuntimeStop() override;
		const char* Name() const override { return "PhysicsSystem"; }

		// Exposed for the renderer's Jolt debug view; Phase 7 should route this through a
		// PhysicsSettings/PhysicsState singleton instead of a direct system lookup.
		PhysicsScene* GetPhysicsScene() { return m_PhysicsScene.get(); }

	private:
		void DispatchCollisionEvents();

		Scope<PhysicsScene> m_PhysicsScene;
		float m_Accumulator = 0.0f;
		// Fixed timestep and the step cap now live in the PhysicsSettings singleton.
	};
}

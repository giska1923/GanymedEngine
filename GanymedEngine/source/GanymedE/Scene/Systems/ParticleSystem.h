#pragma once

#include "GanymedE/ECS/System.h"
#include "GanymedE/ECS/Views.h"
#include "GanymedE/Scene/Components.h"

namespace GanymedE {

	// CPU particle simulation. Pool + RNG live on ParticleEmitterComponent (the Palette
	// posture, extended: state persists across frames, so Scene::Copy resets it and play
	// mode warms up from empty).
	//
	// Editor preview ticks every emitter, always. That diverges from both neighbours on
	// purpose, and the divergence is the point:
	//   - AnimationSystem samples-without-advancing. Impossible here: a spawn/age sim has
	//     no closed form to sample at time T.
	//   - AudioSystem is silent in edit. Defensible for sound, wrong for the thing being
	//     visually authored — a curve editor over an invisible effect authors blind.
	// Unity ticks particles in the scene view; tick-all is the cheap end of that norm.
	// Playing=false (inspector Stop, Phase 4) is how authors get quiet.
	class ParticleSystem : public ECS::System<ParticleSystem>
	{
	public:
		using EmitterView = ECS::IterView<ECS::EntityId,
			ECS::RW<ParticleEmitterComponent>, ECS::RO<WorldTransformComponent>>;

		using Views = TypeList<EmitterView>;

		using ECS::System<ParticleSystem>::System;

		void OnUpdate(Timestep ts) override;
		void OnUpdateEditor(Timestep ts) override;
		const char* Name() const override { return "ParticleSystem"; }

	private:
		void Tick(Timestep ts);
		void TickEmitter(entt::entity entity, ParticleEmitterComponent& emitter,
			const glm::mat4& world, float dt);
	};

}

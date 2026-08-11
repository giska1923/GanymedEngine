#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "GanymedE/ECS/System.h"
#include "GanymedE/ECS/Views.h"
#include "GanymedE/Renderer/Animation.h"
#include "GanymedE/Scene/Components.h"

namespace GanymedE {

	class Mesh;

	// Samples each animator's clip and leaves a joint palette on its AnimatorComponent for
	// RenderSystem to upload.
	//
	// Unlike every other simulating system, this one also runs in edit mode - but it *samples*
	// without *advancing*. Scrubbing Time in the inspector has to move the model, which needs
	// evaluation; running the clock would leave every rigged model in the scene permanently in
	// motion while you are trying to place things. Time therefore only advances in play mode.
	class AnimationSystem : public ECS::System<AnimationSystem>
	{
	public:
		using AnimView = ECS::IterView<ECS::EntityId,
			ECS::RW<AnimatorComponent>, ECS::RO<StaticMeshComponent>>;

		using Views = TypeList<AnimView>;

		using ECS::System<AnimationSystem>::System;

		void OnRuntimeStart() override;
		void OnUpdate(Timestep ts) override;
		void OnUpdateEditor(Timestep ts) override;
		const char* Name() const override { return "AnimationSystem"; }

	private:
		void Evaluate(Timestep ts, bool advanceTime);

		// Resolves the clip, or null for "pose at rest". Warns at most once per entity per
		// distinct bad name, so a typo is loud but not once per frame.
		const AnimationClip* ResolveClip(entt::entity entity, const Mesh& mesh, const std::string& name);

		void BuildPalette(const Skeleton& skeleton, const AnimationClip* clip, float time,
			std::vector<glm::mat4>& outPalette);

		// Scratch reused across entities and frames - a rig is sampled every frame, and
		// reallocating two per-joint arrays per animator per frame is pure waste.
		std::vector<JointPose> m_Locals;
		std::vector<glm::mat4> m_Globals;

		std::unordered_map<entt::entity, std::string> m_WarnedClips;
	};
}

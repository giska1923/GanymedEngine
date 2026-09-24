#pragma once

#include <array>
#include <string>
#include <unordered_map>
#include <unordered_set>
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
			ECS::RW<AnimatorComponent>, ECS::RO<StaticMeshComponent>,
			ECS::OptRW<AimOffsetComponent>>;

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

		// Resolves the chain and, when it should bend, runs ApplyAimOffset on m_Globals
		// before the palette multiply. Disabled, unresolved, or zero angles leave the
		// sampled globals alone.
		void ApplyAim(entt::entity entity, AimOffsetComponent& aim, const Mesh& mesh);

		// Scratch reused across entities and frames - a rig is sampled every frame, and
		// reallocating two per-joint arrays per animator per frame is pure waste.
		std::vector<JointPose> m_Locals;
		std::vector<glm::mat4> m_Globals;

		std::unordered_map<entt::entity, std::string> m_WarnedClips;

		// One warning per entity per distinct missing joint name. A separate set for the
		// singular-skin case, which is not a joint name and must not share the key space.
		std::unordered_map<entt::entity, std::unordered_set<std::string>> m_WarnedAimJoints;
		std::unordered_set<entt::entity> m_WarnedAimAxes;

		// Descendant lists are a fact of the skeleton topology and the resolved indices,
		// not of the pose. Rebuilt when either changes. Lives here rather than on the
		// component: a subtree is variable length, and the component stays a fixed chain.
		struct AimSubtreeCache
		{
			std::vector<int32_t> Parents; // a copy of the topology it was built from
			int Count = 0;
			std::array<int32_t, AimOffsetChain::MaxJoints> Joints{};
			std::array<std::vector<uint32_t>, AimOffsetChain::MaxJoints> Subtrees;
		};
		std::unordered_map<entt::entity, AimSubtreeCache> m_AimSubtrees;
	};
}

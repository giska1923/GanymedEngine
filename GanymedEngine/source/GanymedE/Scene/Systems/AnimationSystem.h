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
			ECS::OptRW<AimOffsetComponent>, ECS::OptRW<TwoHandIKComponent>>;

		// Two-hand IK reads *other* entities: the weapon's socket and scale, and its marker
		// children's local transforms. Declared so ValidateOrdering sees the coupling - the
		// first time this system has read anything but its own entity. Never iterated;
		// FindOne() is the access. The socket is optional so a marker, which has none, still
		// matches the view.
		using WeaponAccess = ECS::AccessView<
			ECS::OptRO<BoneAttachmentComponent>,
			ECS::RO<TransformComponent>,
			ECS::RO<RelationshipComponent>>;

		using Views = TypeList<AnimView, WeaponAccess>;

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

		// Builds the weapon frame from `palette` (already written from the post-aim globals),
		// turns it onto `aim`'s direction when the component's AimLock asks, solves each usable
		// hand on m_Globals, and rewrites the palette entries of the arms it moved. A hand whose
		// joints, marker or weapon cannot be resolved is skipped whole, with one warning;
		// disabled, or both weights zero, leaves the palette untouched.
		void ApplyTwoHandIK(entt::entity entity, TwoHandIKComponent& ik, const AimOffsetComponent* aim,
			const Mesh& mesh, std::vector<glm::mat4>& palette);

		// One warning per entity per distinct message; a setup that is fixed and then broken
		// again warns again, because a fully solved frame clears the entity's set.
		void WarnHandIK(entt::entity entity, const std::string& message);

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

		// Same idea for the two arms: Upper, Lower and End subtrees per hand, rebuilt when the
		// topology or a resolved index changes.
		struct HandIKSubtreeCache
		{
			std::vector<int32_t> Parents;
			std::array<int32_t, 6> Joints{ -1, -1, -1, -1, -1, -1 };
			std::array<std::vector<uint32_t>, 6> Subtrees;
		};
		std::unordered_map<entt::entity, HandIKSubtreeCache> m_HandIKSubtrees;
		std::unordered_map<entt::entity, std::unordered_set<std::string>> m_WarnedHandIK;
	};
}

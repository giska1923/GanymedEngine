#pragma once

#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "GanymedE/ECS/System.h"
#include "GanymedE/ECS/Views.h"
#include "GanymedE/Scene/Components.h"
#include "GanymedE/Scene/Entity.h"

namespace GanymedE {

	// Pins entities to joints. Recovers the joint frame through TryGetJointFrame (Mesh.h)
	// rather than keeping AnimationSystem's scratch globals: attachments are counted in ones
	// and twos, and a second per-joint array on every animator is 2-8 KB for Scene::Copy to
	// shuffle on every play. That function is the one owner of the joint-in-mesh-space maths;
	// this system multiplies by targetWorld and the authored offset.
	//
	// Writes WorldTransformComponent after TransformSystem (never TransformComponent: Euler
	// decomposition of a joint quaternion is lossy). CameraSystem is the first reader of
	// world space, so a camera socketed to a head joint sees this frame's pose.
	//
	// Runs in edit mode too. AnimationSystem already samples without advancing; sockets have
	// to follow that pose or the inspector would show the weapon on last frame's hand.
	class BoneAttachmentSystem : public ECS::System<BoneAttachmentSystem>
	{
	public:
		// RO: the component is authored state. Whether it resolved, and to which joint, lives in
		// m_Resolved - which is what lets AnimationSystem's two-hand IK read a weapon's socket
		// earlier in the frame without ValidateOrdering calling it a stale read.
		using AttachView = ECS::IterView<ECS::EntityId,
			ECS::RO<BoneAttachmentComponent>,
			ECS::RO<TransformComponent>,
			ECS::RO<RelationshipComponent>,
			ECS::RW<WorldTransformComponent>>;

		// The skinned mesh lives on a *different* entity than the socket. Declared so
		// ValidateOrdering knows we read the palette AnimationSystem just wrote. Never
		// iterated; Find() is the access.
		//
		// The target's TwoHandIKComponent is read for the aim lock: a weapon the pass has turned
		// onto the aim is drawn from the frame the pass recorded, not re-derived here, so the
		// hands and the weapon have one owner.
		using TargetAccess = ECS::AccessView<
			ECS::RO<WorldTransformComponent>,
			ECS::OptRO<AnimatorComponent>,
			ECS::OptRO<StaticMeshComponent>,
			ECS::OptRO<TwoHandIKComponent>>;

		using Views = TypeList<AttachView, TargetAccess>;

		using ECS::System<BoneAttachmentSystem>::System;

		void OnRuntimeStart() override;
		void OnUpdate(Timestep ts) override;
		void OnUpdateEditor(Timestep ts) override;
		const char* Name() const override { return "BoneAttachmentSystem"; }

		// The joint this socket was placed on by the last evaluation, or -1 when it did not
		// place it (no target, no rigged mesh, a bad joint name, no palette). The editor's socket
		// gizmo and joint tool read this rather than re-deriving the rule.
		int32_t ResolvedJoint(entt::entity entity) const;

	private:
		void Evaluate();

		Entity ResolveTarget(Entity entity, const BoneAttachmentComponent& attachment) const;
		int HierarchyDepth(Entity entity) const;
		int SortKey(Entity entity, const BoneAttachmentComponent& attachment) const;

		void WarnOnce(entt::entity entity, const std::string& what);

		std::vector<std::pair<int, entt::entity>> m_Order;
		std::unordered_map<entt::entity, std::string> m_Warned;

		// Rebuilt every evaluation: only sockets placed this frame have an entry.
		std::unordered_map<entt::entity, int32_t> m_Resolved;
	};

}

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

	// Pins entities to joints. Recovers jointGlobal from Palette[i] * inverse(InverseBind[i])
	// rather than keeping AnimationSystem's scratch globals: attachments are counted in ones
	// and twos, and a second per-joint array on every animator is 2-8 KB for Scene::Copy to
	// shuffle on every play.
	//
	// That frame alone is NOT the space the mesh is drawn in. Renderer3D draws a skinned
	// submesh as entityWorld * LocalTransform * Palette * v, and the joints may be authored in
	// a different unit than the vertices - a Meshy rig has joints in centimetres and vertices
	// in metres, with LocalTransform (0.01) the factor between them. So the skinned submesh's
	// LocalTransform is folded in, and the bind pose's own column scales are then divided out:
	// that scale is cancelled for vertices by the palette and cancelled for nothing else, so
	// left in it renders an attached entity at 1%. Dividing by the BIND scale rather than
	// normalising keeps animated scale, and is a no-op when the mesh node is identity.
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
		using AttachView = ECS::IterView<ECS::EntityId,
			ECS::RW<BoneAttachmentComponent>,
			ECS::RO<TransformComponent>,
			ECS::RO<RelationshipComponent>,
			ECS::RW<WorldTransformComponent>>;

		// The skinned mesh lives on a *different* entity than the socket. Declared so
		// ValidateOrdering knows we read the palette AnimationSystem just wrote. Never
		// iterated; Find() is the access.
		using TargetAccess = ECS::AccessView<
			ECS::RO<WorldTransformComponent>,
			ECS::OptRO<AnimatorComponent>,
			ECS::OptRO<StaticMeshComponent>>;

		using Views = TypeList<AttachView, TargetAccess>;

		using ECS::System<BoneAttachmentSystem>::System;

		void OnRuntimeStart() override;
		void OnUpdate(Timestep ts) override;
		void OnUpdateEditor(Timestep ts) override;
		const char* Name() const override { return "BoneAttachmentSystem"; }

	private:
		void Evaluate();

		Entity ResolveTarget(Entity entity, const BoneAttachmentComponent& attachment) const;
		int HierarchyDepth(Entity entity) const;
		int SortKey(Entity entity, const BoneAttachmentComponent& attachment) const;

		void WarnOnce(entt::entity entity, const std::string& what);

		std::vector<std::pair<int, entt::entity>> m_Order;
		std::unordered_map<entt::entity, std::string> m_Warned;
	};

}

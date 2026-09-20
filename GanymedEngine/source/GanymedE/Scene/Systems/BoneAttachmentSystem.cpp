#include "gepch.h"
#include "BoneAttachmentSystem.h"

#include "GanymedE/Renderer/Mesh.h"
#include "GanymedE/Scene/Entity.h"
#include "GanymedE/Scene/Scene.h"
#include "GanymedE/Scene/Systems/TransformSystem.h"

#include <algorithm>
#include <unordered_set>

namespace GanymedE {

	namespace {

		// Offset and Rotation replace the entity's local translation and rotation, which is why
		// those two are ignored. Scale has no counterpart on the component, so the local one is
		// kept: a socketed prop is sized in the inspector like any other entity, and - the point -
		// it is sized the SAME way on the Restore() path. A compensation factor that only applies
		// while the socket resolves is how a 1.9 m rifle became an 86 m one on every load.
		glm::mat4 OffsetMatrix(const BoneAttachmentComponent& attachment, const glm::vec3& scale)
		{
			glm::mat4 rotation = glm::rotate(glm::mat4(1.0f), attachment.Rotation.x, { 1, 0, 0 })
				* glm::rotate(glm::mat4(1.0f), attachment.Rotation.y, { 0, 1, 0 })
				* glm::rotate(glm::mat4(1.0f), attachment.Rotation.z, { 0, 0, 1 });

			return glm::translate(glm::mat4(1.0f), attachment.Offset) * rotation
				* glm::scale(glm::mat4(1.0f), scale);
		}

		int32_t ResolveJointIndex(const std::vector<std::string>& names, const std::string& joint,
			int32_t resolved)
		{
			if (joint.empty())
				return -1;

			if (resolved >= 0 && (size_t)resolved < names.size() && names[(size_t)resolved] == joint)
				return resolved;

			for (int32_t i = 0; i < (int32_t)names.size(); i++)
			{
				if (names[(size_t)i] == joint)
					return i;
			}

			return -1;
		}

	}

	void BoneAttachmentSystem::OnRuntimeStart()
	{
		m_Warned.clear();
	}

	void BoneAttachmentSystem::OnUpdate(Timestep ts)
	{
		(void)ts;
		Evaluate();
	}

	void BoneAttachmentSystem::OnUpdateEditor(Timestep ts)
	{
		(void)ts;
		Evaluate();
	}

	Entity BoneAttachmentSystem::ResolveTarget(Entity entity,
		const BoneAttachmentComponent& attachment) const
	{
		UUID id = attachment.Target;
		if (id == UUID{ 0 })
			id = entity.GetComponent<RelationshipComponent>().Parent;

		if (id == UUID{ 0 })
			return {};

		return m_Scene.FindEntityByUUID(id);
	}

	int BoneAttachmentSystem::HierarchyDepth(Entity entity) const
	{
		int depth = 0;
		std::unordered_set<entt::entity> seen;

		while (entity)
		{
			if (!seen.insert((entt::entity)entity).second)
				break;

			UUID parentID = entity.GetComponent<RelationshipComponent>().Parent;
			if (parentID == UUID{ 0 })
				break;

			entity = m_Scene.FindEntityByUUID(parentID);
			++depth;
		}

		return depth;
	}

	int BoneAttachmentSystem::SortKey(Entity entity, const BoneAttachmentComponent& attachment) const
	{
		int key = HierarchyDepth(entity);

		// An explicit Target that is itself socketed has to be applied first, even if this
		// entity is not parented under it (a camera named onto a gun that is not its child).
		Entity target = ResolveTarget(entity, attachment);
		if (target && target != entity && target.HasComponent<BoneAttachmentComponent>())
			key = std::max(key, HierarchyDepth(target) + 1);

		return key;
	}

	void BoneAttachmentSystem::WarnOnce(entt::entity entity, const std::string& what)
	{
		auto [it, inserted] = m_Warned.try_emplace(entity, what);
		if (inserted || it->second != what)
		{
			it->second = what;
			GE_CORE_WARN("{0}", what);
		}
	}

	void BoneAttachmentSystem::Evaluate()
	{
		m_Order.clear();

		for (auto [entity, attachment, transform, relationship, world] : View<AttachView>())
		{
			(void)transform;
			(void)relationship;
			(void)world;

			Entity handle{ entity, &m_Scene };
			m_Order.push_back({ SortKey(handle, attachment), entity });
		}

		if (m_Order.empty())
			return;

		std::sort(m_Order.begin(), m_Order.end(),
			[](const std::pair<int, entt::entity>& a, const std::pair<int, entt::entity>& b)
			{
				return a.first < b.first;
			});

		TransformSystem* transforms = m_Scene.Systems().Get<TransformSystem>();
		if (!transforms)
			return;

		auto targets = View<TargetAccess>();

		for (const auto& item : m_Order)
		{
			Entity entity{ item.second, &m_Scene };
			auto& attachment = entity.GetComponent<BoneAttachmentComponent>();

			const auto Restore = [&]()
			{
				glm::mat4 local = entity.GetComponent<TransformComponent>().GetLocalTransform();
				UUID parentID = entity.GetComponent<RelationshipComponent>().Parent;
				if (parentID == UUID{ 0 })
				{
					transforms->OverrideWorld(entity, local);
					return;
				}

				Entity parent = m_Scene.FindEntityByUUID(parentID);
				if (!parent)
				{
					transforms->OverrideWorld(entity, local);
					return;
				}

				transforms->OverrideWorld(entity,
					parent.GetComponent<WorldTransformComponent>().World * local);
			};

			Entity target = ResolveTarget(entity, attachment);
			if (!target)
			{
				attachment.Resolved = -1;
				WarnOnce(item.second,
					"BoneAttachment on '" + entity.GetName() + "' has no target - "
					"leaving the entity at its parent transform");
				Restore();
				continue;
			}

			if (target == entity)
			{
				attachment.Resolved = -1;
				WarnOnce(item.second,
					"BoneAttachment on '" + entity.GetName() + "' names itself as the target - "
					"leaving the entity at its parent transform");
				Restore();
				continue;
			}

			auto targetWorld = targets.FindOne<WorldTransformComponent>(target);
			auto animator = targets.FindOne<AnimatorComponent>(target);
			auto meshComponent = targets.FindOne<StaticMeshComponent>(target);

			if (!meshComponent || !meshComponent->Mesh.Ready() || !meshComponent->Mesh->HasSkeleton())
			{
				attachment.Resolved = -1;
				WarnOnce(item.second,
					"BoneAttachment on '" + entity.GetName() + "' targets '" + target.GetName() +
					"', which has no rigged mesh - leaving the entity at its parent transform");
				Restore();
				continue;
			}

			const Mesh& mesh = *meshComponent->Mesh.Get();
			const Skeleton& skeleton = mesh.GetSkeleton();

			if (!animator || animator->Palette.size() != skeleton.JointCount()
				|| skeleton.InverseBind.size() != skeleton.JointCount())
			{
				attachment.Resolved = -1;
				WarnOnce(item.second,
					"BoneAttachment on '" + entity.GetName() + "' targets '" + target.GetName() +
					"', which has no joint palette - leaving the entity at its parent transform");
				Restore();
				continue;
			}

			attachment.Resolved = ResolveJointIndex(skeleton.JointNames, attachment.Joint,
				attachment.Resolved);

			if (attachment.Resolved < 0)
			{
				if (attachment.Joint.empty())
				{
					m_Warned.erase(item.second);
					Restore();
					continue;
				}

				WarnOnce(item.second,
					"BoneAttachment on '" + entity.GetName() + "' references joint '" +
					attachment.Joint + "', which mesh '" + mesh.GetPath() +
					"' does not have - leaving the entity at its parent transform");
				Restore();
				continue;
			}

			if (!targetWorld)
			{
				attachment.Resolved = -1;
				Restore();
				continue;
			}

			const glm::mat4& inverseBind = skeleton.InverseBind[(size_t)attachment.Resolved];
			const float det = glm::determinant(inverseBind);
			if (glm::abs(det) < 1e-8f)
			{
				attachment.Resolved = -1;
				WarnOnce(item.second,
					"BoneAttachment on '" + entity.GetName() + "' joint '" + attachment.Joint +
					"' has a singular inverse bind - leaving the entity at its parent transform");
				Restore();
				continue;
			}

			// Renderer3D draws a skinned submesh as entityWorld * LocalTransform * Palette * v, and
			// a socket has to ride the same chain or it is not in the same space as the mesh it is
			// pinned to. Palette * inverse(InverseBind) recovers the joint global alone, which is in
			// whatever unit the JOINTS were authored in - for a Meshy rig, centimetres, while the
			// vertices are metres. LocalTransform (the skinned mesh node's world, which the importer
			// deliberately keeps) is the factor between the two. Omit it and a socket lands at 141
			// *metres* instead of 1.41: a correctly sized prop, far enough away to look tiny.
			glm::mat4 skinTransform{ 1.0f };
			for (const Submesh& submesh : mesh.GetSubmeshes())
			{
				if (submesh.IsSkinned)
				{
					skinTransform = submesh.LocalTransform;
					break;
				}
			}

			// inverse(InverseBind) is LocalTransform * bindGlobal, so it is exactly this frame in
			// the bind pose: translation in metres, basis carrying LocalTransform's scale. That
			// scale is cancelled for *vertices* by the 1/scale inside the palette, and nothing
			// cancels it for a socket - left in, an attached entity renders at 1% and Offset
			// silently means centimetres.
			const glm::mat4 bindGlobal = glm::inverse(inverseBind);
			glm::mat4 jointGlobal = skinTransform
				* animator->Palette[(size_t)attachment.Resolved] * bindGlobal;

			// Divided out per column rather than normalised to unit length, so a clip that scales
			// the joint still scales what is attached to it - the palette's scale is relative to
			// bind, and only the bind part is the authoring artifact. For a rig whose mesh node is
			// identity every column is already 1 and this loop does nothing.
			for (int column = 0; column < 3; column++)
			{
				const float bindScale = glm::length(glm::vec3(bindGlobal[column]));
				if (bindScale > 1e-6f)
					jointGlobal[column] /= bindScale;
			}

			m_Warned.erase(item.second);
			transforms->OverrideWorld(entity, targetWorld->World * jointGlobal
				* OffsetMatrix(attachment, entity.GetComponent<TransformComponent>().Scale));
		}
	}

}

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

		// A linear search by name every frame, no cached hint: a socket's joint name is compared
		// against at most a rig's worth of names, and without a hint there is nothing to go stale
		// across a mesh swap, a DCC rename, a scene copy or a duplicate.
		int32_t ResolveJointIndex(const std::vector<std::string>& names, const std::string& joint)
		{
			if (joint.empty())
				return -1;

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

	int32_t BoneAttachmentSystem::ResolvedJoint(entt::entity entity) const
	{
		auto it = m_Resolved.find(entity);
		return it != m_Resolved.end() ? it->second : -1;
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
		m_Resolved.clear();

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
			const auto& attachment = entity.GetComponent<BoneAttachmentComponent>();

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
				WarnOnce(item.second,
					"BoneAttachment on '" + entity.GetName() + "' has no target - "
					"leaving the entity at its parent transform");
				Restore();
				continue;
			}

			if (target == entity)
			{
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
				WarnOnce(item.second,
					"BoneAttachment on '" + entity.GetName() + "' targets '" + target.GetName() +
					"', which has no rigged mesh - leaving the entity at its parent transform");
				Restore();
				continue;
			}

			const Mesh& mesh = *meshComponent->Mesh.Get();
			const Skeleton& skeleton = mesh.GetSkeleton();

			// The animator's palette, or the rest palette when there is no animator - the pose
			// RenderSystem draws, so a socket on an unanimated rig sits on the visible hand.
			const std::vector<glm::mat4>& palette =
				ResolvePosePalette(mesh, animator ? &animator->Palette : nullptr);
			if (palette.size() != skeleton.JointCount()
				|| skeleton.InverseBind.size() != skeleton.JointCount())
			{
				WarnOnce(item.second,
					"BoneAttachment on '" + entity.GetName() + "' targets '" + target.GetName() +
					"', whose skeleton has no usable palette - leaving the entity at its parent transform");
				Restore();
				continue;
			}

			const int32_t resolved = ResolveJointIndex(skeleton.JointNames, attachment.Joint);

			if (resolved < 0)
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
				Restore();
				continue;
			}

			glm::mat4 jointGlobal{ 1.0f };
			if (!TryGetJointFrame(mesh, palette, resolved, jointGlobal))
			{
				WarnOnce(item.second,
					"BoneAttachment on '" + entity.GetName() + "' joint '" + attachment.Joint +
					"' has a singular inverse bind - leaving the entity at its parent transform");
				Restore();
				continue;
			}

			m_Warned.erase(item.second);
			m_Resolved[item.second] = resolved;
			transforms->OverrideWorld(entity, targetWorld->World * jointGlobal
				* attachment.OffsetMatrix(entity.GetComponent<TransformComponent>().Scale));
		}
	}

}

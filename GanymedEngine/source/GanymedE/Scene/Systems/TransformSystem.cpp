#include "gepch.h"
#include "TransformSystem.h"

#include "GanymedE/Scene/Entity.h"
#include "GanymedE/Scene/Scene.h"

namespace GanymedE {

	glm::mat4 TransformSystem::WorldFromLocals(Entity entity) const
	{
		glm::mat4 local = entity.GetComponent<TransformComponent>().GetLocalTransform();

		UUID parentID = entity.GetComponent<RelationshipComponent>().Parent;
		if (parentID == UUID{ 0 })
			return local;

		Entity parent = m_Scene.FindEntityByUUID(parentID);   // O(1) since Phase 0
		if (!parent)
			return local;

		return WorldFromLocals(parent) * local;
	}

	void TransformSystem::RecomputeSubtree(Entity entity, const glm::mat4& world)
	{
		if (!entity)
			return;

		// A parent and one of its children can both be in the dirty list. Whichever is reached
		// first computes the correct value (WorldFromLocals does not depend on caches), so the
		// second visit would only redo identical work.
		if (!m_Visited.insert((entt::entity)entity).second)
			return;

		if (auto* cache = m_Scene.Reg().try_get<WorldTransformComponent>((entt::entity)entity))
			cache->World = world;

		const auto& relationship = entity.GetComponent<RelationshipComponent>();
		for (UUID childID : relationship.Children)
		{
			Entity child = m_Scene.FindEntityByUUID(childID);
			if (!child)
				continue;

			RecomputeSubtree(child, world * child.GetComponent<TransformComponent>().GetLocalTransform());
		}
	}

	void TransformSystem::RecomputeDirty()
	{
		m_Visited.clear();

		// First read of the ChangeView reports the whole matching world, which is exactly what
		// populates the cache on a fresh scene.
		for (auto [entity, transform, relationship, world] : View<DirtyView>())
		{
			(void)transform;
			(void)relationship;
			(void)world;

			Entity handle{ entity, &m_Scene };
			RecomputeSubtree(handle, WorldFromLocals(handle));
		}
	}

	void TransformSystem::OnUpdate(Timestep ts)
	{
		(void)ts;
		RecomputeDirty();
	}

	void TransformSystem::OnUpdateEditor(Timestep ts)
	{
		(void)ts;
		RecomputeDirty();
	}
}

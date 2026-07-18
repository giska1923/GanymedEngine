#include "gepch.h"
#include "CommandQueue.h"

#include "GanymedE/Scene/Scene.h"

namespace GanymedE::ECS {

	PendingEntity CommandQueue::CreateEntity(const std::string& name)
	{
		const size_t index = m_PendingCount++;
		m_CreateOps.emplace_back([this, index, name](Scene& scene)
		{
			m_CreatedEntities[index] = scene.CreateEntity(name);
		});
		return PendingEntity{ index };
	}

	void CommandQueue::DestroyEntity(Entity entity)
	{
		m_DestroyOps.emplace_back([entity](Scene& scene)
		{
			// Already gone (queued twice, or destroyed by the editor) — not an error.
			if (scene.Reg().valid((entt::entity)entity))
				scene.DestroyEntity(entity);
		});
	}

	void CommandQueue::Flush(Scene& scene)
	{
		// Take ownership of the queues up front. Anything a queued op enqueues lands in the now
		// empty member vectors and runs next frame, rather than executing mid-flush where it would
		// break the ordering guarantee below.
		auto removeOps  = std::exchange(m_RemoveOps, {});
		auto addOps     = std::exchange(m_AddOps, {});
		auto createOps  = std::exchange(m_CreateOps, {});
		auto pendingOps = std::exchange(m_PendingComponentOps, {});
		auto destroyOps = std::exchange(m_DestroyOps, {});
		const size_t pendingCount = std::exchange(m_PendingCount, 0);

		// The order exists so that same-frame remove + re-add works, and so a newly created entity
		// can have components attached in the same frame it is created.

		// 1. component removals on existing entities (removed instances -> graveyard)
		for (auto& op : removeOps)
			op(scene);

		// 2. component additions on existing entities
		for (auto& op : addOps)
			op(scene);

		// 3. entity creations, then components on those new entities
		m_CreatedEntities.assign(pendingCount, Entity{});
		for (auto& op : createOps)
			op(scene);
		for (auto& op : pendingOps)
			op(scene);

		// 4. entity destructions
		for (auto& op : destroyOps)
			op(scene);

		m_CreatedEntities.clear();
	}
}

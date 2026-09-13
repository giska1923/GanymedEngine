#include "gepch.h"

#include <optional>
#include <unordered_set>
#include "GanymedE/Scene/Components.h"
#include "GanymedE/Scene/PrefabSerializer.h"
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

	void CommandQueue::DestroyEntityTree(Entity root)
	{
		m_DestroyOps.emplace_back([root](Scene& scene)
		{
			if (!scene.Reg().valid((entt::entity)root))
				return;

			// Collected first, then destroyed: the walk reads RelationshipComponent, and
			// destroying as it goes would be reading a hierarchy it is dismantling. The same
			// shape the editor's delete uses.
			std::vector<Entity> subtree;
			std::unordered_set<UUID> visited;
			scene.CollectSubtree(root, subtree, visited);

			// Deepest first, so each entity still has a valid parent link to detach from.
			for (auto it = subtree.rbegin(); it != subtree.rend(); ++it)
			{
				if (scene.Reg().valid((entt::entity)*it))
					scene.DestroyEntity(*it);
			}
		});
	}

	UUID CommandQueue::InstantiatePrefab(AssetHandle source, const TransformComponent* rootTransform)
	{
		if (m_SpawnsThisFrame >= MaxSpawnsPerFrame)
		{
			++m_SpawnsRefused;
			return UUID{ 0 };
		}

		++m_SpawnsThisFrame;

		// Minted here so the caller has something to hold before the entity exists. The
		// instantiate below pins it rather than generating its own.
		const UUID rootID;

		// Captured **by value**: the op runs next frame, long after the caller's locals are gone
		// - the same rule AddComponent's arguments follow, and the reason this is an optional
		// rather than the pointer the parameter arrives as.
		std::optional<TransformComponent> transform;
		if (rootTransform)
			transform = *rootTransform;

		m_CreateOps.emplace_back([source, rootID, transform](Scene& scene)
		{
			PrefabSerializer::InstantiateOptions options;
			options.RootUUID = rootID;
			options.RootTransform = transform ? &(*transform) : nullptr;

			// Legal here and nowhere else on this path: the flush runs from FrameBegin with
			// IsUpdating false, so Instantiate's immediate Entity API is allowed. Called from a
			// script directly it would trip Entity::AddComponent's assert.
			if (!PrefabSerializer::InstantiateFromAsset(source, scene, options))
			{
				GE_CORE_WARN("Spawn: prefab {0} could not be instantiated",
					static_cast<uint64_t>(source));
			}
		});

		return rootID;
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

		// Once per frame with the total, not once per refusal: a runaway loop would otherwise
		// trade memory exhaustion for log exhaustion.
		if (const std::size_t refused = std::exchange(m_SpawnsRefused, 0); refused > 0)
		{
			GE_CORE_WARN("Spawn cap: {0} prefab spawn(s) refused this frame - the cap is {1}. A "
				"script spawning in an unguarded loop is the usual cause; Scene.Spawn returned "
				"nil for each of them.", refused, MaxSpawnsPerFrame);
		}

		m_SpawnsThisFrame = 0;

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

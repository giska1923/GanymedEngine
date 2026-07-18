#pragma once

#include <functional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "GanymedE/Scene/Entity.h"

// Deferred structural changes.
//
// Adding or removing a component of a type currently being iterated is undefined behaviour in
// entt, which makes the immediate Entity::AddComponent / RemoveComponent API a latent crash when
// called from inside a system. Systems queue their structural changes here instead; the queue is
// applied once per frame at a fixed point, in a fixed order, before any system runs.
//
// Consequence to accept consciously: a structural change made by a system becomes visible on the
// *next* frame. Editor and tooling code runs outside the update loop and keeps using the immediate
// Entity API.

namespace GanymedE::ECS {

	// Handle to an entity that does not exist yet. Only meaningful within the frame it was created
	// in: components can be queued onto it, and the flush resolves it to a real Entity.
	class PendingEntity
	{
	public:
		PendingEntity() = default;
		bool IsValid() const { return m_Index != InvalidIndex; }

	private:
		friend class CommandQueue;
		explicit PendingEntity(size_t index) : m_Index(index) {}

		static constexpr size_t InvalidIndex = ~size_t(0);
		size_t m_Index = InvalidIndex;
	};

	class CommandQueue
	{
	public:
		template<typename T, typename... Args>
		void AddComponent(Entity entity, Args&&... args)
		{
			// Arguments are captured by value: the op runs next frame, long after the caller's
			// locals are gone.
			auto arguments = std::make_tuple(std::forward<Args>(args)...);
			m_AddOps.emplace_back([entity, arguments](Scene& scene) mutable
			{
				ApplyAdd<T>(scene, entity, std::move(arguments),
					std::make_index_sequence<sizeof...(Args)>{});
			});
		}

		template<typename T>
		void RemoveComponent(Entity entity)
		{
			m_RemoveOps.emplace_back([entity](Scene& scene)
			{
				entt::entity handle = (entt::entity)entity;
				if (scene.Reg().valid(handle) && scene.Reg().all_of<T>(handle))
					scene.Reg().remove<T>(handle);   // fires on_destroy -> graveyard
			});
		}

		PendingEntity CreateEntity(const std::string& name = std::string());

		// Queue a component onto an entity this queue will create during the same flush.
		template<typename T, typename... Args>
		void AddComponent(PendingEntity pending, Args&&... args)
		{
			GE_CORE_ASSERT(pending.IsValid(), "AddComponent on a default-constructed PendingEntity");
			const size_t index = pending.m_Index;
			auto arguments = std::make_tuple(std::forward<Args>(args)...);
			m_PendingComponentOps.emplace_back([this, index, arguments](Scene& scene) mutable
			{
				if (index >= m_CreatedEntities.size())
					return;
				Entity created = m_CreatedEntities[index];
				if (!created)
					return;
				ApplyAdd<T>(scene, created, std::move(arguments),
					std::make_index_sequence<sizeof...(Args)>{});
			});
		}

		void DestroyEntity(Entity entity);

		bool Empty() const
		{
			return m_RemoveOps.empty() && m_AddOps.empty() && m_CreateOps.empty()
				&& m_PendingComponentOps.empty() && m_DestroyOps.empty();
		}

	private:
		friend class ::GanymedE::Scene;

		// Applies every queued op in the guaranteed order. See Scene::FlushCommands.
		void Flush(Scene& scene);

		template<typename T, typename Tuple, size_t... I>
		static void ApplyAdd(Scene& scene, Entity entity, Tuple&& arguments, std::index_sequence<I...>)
		{
			const entt::entity handle = (entt::entity)entity;

			// Destroyed in the meantime by something outside the queue — not an error.
			if (!scene.Reg().valid(handle))
				return;

			// A same-frame remove + re-add is legal (removals run before additions). Still holding
			// the component here means two callers queued the same add, which is a bug.
			GE_CORE_ASSERT(!scene.Reg().all_of<T>(handle),
				"Queued AddComponent on an entity that already has the component");
			if (scene.Reg().all_of<T>(handle))
				return;

			entity.AddComponent<T>(std::get<I>(std::move(arguments))...);
		}

		std::vector<std::function<void(Scene&)>> m_RemoveOps;
		std::vector<std::function<void(Scene&)>> m_AddOps;
		std::vector<std::function<void(Scene&)>> m_CreateOps;
		std::vector<std::function<void(Scene&)>> m_PendingComponentOps;
		std::vector<std::function<void(Scene&)>> m_DestroyOps;

		std::vector<Entity> m_CreatedEntities;   // index -> real entity, valid only during a flush
		size_t m_PendingCount = 0;
	};
}

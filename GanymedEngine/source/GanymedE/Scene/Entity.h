#pragma once

#include "Scene.h"
#include "GanymedE/Core/UUID.h"

#include <entt/entt.hpp>

namespace GanymedE {

	class Entity
	{
	public:
		Entity() = default;
		Entity(entt::entity handle, Scene* scene);
		Entity(const Entity& other) = default;

		// Structural changes below are IMMEDIATE, and therefore illegal while systems are running:
		// entt makes adding or removing a component of an iterated type undefined behaviour. From
		// inside a system, queue the change through Scene::Commands() instead. Editor, serializer
		// and asset-import code runs outside the update loop and may keep using these directly.
		template<typename T, typename... Args>
		T& AddComponent(Args&&... args)
		{
			GE_CORE_ASSERT(!m_Scene->IsUpdating(),
				"Immediate AddComponent during system update - use Scene::Commands().AddComponent()");
			GE_CORE_ASSERT(!HasComponent<T>(), "Entity already has component!");
			T& component = m_Scene->m_Registry.emplace<T>(m_EntityHandle, std::forward<Args>(args)...);
			m_Scene->OnComponentAdded<T>(*this, component);
			return component;
		}

		template<typename T>
		T& GetComponent()
		{
			GE_CORE_ASSERT(HasComponent<T>(), "Entity does not have component!");
			return m_Scene->m_Registry.get<T>(m_EntityHandle);
		}

		template<typename T>
		const T& GetComponent() const
		{
			GE_CORE_ASSERT(HasComponent<T>(), "Entity does not have component!");
			return m_Scene->m_Registry.get<T>(m_EntityHandle);
		}

		template<typename T>
		bool HasComponent() const
		{
			return m_Scene->m_Registry.all_of<T>(m_EntityHandle);
		}

		template<typename T>
		void RemoveComponent()
		{
			GE_CORE_ASSERT(!m_Scene->IsUpdating(),
				"Immediate RemoveComponent during system update - use Scene::Commands().RemoveComponent()");
			GE_CORE_ASSERT(HasComponent<T>(), "Entity does not have component!");
			m_Scene->m_Registry.remove<T>(m_EntityHandle);
		}

		UUID GetUUID() const;
		const std::string& GetName() const;

		operator bool() const { return m_EntityHandle != entt::null; }
		operator entt::entity() const { return m_EntityHandle; }
		operator uint32_t() const { return (uint32_t)m_EntityHandle; }

		bool operator==(const Entity& other) const
		{
			return m_EntityHandle == other.m_EntityHandle && m_Scene == other.m_Scene;
		}

		bool operator!=(const Entity& other) const
		{
			return !(*this == other);
		}
	private:
		entt::entity m_EntityHandle{ entt::null };
		Scene* m_Scene = nullptr;
	};
}

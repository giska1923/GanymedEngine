#pragma once

#include <entt/entt.hpp>

#include "GanymedE/Core/Core.h"
#include "GanymedE/Core/Timestep.h"
#include "GanymedE/Core/UUID.h"
#include "GanymedE/Renderer/EditorCamera.h"

namespace GanymedE {

	class Entity;

	class Scene
	{
	public:
		Scene();
		~Scene();

		static Ref<Scene> Copy(Ref<Scene> other);

		Entity CreateEntity(const std::string& name = std::string());
		Entity CreateEntityWithUUID(UUID uuid, const std::string& name = std::string());
		void DestroyEntity(Entity entity);

		void OnUpdateRuntime(Timestep ts);
		void OnUpdateEditor(Timestep ts, EditorCamera& camera);
		void OnViewportResize(uint32_t width, uint32_t height);

		Entity GetPrimaryCameraEntity();
		Entity FindEntityByUUID(UUID uuid);

		glm::mat4 GetWorldSpaceTransform(Entity entity);
		void SetParent(Entity child, Entity parent);
		void Unparent(Entity child);
	private:
		template<typename T>
		void OnComponentAdded(Entity entity, T& component);

		void RemoveChildFromParent(UUID parentID, UUID childID);
	private:
		entt::registry m_Registry;
		uint32_t m_ViewportWidth = 0, m_ViewportHeight = 0;

		friend class Entity;
		friend class SceneSerializer;
		friend class SceneHierarchyPanel;
	};
}

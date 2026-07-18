#pragma once

#include <entt/entt.hpp>

#include <unordered_map>

#include "GanymedE/Core/Core.h"
#include "GanymedE/Core/Timestep.h"
#include "GanymedE/Core/UUID.h"
#include "GanymedE/Physics/PhysicsScene.h"
#include "GanymedE/Renderer/EditorCamera.h"

namespace GanymedE {

	class Entity;
	struct CameraComponent;

	class Scene
	{
	public:
		Scene();
		~Scene();

		static Ref<Scene> Copy(Ref<Scene> other);

		Entity CreateEntity(const std::string& name = std::string());
		Entity CreateEntityWithUUID(UUID uuid, const std::string& name = std::string());
		void DestroyEntity(Entity entity);

		void OnRuntimeStart();
		void OnRuntimeStop();

		void OnUpdateRuntime(Timestep ts, EditorCamera* fallbackCamera = nullptr);
		void OnUpdateEditor(Timestep ts, EditorCamera& camera);
		void OnViewportResize(uint32_t width, uint32_t height);

		Entity GetPrimaryCameraEntity();
		Entity FindEntityByUUID(UUID uuid);

		glm::mat4 GetWorldSpaceTransform(Entity entity);
		void SetParent(Entity child, Entity parent);
		void Unparent(Entity child);

		entt::registry& Reg() { return m_Registry; }
		const entt::registry& Reg() const { return m_Registry; }

		PhysicsDebugDrawSettings& GetPhysicsDebugDrawSettings() { return m_PhysicsDebugDraw; }
		const PhysicsDebugDrawSettings& GetPhysicsDebugDrawSettings() const { return m_PhysicsDebugDraw; }
	private:
		// Default: components need no post-add fixup. Specialize below (out of class) only for the
		// ones that do — no need to touch this when adding a component type.
		template<typename T>
		void OnComponentAdded(Entity, T&) {}

		void RemoveChildFromParent(UUID parentID, UUID childID);

		// Gathers light + sky components and submits them to Renderer3D, then draws the sky.
		void SubmitLightsAndSky();
		void DrawColliderGizmos();
		void DispatchCollisionEvents();
		void InstantiateScripts();
	private:
		entt::registry m_Registry;
		std::unordered_map<UUID, entt::entity> m_EntityMap;   // O(1) FindEntityByUUID
		uint32_t m_ViewportWidth = 0, m_ViewportHeight = 0;

		Scope<PhysicsScene> m_PhysicsScene;
		float m_PhysicsAccumulator = 0.0f;
		static constexpr float s_FixedTimestep = 1.0f / 60.0f;
		PhysicsDebugDrawSettings m_PhysicsDebugDraw;

		friend class Entity;
		friend class SceneSerializer;
		friend class SceneHierarchyPanel;
		friend class PhysicsScene;
	};

	// Must be declared before any use of AddComponent<CameraComponent>, otherwise translation units
	// would silently instantiate the empty primary template instead.
	template<>
	void Scene::OnComponentAdded<CameraComponent>(Entity entity, CameraComponent& component);
}

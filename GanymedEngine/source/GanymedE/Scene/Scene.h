#pragma once

#include <entt/entt.hpp>

#include <unordered_map>

#include "GanymedE/Core/Core.h"
#include "GanymedE/Core/Timestep.h"
#include "GanymedE/Core/UUID.h"
#include "GanymedE/ECS/ChangeBuffer.h"
#include "GanymedE/Physics/PhysicsScene.h"
#include "GanymedE/Renderer/EditorCamera.h"

namespace GanymedE {

	class Entity;
	struct CameraComponent;

	namespace ECS { class SystemManager; }

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

		ECS::SystemManager& Systems() { return *m_Systems; }
		const ECS::SystemManager& Systems() const { return *m_Systems; }

		// The editor camera for the current update: the view camera in edit mode, or the Play-mode
		// fallback used when the scene has no primary camera. Set at the top of each update.
		// Phase 7 replaces this with a RenderContext singleton in registry.ctx().
		EditorCamera* GetActiveEditorCamera() const { return m_ActiveEditorCamera; }

		// Change log for one tracked component type, created on first use.
		// Scene.h cannot include ComponentTraits.h (Components.h -> ScriptableEntity.h -> Entity.h
		// -> Scene.h), so the TrackChanges guard lives at the call sites in ECS/ that can.
		ECS::ChangeBuffer& GetChangeBuffer(entt::id_type componentTypeId);

		template<typename T>
		ECS::ChangeBuffer& GetChangeBuffer() { return GetChangeBuffer(entt::type_hash<T>::value()); }
	private:
		// Default: components need no post-add fixup. Specialize below (out of class) only for the
		// ones that do — no need to touch this when adding a component type.
		template<typename T>
		void OnComponentAdded(Entity, T&) {}

		// on_construct handler for change-tracked components: creating a component counts as a
		// change, so that a remove + re-add within one frame still surfaces in ChangeViews.
		template<typename T>
		void OnTrackedConstruct(entt::registry& registry, entt::entity entity);

		// Per-frame preamble for both runtime and editor updates. Rotates change history; later
		// phases add graveyard clearing, reactive buffer resets, the frame epoch and command flush.
		void FrameBegin();

		void RemoveChildFromParent(UUID parentID, UUID childID);
	private:
		entt::registry m_Registry;
		std::unordered_map<UUID, entt::entity> m_EntityMap;   // O(1) FindEntityByUUID
		std::unordered_map<entt::id_type, ECS::ChangeBuffer> m_ChangeBuffers;   // one per tracked type
		uint32_t m_ViewportWidth = 0, m_ViewportHeight = 0;

		// Held by pointer so Scene.h need not include System.h: the systems include Views.h, which
		// includes Scene.h. Scene's destructor is defined in Scene.cpp, where the type is complete.
		Scope<ECS::SystemManager> m_Systems;

		PhysicsDebugDrawSettings m_PhysicsDebugDraw;
		EditorCamera* m_ActiveEditorCamera = nullptr;

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

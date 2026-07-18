#pragma once

#include <entt/entt.hpp>

#include <unordered_map>

#include "GanymedE/Core/Core.h"
#include "GanymedE/Core/Timestep.h"
#include "GanymedE/Core/UUID.h"
#include "GanymedE/ECS/ChangeBuffer.h"
#include "GanymedE/ECS/Graveyard.h"
#include "GanymedE/Physics/PhysicsScene.h"
#include "GanymedE/Renderer/EditorCamera.h"

namespace GanymedE {

	class Entity;
	struct CameraComponent;

	namespace ECS { class SystemManager; class CommandQueue; }

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

		// Deferred structural changes. Systems must mutate through this rather than the immediate
		// Entity API; the queue is applied in FrameBegin, before any system runs.
		ECS::CommandQueue& Commands() { return *m_Commands; }

		// True while systems are running. Structural changes are illegal in that window.
		bool IsUpdating() const { return m_IsUpdating; }

		// Monotonic per-update counter driving the reactive views' epoch protocol.
		// Starts at 1 so that a view state's Epoch of 0 unambiguously means "never read".
		uint32_t GetFrameEpoch() const { return m_FrameEpoch; }

		// Entities whose component of this type was created / destroyed since systems last ran.
		const std::vector<entt::entity>& GetInitBuffer(entt::id_type componentTypeId) const;
		const std::vector<entt::entity>& GetFiniBuffer(entt::id_type componentTypeId) const;

		template<typename T>
		const std::vector<entt::entity>& GetInitBuffer() const
		{
			return GetInitBuffer(entt::type_hash<T>::value());
		}

		template<typename T>
		const std::vector<entt::entity>& GetFiniBuffer() const
		{
			return GetFiniBuffer(entt::type_hash<T>::value());
		}

		// One-frame storage of components removed since systems last ran, for FiniView.
		template<typename T>
		ECS::Graveyard<T>& GetGraveyard()
		{
			const entt::id_type id = entt::type_hash<T>::value();
			auto it = m_Graveyards.find(id);
			if (it == m_Graveyards.end())
				it = m_Graveyards.emplace(id, CreateScope<ECS::Graveyard<T>>()).first;
			return static_cast<ECS::Graveyard<T>&>(*it->second);
		}
	private:
		// Default: components need no post-add fixup. Specialize below (out of class) only for the
		// ones that do — no need to touch this when adding a component type.
		template<typename T>
		void OnComponentAdded(Entity, T&) {}

		// on_construct handler for change-tracked components: creating a component counts as a
		// change, so that a remove + re-add within one frame still surfaces in ChangeViews.
		template<typename T>
		void OnTrackedConstruct(entt::registry& registry, entt::entity entity);

		// on_destroy handler for components with EnableFini: entt fires this *before* the instance
		// is removed, which is the only moment a copy can still be taken.
		template<typename T>
		void OnFiniDestroy(entt::registry& registry, entt::entity entity);

		template<typename T>
		void OnInitConstruct(entt::registry& registry, entt::entity entity);

		// Preamble for both runtime and editor updates: bump the epoch, rotate change history,
		// then flush the command queue (which refills the init/fini buffers and graveyards).
		void FrameBegin();

		// Postamble: discard the reactive events and corpses this update's systems have now seen.
		//
		// DEVIATION from the guide, which clears these at the *top* of FrameBegin. Doing so drops
		// anything recorded outside the flush — notably a component the editor removes between two
		// frames — before a single system can react to it, which would leak script instances.
		// Clearing after the systems have run means the buffers hold "everything since systems
		// last ran", which is what the reactive views actually promise.
		void FrameEnd();

		void FlushCommands();
		void ClearGraveyards();

		void RemoveChildFromParent(UUID parentID, UUID childID);
	private:
		entt::registry m_Registry;
		std::unordered_map<UUID, entt::entity> m_EntityMap;   // O(1) FindEntityByUUID
		std::unordered_map<entt::id_type, ECS::ChangeBuffer> m_ChangeBuffers;   // one per tracked type
		uint32_t m_ViewportWidth = 0, m_ViewportHeight = 0;

		// Held by pointer so Scene.h need not include System.h: the systems include Views.h, which
		// includes Scene.h. Scene's destructor is defined in Scene.cpp, where the type is complete.
		Scope<ECS::SystemManager> m_Systems;
		Scope<ECS::CommandQueue> m_Commands;

		std::unordered_map<entt::id_type, Scope<ECS::GraveyardBase>> m_Graveyards;

		struct ReactiveState
		{
			std::vector<entt::entity> InitSinceLastUpdate;
			std::vector<entt::entity> FiniSinceLastUpdate;
		};
		std::unordered_map<entt::id_type, ReactiveState> m_Reactive;

		uint32_t m_FrameEpoch = 1;   // MUST start at 1; 0 means "this view has never been read"
		bool m_IsUpdating = false;

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

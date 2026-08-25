#include "gepch.h"
#include "Scene.h"

#include "Entity.h"
#include "Components.h"
#include "GanymedE/ECS/CommandQueue.h"
#include "GanymedE/ECS/ComponentTraits.h"
#include "GanymedE/ECS/System.h"
#include "GanymedE/Scene/Systems/AnimationSystem.h"
#include "GanymedE/Scene/Systems/AudioSystem.h"
#include "GanymedE/Scene/Systems/CameraSystem.h"
#include "GanymedE/Scene/Systems/LuaScriptSystem.h"
#include "GanymedE/Scene/Systems/NativeScriptSystem.h"
#include "GanymedE/Scene/Systems/PhysicsSystem.h"
#include "GanymedE/Scene/Systems/RenderSystem.h"
#include "GanymedE/Scene/Systems/TransformSystem.h"
#include "GanymedE/Physics/PhysicsScene.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

namespace GanymedE {

	Scene::Scene()
	{
		// Component creation counts as a change, so ChangeViews see a remove + re-add within one
		// frame rather than treating the new instance as unchanged.
		ForEachType(ComponentList{}, [this](auto typeTag)
		{
			using T = typename decltype(typeTag)::Type;
			if constexpr (ComponentTraits<T>::TrackChanges)
				m_Registry.on_construct<T>().template connect<&Scene::OnTrackedConstruct<T>>(*this);

			if constexpr (ComponentTraits<T>::EnableInit)
				m_Registry.on_construct<T>().template connect<&Scene::OnInitConstruct<T>>(*this);

			// on_destroy fires before the instance is removed, so the handler can still copy it.
			if constexpr (ComponentTraits<T>::EnableFini)
				m_Registry.on_destroy<T>().template connect<&Scene::OnFiniDestroy<T>>(*this);
		});

		m_Commands = CreateScope<ECS::CommandQueue>();

		// Scene-wide state the systems below expect to exist.
		SetSingleton<RenderContext>();
		SetSingleton<PhysicsSettings>();

		// Registration order IS execution order, and matches the order the logic previously ran
		// inline in OnUpdateRuntime: physics, then scripts, then rendering. CameraSystem must
		// come before RenderSystem, which now reads the camera it resolves.
		m_Systems = CreateScope<ECS::SystemManager>();
		m_Systems->Add<PhysicsSystem>(*this);
		m_Systems->Add<NativeScriptSystem>(*this);
		m_Systems->Add<LuaScriptSystem>(*this);   // scripts move things...
		// ...and set clip state, which the palette must reflect the same frame. Nothing enforces
		// this slot: AnimationSystem shares no component with TransformSystem, and it and the
		// script systems are both *writers* of AnimatorComponent, which ValidateOrdering
		// deliberately refuses to arbitrate. It becomes enforced against RenderSystem only, once
		// that reads the palette.
		m_Systems->Add<AnimationSystem>(*this);
		m_Systems->Add<TransformSystem>(*this);   // after anything that moves entities...
		m_Systems->Add<CameraSystem>(*this);      // ...and before anything that reads world space
		// After CameraSystem for a reason ValidateOrdering cannot see: AudioSystem shares no
		// component with it, but its listener FALLBACK reads the camera pose CameraSystem just
		// resolved into RenderContext, so running after means the fallback ears are this
		// frame's. After RenderSystem would be equally correct - Render reads nothing of audio -
		// but would break the "Render is last" reading of this list for no gain.
		m_Systems->Add<AudioSystem>(*this);
		m_Systems->Add<RenderSystem>(*this);

		// The views the systems declare imply an ordering; check the order above actually honours
		// it, rather than relying on the comments staying true.
		const size_t orderingViolations = m_Systems->ValidateOrdering();
		GE_CORE_ASSERT(orderingViolations == 0,
			"System registration order contradicts the systems' declared component access");
		(void)orderingViolations;
	}

	template<typename T>
	void Scene::OnTrackedConstruct(entt::registry&, entt::entity entity)
	{
		GetChangeBuffer<T>().Add(entity);
	}

	template<typename T>
	void Scene::OnInitConstruct(entt::registry&, entt::entity entity)
	{
		m_Reactive[entt::type_hash<T>::value()].InitSinceLastUpdate.push_back(entity);
	}

	template<typename T>
	void Scene::OnFiniDestroy(entt::registry& registry, entt::entity entity)
	{
		GetGraveyard<T>().Bury(entity, registry.get<T>(entity));   // still alive inside on_destroy
		m_Reactive[entt::type_hash<T>::value()].FiniSinceLastUpdate.push_back(entity);
	}

	const std::vector<entt::entity>& Scene::GetInitBuffer(entt::id_type componentTypeId) const
	{
		static const std::vector<entt::entity> s_Empty;
		auto it = m_Reactive.find(componentTypeId);
		return it != m_Reactive.end() ? it->second.InitSinceLastUpdate : s_Empty;
	}

	const std::vector<entt::entity>& Scene::GetFiniBuffer(entt::id_type componentTypeId) const
	{
		static const std::vector<entt::entity> s_Empty;
		auto it = m_Reactive.find(componentTypeId);
		return it != m_Reactive.end() ? it->second.FiniSinceLastUpdate : s_Empty;
	}

	ECS::ChangeBuffer& Scene::GetChangeBuffer(entt::id_type componentTypeId)
	{
		return m_ChangeBuffers[componentTypeId];
	}

	void Scene::ClearGraveyards()
	{
		for (auto& entry : m_Graveyards)
			entry.second->Clear();
	}

	void Scene::FlushCommands()
	{
		m_Commands->Flush(*this);
	}

	void Scene::FrameBegin()
	{
		m_FrameEpoch++;

		for (auto& entry : m_ChangeBuffers)
			entry.second.NextFrame();

		// The flush is what generates this update's init/fini events and graveyard entries, on top
		// of anything already recorded since the last update (e.g. by the editor between frames).
		FlushCommands();
	}

	void Scene::FrameEnd()
	{
		for (auto& entry : m_Reactive)
		{
			entry.second.InitSinceLastUpdate.clear();
			entry.second.FiniSinceLastUpdate.clear();
		}
		ClearGraveyards();
	}

	Scene::~Scene()
	{
		OnRuntimeStop();
	}

	void Scene::OnRuntimeStart()
	{
		m_Systems->OnRuntimeStart();
	}

	void Scene::OnRuntimeStop()
	{
		m_Systems->OnRuntimeStop();
	}

	template<typename Component>
	static void CopyComponent(entt::registry& dst, entt::registry& src, const std::unordered_map<UUID, entt::entity>& enttMap)
	{
		auto view = src.view<Component>();
		for (auto srcEntity : view)
		{
			UUID uuid = src.get<IDComponent>(srcEntity).ID;
			GE_CORE_ASSERT(enttMap.find(uuid) != enttMap.end(), "Entity UUID not found in map!");
			entt::entity dstEntity = enttMap.at(uuid);

			auto& component = src.get<Component>(srcEntity);
			dst.emplace_or_replace<Component>(dstEntity, component);
		}
	}

	Ref<Scene> Scene::Copy(Ref<Scene> other)
	{
		Ref<Scene> newScene = CreateRef<Scene>();

		newScene->m_ViewportWidth = other->m_ViewportWidth;
		newScene->m_ViewportHeight = other->m_ViewportHeight;

		auto& srcRegistry = other->m_Registry;
		auto& dstRegistry = newScene->m_Registry;
		std::unordered_map<UUID, entt::entity> enttMap;

		// Create entities with the same UUIDs
		auto idView = srcRegistry.view<IDComponent>();
		for (auto e : idView)
		{
			UUID uuid = srcRegistry.get<IDComponent>(e).ID;
			const auto& name = srcRegistry.get<TagComponent>(e).Tag;
			Entity newEntity = newScene->CreateEntityWithUUID(uuid, name);
			enttMap[uuid] = (entt::entity)newEntity;
		}

		// Copy components (skip ID and Tag — already set in CreateEntityWithUUID)
		ForEachType(ComponentList{}, [&](auto typeTag)
		{
			using T = typename decltype(typeTag)::Type;
			CopyComponent<T>(dstRegistry, srcRegistry, enttMap);
		});

		// Runtime script instances must be recreated on play
		{
			auto view = dstRegistry.view<NativeScriptComponent>();
			for (auto e : view)
			{
				auto& nsc = view.get<NativeScriptComponent>(e);
				nsc.Instance = nullptr;
			}
		}

		// Joint palettes are rebuilt every frame by AnimationSystem; copying them would carry a
		// per-joint matrix array per entity into the new scene for one frame's worth of nothing.
		{
			auto view = dstRegistry.view<AnimatorComponent>();
			for (auto e : view)
				view.get<AnimatorComponent>(e).Palette.clear();
		}

		return newScene;
	}

	Entity Scene::CreateEntity(const std::string& name)
	{
		return CreateEntityWithUUID(UUID(), name);
	}

	Entity Scene::CreateEntityWithUUID(UUID uuid, const std::string& name)
	{
		Entity entity = { m_Registry.create(), this };
		entity.AddComponent<IDComponent>(uuid);
		m_EntityMap[uuid] = (entt::entity)entity;
		entity.AddComponent<TransformComponent>();
		entity.AddComponent<WorldTransformComponent>();
		entity.AddComponent<RelationshipComponent>();
		auto& tag = entity.AddComponent<TagComponent>();
		tag.Tag = name.empty() ? "Entity" : name;
		return entity;
	}

	void Scene::RemoveChildFromParent(UUID parentID, UUID childID)
	{
		Entity parent = FindEntityByUUID(parentID);
		if (!parent)
			return;

		auto& children = parent.GetComponent<RelationshipComponent>().Children;
		children.erase(std::remove(children.begin(), children.end(), childID), children.end());
		MarkChanged<RelationshipComponent>(parent);
	}

	void Scene::Unparent(Entity child)
	{
		if (!child)
			return;

		auto& relationship = child.GetComponent<RelationshipComponent>();
		if (relationship.Parent == UUID{ 0 })
			return;

		RemoveChildFromParent(relationship.Parent, child.GetUUID());
		relationship.Parent = UUID{ 0 };
		MarkChanged<RelationshipComponent>(child);   // the child's world transform just changed
	}

	void Scene::SetParent(Entity child, Entity parent)
	{
		if (!child)
			return;

		// Prevent parenting to self or to a descendant
		if (parent)
		{
			if (child == parent)
				return;

			UUID childID = child.GetUUID();
			Entity current = parent;
			while (current)
			{
				if (current.GetUUID() == childID)
					return;

				UUID parentID = current.GetComponent<RelationshipComponent>().Parent;
				if (parentID == UUID{ 0 })
					break;
				current = FindEntityByUUID(parentID);
			}
		}

		Unparent(child);

		auto& childRel = child.GetComponent<RelationshipComponent>();
		if (!parent)
		{
			childRel.Parent = UUID{ 0 };
			MarkChanged<RelationshipComponent>(child);
			return;
		}

		childRel.Parent = parent.GetUUID();
		parent.GetComponent<RelationshipComponent>().Children.push_back(child.GetUUID());
		MarkChanged<RelationshipComponent>(child);
		MarkChanged<RelationshipComponent>(parent);
	}

	void Scene::DestroyEntity(Entity entity)
	{
		GE_CORE_ASSERT(!m_IsUpdating,
			"Immediate DestroyEntity during system update - use Scene::Commands().DestroyEntity()");

		if (!entity)
			return;

		UUID entityID = entity.GetUUID();
		auto& relationship = entity.GetComponent<RelationshipComponent>();

		// Detach from parent
		if (relationship.Parent != UUID{ 0 })
			RemoveChildFromParent(relationship.Parent, entityID);

		// Unparent children (keep them in the scene as roots)
		std::vector<UUID> children = relationship.Children;
		for (UUID childID : children)
		{
			Entity child = FindEntityByUUID(childID);
			if (!child)
				continue;

			child.GetComponent<RelationshipComponent>().Parent = UUID{ 0 };

			// The orphan's world transform just changed - it lost a parent's contribution -
			// but writing Parent through GetComponent is invisible to change tracking, so
			// TransformSystem never learned. The cache stayed at the old parented value
			// until something else happened to dirty the entity.
			MarkChanged<RelationshipComponent>(child);
		}

		m_EntityMap.erase(entityID);
		m_Registry.destroy(entity);
	}

	void Scene::CollectSubtree(Entity root, std::vector<Entity>& out, std::unordered_set<UUID>& visited)
	{
		if (!root)
			return;

		if (!visited.insert(root.GetUUID()).second)
			return;

		out.push_back(root);

		if (!root.HasComponent<RelationshipComponent>())
			return;

		// By value: nothing here mutates the vector, but a Children vector reached through a
		// component reference is exactly the kind of thing a caller ends up mutating mid-walk
		// (the DrawEntityNode lesson).
		std::vector<UUID> children = root.GetComponent<RelationshipComponent>().Children;
		for (UUID childID : children)
			CollectSubtree(FindEntityByUUID(childID), out, visited);
	}

	Entity Scene::DuplicateEntity(Entity source)
	{
		GE_CORE_ASSERT(!m_IsUpdating,
			"Immediate DuplicateEntity during system update - structural changes must be deferred");

		if (!source)
			return {};

		std::vector<Entity> subtree;
		std::unordered_set<UUID> visited;
		CollectSubtree(source, subtree, visited);

		// Two passes: every copy has to exist before any Relationship can be rewritten, since a
		// parent's Children names entities created later in the walk.
		std::unordered_map<UUID, UUID> remap;
		std::vector<Entity> copies;
		copies.reserve(subtree.size());

		for (Entity original : subtree)
		{
			Entity copy = CreateEntityWithUUID(UUID(), original.GetComponent<TagComponent>().Tag);
			remap[original.GetUUID()] = copy.GetUUID();
			copies.push_back(copy);
		}

		for (size_t i = 0; i < subtree.size(); i++)
		{
			auto src = (entt::entity)subtree[i];
			auto dst = (entt::entity)copies[i];

			// Verbatim for everything in ComponentList - which is what keeps the AssetHandle
			// fields intact. IDComponent and TagComponent are outside the list by design and are
			// handled above.
			ForEachType(ComponentList{}, [&](auto typeTag)
			{
				using T = typename decltype(typeTag)::Type;
				if (auto* component = m_Registry.try_get<T>(src))
					m_Registry.emplace_or_replace<T>(dst, *component);
			});

			// RelationshipComponent came across pointing at the *originals*. Rewrite it in the
			// copy's own terms; a child outside the subtree cannot exist, so an unmapped entry is
			// dropped rather than left dangling.
			auto& relationship = m_Registry.get<RelationshipComponent>(dst);

			std::vector<UUID> children;
			children.reserve(relationship.Children.size());
			for (UUID childID : relationship.Children)
			{
				auto it = remap.find(childID);
				if (it != remap.end())
					children.push_back(it->second);
			}
			relationship.Children = std::move(children);

			// The subtree root's parent is outside the copy, so it resolves to none here and is
			// re-attached below.
			auto parentIt = remap.find(relationship.Parent);
			relationship.Parent = parentIt != remap.end() ? parentIt->second : UUID{ 0 };

			// A native script instance is owned by the original; two components pointing at one
			// ScriptableEntity is a double delete waiting for the second teardown. Scene::Copy
			// nulls it for the same reason.
			if (auto* nsc = m_Registry.try_get<NativeScriptComponent>(dst))
				nsc->Instance = nullptr;

			// emplace_or_replace does not fire the on_construct signal the change tracker hooks,
			// so the copies would otherwise never reach TransformSystem and would render at
			// whatever the cache was seeded with.
			MarkChanged<TransformComponent>(copies[i]);
			MarkChanged<RelationshipComponent>(copies[i]);
		}

		// A duplicate is a *sibling* of its source, not a child of it.
		Entity root = copies.front();
		UUID sourceParent = source.GetComponent<RelationshipComponent>().Parent;
		if (sourceParent != UUID{ 0 })
			SetParent(root, FindEntityByUUID(sourceParent));

		return root;
	}

	void Scene::OnUpdateRuntime(Timestep ts, EditorCamera* fallbackCamera)
	{
		FrameBegin();

		// Used by RenderSystem only when the scene has no primary camera.
		GetSingleton<RenderContext>().EditorViewCamera = fallbackCamera;

		m_IsUpdating = true;
		m_Systems->OnUpdate(ts);
		m_IsUpdating = false;

		FrameEnd();
	}

	void Scene::OnUpdateEditor(Timestep ts, EditorCamera& camera)
	{
		FrameBegin();

		GetSingleton<RenderContext>().EditorViewCamera = &camera;

		m_IsUpdating = true;
		m_Systems->OnUpdateEditor(ts);
		m_IsUpdating = false;

		FrameEnd();
	}

	void Scene::OnViewportResize(uint32_t width, uint32_t height)
	{
		m_ViewportWidth = width;
		m_ViewportHeight = height;

		// Resize our non-FixedAspectRatio cameras
		auto view = m_Registry.view<CameraComponent>();
		for (auto entity : view)
		{
			auto& cameraComponent = view.get<CameraComponent>(entity);
			if (!cameraComponent.FixedAspectRatio)
				cameraComponent.Camera.SetViewportSize(width, height);
		}
	}

	Entity Scene::GetPrimaryCameraEntity()
	{
		auto view = m_Registry.view<CameraComponent>();
		for (auto entity : view)
		{
			const auto& camera = view.get<CameraComponent>(entity);
			if (camera.Primary)
				return Entity{ entity, this };
		}
		return {};
	}

	Entity Scene::FindEntityByUUID(UUID uuid)
	{
		auto it = m_EntityMap.find(uuid);
		if (it != m_EntityMap.end())
			return Entity{ it->second, this };
		return {};
	}

	glm::mat4 Scene::GetWorldSpaceTransform(Entity entity)
	{
		glm::mat4 transform = entity.GetComponent<TransformComponent>().GetLocalTransform();

		UUID parentID = entity.GetComponent<RelationshipComponent>().Parent;
		while (parentID != UUID{ 0 })
		{
			Entity parent = FindEntityByUUID(parentID);
			if (!parent)
				break;

			transform = parent.GetComponent<TransformComponent>().GetLocalTransform() * transform;
			parentID = parent.GetComponent<RelationshipComponent>().Parent;
		}

		return transform;
	}

	// The primary template lives in Scene.h and does nothing; only components needing post-add
	// fixup are specialized here (declared in Scene.h so every TU picks up the specialization).
	template<>
	void Scene::OnComponentAdded<CameraComponent>(const Entity& entity, CameraComponent& component)
	{
		if (m_ViewportWidth > 0 && m_ViewportHeight > 0)
			component.Camera.SetViewportSize(m_ViewportWidth, m_ViewportHeight);
	}
}

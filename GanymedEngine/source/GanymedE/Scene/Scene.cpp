#include "gepch.h"
#include "Scene.h"

#include "Entity.h"
#include "Components.h"
#include "GanymedE/Renderer/Renderer2D.h"

#include <glm/glm.hpp>

namespace GanymedE {

	Scene::Scene()
	{
	}

	Scene::~Scene()
	{
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
		CopyComponent<TransformComponent>(dstRegistry, srcRegistry, enttMap);
		CopyComponent<RelationshipComponent>(dstRegistry, srcRegistry, enttMap);
		CopyComponent<SpriteRendererComponent>(dstRegistry, srcRegistry, enttMap);
		CopyComponent<CameraComponent>(dstRegistry, srcRegistry, enttMap);
		CopyComponent<NativeScriptComponent>(dstRegistry, srcRegistry, enttMap);

		// Runtime script instances must be recreated on play
		{
			auto view = dstRegistry.view<NativeScriptComponent>();
			for (auto e : view)
			{
				auto& nsc = view.get<NativeScriptComponent>(e);
				nsc.Instance = nullptr;
			}
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
		entity.AddComponent<TransformComponent>();
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
			return;
		}

		childRel.Parent = parent.GetUUID();
		parent.GetComponent<RelationshipComponent>().Children.push_back(child.GetUUID());
	}

	void Scene::DestroyEntity(Entity entity)
	{
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
			if (child)
				child.GetComponent<RelationshipComponent>().Parent = UUID{ 0 };
		}

		m_Registry.destroy(entity);
	}

	void Scene::OnUpdateRuntime(Timestep ts)
	{
		// Update scripts
		{
			m_Registry.view<NativeScriptComponent>().each([=](auto entity, auto& nsc)
				{
					// TODO: Move to Scene::OnScenePlay
					if (!nsc.Instance)
					{
						nsc.Instance = nsc.InstantiateScript();
						nsc.Instance->m_Entity = Entity{ entity, this };

						nsc.Instance->OnCreate();
					}

					nsc.Instance->OnUpdate(ts);
				});
		}

		// Render 2D
		Camera* mainCamera = nullptr;
		glm::mat4 cameraTransform;
		{
			auto view = m_Registry.view<TransformComponent, CameraComponent>();
			for (auto entity : view)
			{
				auto [transform, camera] = view.get<TransformComponent, CameraComponent>(entity);

				if (camera.Primary)
				{
					mainCamera = &camera.Camera;
					cameraTransform = GetWorldSpaceTransform(Entity{ entity, this });
					break;
				}
			}
		}

		if (mainCamera)
		{
			Renderer2D::BeginScene(*mainCamera, cameraTransform);

			auto view = m_Registry.view<TransformComponent, SpriteRendererComponent>();
			for (auto entity : view)
			{
				auto [transform, sprite] = view.get<TransformComponent, SpriteRendererComponent>(entity);
				(void)transform;

				Renderer2D::DrawQuad(GetWorldSpaceTransform(Entity{ entity, this }), sprite.Color, (int)entity);
			}

			Renderer2D::EndScene();
		}
	}

	void Scene::OnUpdateEditor(Timestep ts, EditorCamera& camera)
	{
		Renderer2D::BeginScene(camera);

		auto view = m_Registry.view<TransformComponent, SpriteRendererComponent>();
		for (auto entity : view)
		{
			auto [transform, sprite] = view.get<TransformComponent, SpriteRendererComponent>(entity);
			(void)transform;

			Renderer2D::DrawQuad(GetWorldSpaceTransform(Entity{ entity, this }), sprite.Color, (int)entity);
		}

		Renderer2D::EndScene();
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
		auto view = m_Registry.view<IDComponent>();
		for (auto entity : view)
		{
			if (view.get<IDComponent>(entity).ID == uuid)
				return Entity{ entity, this };
		}
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

	template<typename T>
	void Scene::OnComponentAdded(Entity entity, T& component)
	{
		// Must depend on T: a plain static_assert(false) fires even when never instantiated on GCC/Clang
		static_assert(sizeof(T) == 0, "OnComponentAdded is not specialized for this component type!");
	}

	template<>
	void Scene::OnComponentAdded<IDComponent>(Entity entity, IDComponent& component)
	{
	}

	template<>
	void Scene::OnComponentAdded<TransformComponent>(Entity entity, TransformComponent& component)
	{
	}

	template<>
	void Scene::OnComponentAdded<RelationshipComponent>(Entity entity, RelationshipComponent& component)
	{
	}

	template<>
	void Scene::OnComponentAdded<CameraComponent>(Entity entity, CameraComponent& component)
	{
		if (m_ViewportWidth > 0 && m_ViewportHeight > 0)
			component.Camera.SetViewportSize(m_ViewportWidth, m_ViewportHeight);
	}

	template<>
	void Scene::OnComponentAdded<SpriteRendererComponent>(Entity entity, SpriteRendererComponent& component)
	{
	}

	template<>
	void Scene::OnComponentAdded<TagComponent>(Entity entity, TagComponent& component)
	{
	}

	template<>
	void Scene::OnComponentAdded<NativeScriptComponent>(Entity entity, NativeScriptComponent& component)
	{
	}
}

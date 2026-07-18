#include "gepch.h"
#include "Scene.h"

#include "Entity.h"
#include "Components.h"
#include "GanymedE/ECS/ComponentTraits.h"
#include "GanymedE/Assets/AssetManager.h"
#include "GanymedE/Renderer/Mesh.h"
#include "GanymedE/Renderer/Renderer2D.h"
#include "GanymedE/Renderer/Renderer3D.h"
#include "GanymedE/Renderer/Environment.h"
#include "GanymedE/Physics/PhysicsScene.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

namespace GanymedE {

	Scene::Scene()
	{
	}

	Scene::~Scene()
	{
		OnRuntimeStop();
	}

	void Scene::OnRuntimeStart()
	{
		InstantiateScripts();

		m_PhysicsScene = CreateScope<PhysicsScene>();
		m_PhysicsScene->Start(this);
		m_PhysicsAccumulator = 0.0f;
	}

	void Scene::OnRuntimeStop()
	{
		if (m_PhysicsScene)
		{
			m_PhysicsScene->Stop();
			m_PhysicsScene.reset();
		}
		m_PhysicsAccumulator = 0.0f;

		// Destroy script instances
		{
			auto view = m_Registry.view<NativeScriptComponent>();
			for (auto e : view)
			{
				auto& nsc = view.get<NativeScriptComponent>(e);
				if (nsc.Instance)
				{
					nsc.Instance->OnDestroy();
					nsc.DestroyScript(&nsc);
				}
			}
		}
	}

	void Scene::InstantiateScripts()
	{
		m_Registry.view<NativeScriptComponent>().each([=](auto entity, auto& nsc)
		{
			if (!nsc.Instance && nsc.InstantiateScript)
			{
				nsc.Instance = nsc.InstantiateScript();
				nsc.Instance->m_Entity = Entity{ entity, this };
				nsc.Instance->OnCreate();
			}
		});
	}

	void Scene::DispatchCollisionEvents()
	{
		if (!m_PhysicsScene)
			return;

		for (const auto& event : m_PhysicsScene->GetCollisionEvents())
		{
			Entity a = FindEntityByUUID(event.EntityA);
			Entity b = FindEntityByUUID(event.EntityB);
			if (!a || !b)
				continue;

			auto notify = [&](Entity self, Entity other)
			{
				if (!self.HasComponent<NativeScriptComponent>())
					return;
				auto& nsc = self.GetComponent<NativeScriptComponent>();
				if (!nsc.Instance)
					return;
				if (event.Entered)
					nsc.Instance->OnCollisionEnter(other);
				else
					nsc.Instance->OnCollisionExit(other);
			};

			notify(a, b);
			notify(b, a);
		}

		m_PhysicsScene->ClearCollisionEvents();
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

		m_EntityMap.erase(entityID);
		m_Registry.destroy(entity);
	}

	void Scene::OnUpdateRuntime(Timestep ts, EditorCamera* fallbackCamera)
	{
		// Fixed-timestep physics
		if (m_PhysicsScene && m_PhysicsScene->IsActive())
		{
			m_PhysicsAccumulator += ts;
			// Spiral-of-death guard
			constexpr int maxSteps = 5;
			int steps = 0;
			while (m_PhysicsAccumulator >= s_FixedTimestep && steps < maxSteps)
			{
				m_PhysicsScene->Step(s_FixedTimestep);
				DispatchCollisionEvents();
				m_PhysicsAccumulator -= s_FixedTimestep;
				steps++;
			}
			if (steps == maxSteps)
				m_PhysicsAccumulator = 0.0f;

			float alpha = m_PhysicsAccumulator / s_FixedTimestep;
			m_PhysicsScene->SyncTransforms(this, alpha);
		}

		// Update scripts
		{
			m_Registry.view<NativeScriptComponent>().each([=](auto entity, auto& nsc)
				{
					if (!nsc.Instance && nsc.InstantiateScript)
					{
						nsc.Instance = nsc.InstantiateScript();
						nsc.Instance->m_Entity = Entity{ entity, this };
						nsc.Instance->OnCreate();
					}

					if (nsc.Instance)
						nsc.Instance->OnUpdate(ts);
				});
		}

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

		auto renderScene3D = [&](const glm::vec3& cameraPos)
		{
			SubmitLightsAndSky();
			{
				auto view = m_Registry.view<TransformComponent, StaticMeshComponent>();
				for (auto entity : view)
				{
					auto [transform, meshComp] = view.get<TransformComponent, StaticMeshComponent>(entity);
					(void)transform;
					if (IsAssetHandleValid(meshComp.Mesh))
					{
						Ref<Mesh> mesh = AssetManager::GetAsset<Mesh>(meshComp.Mesh);
						if (mesh)
							Renderer3D::SubmitMesh(mesh, GetWorldSpaceTransform(Entity{ entity, this }), (int)entity);
					}
				}
			}

			// Prefer Jolt's view of the world when enabled; otherwise draw authored collider gizmos
			if (m_PhysicsScene && m_PhysicsScene->IsActive() && m_PhysicsDebugDraw.Enabled)
				m_PhysicsScene->DebugDraw(cameraPos, m_PhysicsDebugDraw);
			else
				DrawColliderGizmos();

			Renderer3D::EndScene();
		};

		if (mainCamera)
		{
			Renderer3D::BeginScene(*mainCamera, cameraTransform);
			renderScene3D(glm::vec3(cameraTransform[3]));

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
		else if (fallbackCamera)
		{
			// Editor convenience: Play with no scene Camera still shows the viewport
			Renderer3D::BeginScene(*fallbackCamera);
			renderScene3D(fallbackCamera->GetPosition());

			Renderer2D::BeginScene(*fallbackCamera);
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
		Renderer3D::BeginScene(camera);
		SubmitLightsAndSky();
		Renderer3D::DrawGrid();

		{
			auto view = m_Registry.view<TransformComponent, StaticMeshComponent>();
			for (auto entity : view)
			{
				auto [transform, meshComp] = view.get<TransformComponent, StaticMeshComponent>(entity);
				(void)transform;
				if (IsAssetHandleValid(meshComp.Mesh))
				{
					Ref<Mesh> mesh = AssetManager::GetAsset<Mesh>(meshComp.Mesh);
					if (mesh)
						Renderer3D::SubmitMesh(mesh, GetWorldSpaceTransform(Entity{ entity, this }), (int)entity);
				}
			}
		}
		DrawColliderGizmos();
		Renderer3D::EndScene();

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

	void Scene::DrawColliderGizmos()
	{
		const glm::vec4 boxColor{ 0.2f, 0.9f, 0.35f, 1.0f };
		const glm::vec4 sphereColor{ 0.3f, 0.7f, 1.0f, 1.0f };
		const glm::vec4 capsuleColor{ 1.0f, 0.75f, 0.2f, 1.0f };

		{
			auto view = m_Registry.view<TransformComponent, BoxColliderComponent>();
			for (auto entity : view)
			{
				Entity e{ entity, this };
				auto& col = e.GetComponent<BoxColliderComponent>();
				glm::mat4 world = GetWorldSpaceTransform(e);
				glm::mat4 collider = world
					* glm::translate(glm::mat4(1.0f), col.Offset)
					* glm::scale(glm::mat4(1.0f), col.HalfExtents * 2.0f);
				Renderer3D::DrawWireBox(collider, boxColor);
			}
		}
		{
			auto view = m_Registry.view<TransformComponent, SphereColliderComponent>();
			for (auto entity : view)
			{
				Entity e{ entity, this };
				auto& col = e.GetComponent<SphereColliderComponent>();
				glm::mat4 world = GetWorldSpaceTransform(e);
				glm::vec3 center = glm::vec3(world * glm::vec4(col.Offset, 1.0f));
				glm::vec3 scale = {
					glm::length(glm::vec3(world[0])),
					glm::length(glm::vec3(world[1])),
					glm::length(glm::vec3(world[2]))
				};
				float radius = col.Radius * glm::max(scale.x, glm::max(scale.y, scale.z));
				Renderer3D::DrawWireSphere(center, radius, sphereColor);
			}
		}
		{
			auto view = m_Registry.view<TransformComponent, CapsuleColliderComponent>();
			for (auto entity : view)
			{
				Entity e{ entity, this };
				auto& col = e.GetComponent<CapsuleColliderComponent>();
				glm::mat4 world = GetWorldSpaceTransform(e);
				glm::vec3 center = glm::vec3(world * glm::vec4(col.Offset, 1.0f));
				glm::vec3 scale = {
					glm::length(glm::vec3(world[0])),
					glm::length(glm::vec3(world[1])),
					glm::length(glm::vec3(world[2]))
				};
				float radius = col.Radius * glm::max(scale.x, scale.z);
				float halfHeight = col.HalfHeight * scale.y;
				glm::quat rotation = glm::normalize(glm::quat_cast(glm::mat3(
					glm::vec3(world[0]) / glm::max(scale.x, 1e-6f),
					glm::vec3(world[1]) / glm::max(scale.y, 1e-6f),
					glm::vec3(world[2]) / glm::max(scale.z, 1e-6f)
				)));
				Renderer3D::DrawWireCapsule(center, rotation, radius, halfHeight, capsuleColor);
			}
		}
	}

	void Scene::SubmitLightsAndSky()
	{
		// Directional lights (the first shadow-caster claims the shadow map)
		{
			auto view = m_Registry.view<TransformComponent, DirectionalLightComponent>();
			for (auto entity : view)
			{
				auto& light = view.get<DirectionalLightComponent>(entity);
				glm::mat4 world = GetWorldSpaceTransform(Entity{ entity, this });
				glm::vec3 dir = glm::normalize(glm::vec3(world * glm::vec4(0.0f, 0.0f, -1.0f, 0.0f)));
				Renderer3D::SubmitDirectionalLight(dir, light.Color, light.Intensity, light.CastShadows);
			}
		}

		// Point lights
		{
			auto view = m_Registry.view<TransformComponent, PointLightComponent>();
			for (auto entity : view)
			{
				auto& light = view.get<PointLightComponent>(entity);
				glm::mat4 world = GetWorldSpaceTransform(Entity{ entity, this });
				glm::vec3 position = glm::vec3(world[3]);
				Renderer3D::SubmitPointLight(position, light.Color, light.Intensity, light.Radius, light.Falloff);
			}
		}

		// Spot lights
		{
			auto view = m_Registry.view<TransformComponent, SpotLightComponent>();
			for (auto entity : view)
			{
				auto& light = view.get<SpotLightComponent>(entity);
				glm::mat4 world = GetWorldSpaceTransform(Entity{ entity, this });
				glm::vec3 position = glm::vec3(world[3]);
				glm::vec3 dir = glm::normalize(glm::vec3(world * glm::vec4(0.0f, 0.0f, -1.0f, 0.0f)));
				Renderer3D::SubmitSpotLight(position, dir, light.Color, light.Intensity, light.Range,
					glm::cos(light.InnerConeAngle), glm::cos(light.OuterConeAngle), light.Falloff);
			}
		}

		// Sky light / environment (first one wins)
		{
			auto view = m_Registry.view<SkyLightComponent>();
			for (auto entity : view)
			{
				auto& sky = view.get<SkyLightComponent>(entity);
				if (IsAssetHandleValid(sky.Environment))
				{
					Ref<Environment> environment = AssetManager::GetAsset<Environment>(sky.Environment);
					if (environment && environment->IsValid())
						Renderer3D::SubmitEnvironment(environment, sky.Intensity, sky.DrawSkybox);
					else
						Renderer3D::SubmitSkyLight(sky.SkyColor, sky.GroundColor, sky.Intensity, sky.DrawSkybox);
				}
				else
				{
					Renderer3D::SubmitSkyLight(sky.SkyColor, sky.GroundColor, sky.Intensity, sky.DrawSkybox);
				}
				break;
			}
		}

		Renderer3D::DrawSkybox();
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
	void Scene::OnComponentAdded<CameraComponent>(Entity entity, CameraComponent& component)
	{
		if (m_ViewportWidth > 0 && m_ViewportHeight > 0)
			component.Camera.SetViewportSize(m_ViewportWidth, m_ViewportHeight);
	}
}

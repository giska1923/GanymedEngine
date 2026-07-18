#include "gepch.h"
#include "RenderSystem.h"

#include "PhysicsSystem.h"
#include "GanymedE/ECS/Singleton.h"
#include "GanymedE/Scene/SceneSingletons.h"
#include "GanymedE/Assets/AssetManager.h"
#include "GanymedE/Renderer/EditorCamera.h"
#include "GanymedE/Renderer/Environment.h"
#include "GanymedE/Renderer/Mesh.h"
#include "GanymedE/Renderer/Renderer2D.h"
#include "GanymedE/Renderer/Renderer3D.h"
#include "GanymedE/Scene/Entity.h"
#include "GanymedE/Scene/Scene.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

namespace GanymedE {

	void RenderSystem::SubmitLightsAndSky()
	{
		// Directional lights (the first shadow-caster claims the shadow map)
		for (auto [entity, worldTransform, light] : View<DirLightView>())
		{
			(void)entity;
			const glm::mat4& world = worldTransform.World;
			glm::vec3 direction = glm::normalize(glm::vec3(world * glm::vec4(0.0f, 0.0f, -1.0f, 0.0f)));
			Renderer3D::SubmitDirectionalLight(direction, light.Color, light.Intensity, light.CastShadows);
		}

		// Point lights
		for (auto [entity, worldTransform, light] : View<PointLightView>())
		{
			(void)entity;
			const glm::mat4& world = worldTransform.World;
			glm::vec3 position = glm::vec3(world[3]);
			Renderer3D::SubmitPointLight(position, light.Color, light.Intensity, light.Radius, light.Falloff);
		}

		// Spot lights
		for (auto [entity, worldTransform, light] : View<SpotLightView>())
		{
			(void)entity;
			const glm::mat4& world = worldTransform.World;
			glm::vec3 position = glm::vec3(world[3]);
			glm::vec3 direction = glm::normalize(glm::vec3(world * glm::vec4(0.0f, 0.0f, -1.0f, 0.0f)));
			Renderer3D::SubmitSpotLight(position, direction, light.Color, light.Intensity, light.Range,
				glm::cos(light.InnerConeAngle), glm::cos(light.OuterConeAngle), light.Falloff);
		}

		// Sky light / environment (first one wins) — Phase 7 resolves this into a singleton once
		// instead of re-deciding every frame.
		for (auto [sky] : View<SkyView>())
		{
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

		Renderer3D::DrawSkybox();
	}

	void RenderSystem::SubmitMeshes()
	{
		for (auto [entity, worldTransform, meshComponent] : View<MeshView>())
		{
			if (!IsAssetHandleValid(meshComponent.Mesh))
				continue;

			Ref<Mesh> mesh = AssetManager::GetAsset<Mesh>(meshComponent.Mesh);
			if (mesh)
				Renderer3D::SubmitMesh(mesh, worldTransform.World, (int)entity);
		}
	}

	void RenderSystem::SubmitSprites()
	{
		for (auto [entity, worldTransform, sprite] : View<SpriteView>())
			Renderer2D::DrawQuad(worldTransform.World, sprite.Color, (int)entity);
	}

	void RenderSystem::DrawColliderGizmos()
	{
		const glm::vec4 boxColor{ 0.2f, 0.9f, 0.35f, 1.0f };
		const glm::vec4 sphereColor{ 0.3f, 0.7f, 1.0f, 1.0f };
		const glm::vec4 capsuleColor{ 1.0f, 0.75f, 0.2f, 1.0f };

		for (auto [entity, worldTransform, collider] : View<BoxColliderView>())
		{
			(void)entity;
			const glm::mat4& world = worldTransform.World;
			glm::mat4 colliderTransform = world
				* glm::translate(glm::mat4(1.0f), collider.Offset)
				* glm::scale(glm::mat4(1.0f), collider.HalfExtents * 2.0f);
			Renderer3D::DrawWireBox(colliderTransform, boxColor);
		}

		for (auto [entity, worldTransform, collider] : View<SphereColliderView>())
		{
			(void)entity;
			const glm::mat4& world = worldTransform.World;
			glm::vec3 center = glm::vec3(world * glm::vec4(collider.Offset, 1.0f));
			glm::vec3 scale = {
				glm::length(glm::vec3(world[0])),
				glm::length(glm::vec3(world[1])),
				glm::length(glm::vec3(world[2]))
			};
			float radius = collider.Radius * glm::max(scale.x, glm::max(scale.y, scale.z));
			Renderer3D::DrawWireSphere(center, radius, sphereColor);
		}

		for (auto [entity, worldTransform, collider] : View<CapsuleColliderView>())
		{
			(void)entity;
			const glm::mat4& world = worldTransform.World;
			glm::vec3 center = glm::vec3(world * glm::vec4(collider.Offset, 1.0f));
			glm::vec3 scale = {
				glm::length(glm::vec3(world[0])),
				glm::length(glm::vec3(world[1])),
				glm::length(glm::vec3(world[2]))
			};
			float radius = collider.Radius * glm::max(scale.x, scale.z);
			float halfHeight = collider.HalfHeight * scale.y;
			glm::quat rotation = glm::normalize(glm::quat_cast(glm::mat3(
				glm::vec3(world[0]) / glm::max(scale.x, 1e-6f),
				glm::vec3(world[1]) / glm::max(scale.y, 1e-6f),
				glm::vec3(world[2]) / glm::max(scale.z, 1e-6f)
			)));
			Renderer3D::DrawWireCapsule(center, rotation, radius, halfHeight, capsuleColor);
		}
	}

	void RenderSystem::DrawPhysicsDebugOrGizmos(const glm::vec3& cameraPosition)
	{
		PhysicsScene* physics = nullptr;
		if (PhysicsSystem* physicsSystem = m_Scene.Systems().Get<PhysicsSystem>())
			physics = physicsSystem->GetPhysicsScene();

		ECS::SingletonAccessView<PhysicsSettings> settingsView{ m_Scene };
		const PhysicsDebugDrawSettings& settings = settingsView.Get()->DebugDraw;

		// Prefer Jolt's view of the world when enabled; otherwise draw authored collider gizmos
		if (physics && physics->IsActive() && settings.Enabled)
			physics->DebugDraw(cameraPosition, settings);
		else
			DrawColliderGizmos();
	}

	void RenderSystem::OnUpdate(Timestep ts)
	{
		(void)ts;

		ECS::SingletonAccessView<RenderContext> renderView{ m_Scene };
		const RenderContext& context = *renderView.Get();

		auto renderScene3D = [&](const glm::vec3& cameraPosition)
		{
			SubmitLightsAndSky();
			SubmitMeshes();
			DrawPhysicsDebugOrGizmos(cameraPosition);
			Renderer3D::EndScene();
		};

		if (context.MainCamera)
		{
			Renderer3D::BeginScene(*context.MainCamera, context.CameraTransform);
			renderScene3D(glm::vec3(context.CameraTransform[3]));

			Renderer2D::BeginScene(*context.MainCamera, context.CameraTransform);
			SubmitSprites();
			Renderer2D::EndScene();
		}
		else if (EditorCamera* fallbackCamera = context.EditorViewCamera)
		{
			// Editor convenience: Play with no scene Camera still shows the viewport
			Renderer3D::BeginScene(*fallbackCamera);
			renderScene3D(fallbackCamera->GetPosition());

			Renderer2D::BeginScene(*fallbackCamera);
			SubmitSprites();
			Renderer2D::EndScene();
		}
	}

	void RenderSystem::OnUpdateEditor(Timestep ts)
	{
		(void)ts;

		ECS::SingletonAccessView<RenderContext> renderView{ m_Scene };
		EditorCamera* camera = renderView.Get()->EditorViewCamera;
		GE_CORE_ASSERT(camera, "Editor update without an active editor camera");
		if (!camera)
			return;

		Renderer3D::BeginScene(*camera);
		SubmitLightsAndSky();
		Renderer3D::DrawGrid();
		SubmitMeshes();
		DrawColliderGizmos();
		Renderer3D::EndScene();

		Renderer2D::BeginScene(*camera);
		SubmitSprites();
		Renderer2D::EndScene();
	}
}

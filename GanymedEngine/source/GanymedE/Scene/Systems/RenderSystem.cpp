#include "gepch.h"
#include "RenderSystem.h"

#include "PhysicsSystem.h"
#include "GanymedE/ECS/Singleton.h"
#include "GanymedE/Scene/SceneSingletons.h"
#include "GanymedE/Assets/AssetManager.h"
#include "GanymedE/Renderer/EditorCamera.h"
#include "GanymedE/Renderer/Environment.h"
#include "GanymedE/Renderer/Mesh.h"
#include "GanymedE/Renderer/ParticleRenderer.h"
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
			// Resolved through the component's own reference: the first frame loads, every
			// frame after is a pointer already in the component. This used to be a handle -> Ref
			// hash lookup per entity per frame, for every asset on this page.
			const Ref<Environment>& environment = sky.Environment.Get();
			if (environment && environment->IsValid())
				Renderer3D::SubmitEnvironment(environment, sky.Intensity, sky.DrawSkybox);
			else
				Renderer3D::SubmitSkyLight(sky.SkyColor, sky.GroundColor, sky.Intensity, sky.DrawSkybox);
			break;
		}

		Renderer3D::DrawSkybox();
	}

	void RenderSystem::SubmitMeshes()
	{
		for (auto [entity, worldTransform, meshComponent, animator] : View<MeshView>())
		{
			const Ref<Mesh>& mesh = meshComponent.Mesh.Get();
			if (!mesh)
				continue;

			// Reload still lands in the viewport on the next frame with no reverse
			// material-to-mesh index: eviction bumps the epoch every AssetRef checks, so the
			// slot re-resolves itself. That is what replaced "re-fetch by handle every frame".
			//
			// The scratch vector exists because Renderer3D takes a contiguous
			// `const Ref<Material>*` and AssetRef is not layout-compatible with Ref. It is
			// reused across entities: an override-heavy scene would otherwise allocate once
			// per mesh per frame.
			m_ResolvedOverrides.clear();
			for (const AssetRef<Material>& slot : meshComponent.MaterialOverrides)
				m_ResolvedOverrides.push_back(slot.Get());

			const Ref<Material>* overrides = m_ResolvedOverrides.empty() ? nullptr : m_ResolvedOverrides.data();
			const uint32_t overrideCount = (uint32_t)m_ResolvedOverrides.size();

			// An entity is skinned iff its mesh has a skeleton and it has an animator -
			// the same gate the AnimationSystem poses on. A rigged mesh with no animator
			// draws as static geometry in its bind pose, which is the sane default for
			// dropping a character into a scene before authoring anything.
			if (animator && mesh->HasSkeleton() && !animator->Palette.empty())
			{
				Renderer3D::SubmitSkinnedMesh(mesh, worldTransform.World,
					animator->Palette.data(), (uint32_t)animator->Palette.size(), (int)entity,
					overrides, overrideCount);
			}
			else
			{
				Renderer3D::SubmitMesh(mesh, worldTransform.World, (int)entity, overrides, overrideCount);
			}
		}
	}

	void RenderSystem::SubmitParticles(const glm::vec3& cameraPosition, const glm::vec3& cameraRight,
		const glm::vec3& cameraUp)
	{
		ParticleRenderer::SetView(cameraPosition, cameraRight, cameraUp);

		for (auto [entity, worldTransform, emitter] : View<ParticleView>())
		{
			if (emitter.RenderMode == ParticleEmitterComponent::Mode::Mesh)
			{
				if (!Renderer3D::FrustumIntersects(emitter.WorldBounds))
				{
					Renderer3D::AddCulledParticleEmitter();
					continue;
				}

				if (emitter.Pool.empty())
					continue;

				const Ref<Mesh>& mesh = emitter.Mesh.Get();
				if (!mesh)
					continue;

				const Ref<Material>& forced = emitter.Material.Get();

				const glm::mat4& world = worldTransform.World;
				for (const Particle& p : emitter.Pool)
				{
					const float life = p.Lifetime > 0.0f
						? glm::clamp(p.Age / p.Lifetime, 0.0f, 1.0f) : 1.0f;
					const float size = p.StartSize * emitter.SizeCurve.Sample(life);
					if (size <= 0.0f)
						continue;

					glm::mat4 local(1.0f);
					local = glm::translate(local, p.Position);
					local = glm::rotate(local, glm::radians(p.Rotation), glm::vec3(0.0f, 1.0f, 0.0f));
					local = glm::scale(local, glm::vec3(size));
					const glm::mat4 transform = emitter.WorldSpace ? local : world * local;

					if (forced)
					{
						const auto& submeshes = mesh->GetSubmeshes();
						for (uint32_t i = 0; i < (uint32_t)submeshes.size(); i++)
							Renderer3D::SubmitMesh(mesh, i, forced, transform, (int)entity);
					}
					else
					{
						Renderer3D::SubmitMesh(mesh, transform, (int)entity);
					}
				}
				continue;
			}

			ParticleRenderer::Submit((int)entity, emitter, worldTransform.World);
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
		const PhysicsSettings& settings = *settingsView.Get();

		// Prefer Jolt's view of the world when enabled; otherwise draw authored collider
		// gizmos, but only for a host that asked for them. This used to fall through
		// unconditionally, which meant any non-editor front-end drew collider wireframes
		// over the game.
		if (physics && physics->IsActive() && settings.DebugDraw.Enabled)
			physics->DebugDraw(cameraPosition, settings.DebugDraw);
		else if (settings.ShowColliderGizmos)
			DrawColliderGizmos();
	}

	void RenderSystem::OnUpdate(Timestep ts)
	{
		ECS::SingletonAccessView<RenderContext> renderView{ m_Scene };
		const RenderContext& context = *renderView.Get();

		auto renderScene3D = [&](const glm::vec3& cameraPosition, const glm::vec3& cameraRight,
			const glm::vec3& cameraUp)
		{
			SubmitLightsAndSky();
			SubmitMeshes();
			SubmitParticles(cameraPosition, cameraRight, cameraUp);
			DrawPhysicsDebugOrGizmos(cameraPosition);
			Renderer3D::EndScene();
		};

		if (context.MainCamera)
		{
			const glm::mat4& t = context.CameraTransform;
			glm::vec3 right = glm::vec3(t[0]);
			glm::vec3 up = glm::vec3(t[1]);
			const float rl = glm::length(right);
			const float ul = glm::length(up);
			right = rl > 0.0f ? right / rl : glm::vec3(1.0f, 0.0f, 0.0f);
			up = ul > 0.0f ? up / ul : glm::vec3(0.0f, 1.0f, 0.0f);

			Renderer3D::BeginScene(*context.MainCamera, context.CameraTransform);
			renderScene3D(glm::vec3(t[3]), right, up);

			Renderer2D::BeginScene(*context.MainCamera, context.CameraTransform);
			SubmitSprites();
			Renderer2D::EndScene();
		}
		else if (EditorCamera* fallbackCamera = context.EditorViewCamera)
		{
			// Editor convenience: Play with no scene Camera still shows the viewport
			Renderer3D::BeginScene(*fallbackCamera);
			renderScene3D(fallbackCamera->GetPosition(), fallbackCamera->GetRightDirection(),
				fallbackCamera->GetUpDirection());

			Renderer2D::BeginScene(*fallbackCamera);
			SubmitSprites();
			Renderer2D::EndScene();
		}
		else
		{
			// No primary camera and no fallback: the scene target still gets cleared, so
			// the frame is the clear colour rather than garbage. Say why, loudly but not
			// 60 times a second - a per-frame error would bury everything else in the log
			// while telling you the same thing.
			m_NoCameraLogTimer += ts;
			if (m_NoCameraLogTimer >= kNoCameraLogInterval)
			{
				m_NoCameraLogTimer = 0.0f;
				GE_CORE_ERROR("Scene has no primary camera and no fallback view camera - "
					"rendering the clear colour only. Add a CameraComponent marked Primary.");
			}
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
		SubmitParticles(camera->GetPosition(), camera->GetRightDirection(), camera->GetUpDirection());
		DrawColliderGizmos();
		Renderer3D::EndScene();

		Renderer2D::BeginScene(*camera);
		SubmitSprites();
		Renderer2D::EndScene();
	}
}

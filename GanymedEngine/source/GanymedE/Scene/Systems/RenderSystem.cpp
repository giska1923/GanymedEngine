#include "gepch.h"
#include "RenderSystem.h"

#include "PhysicsSystem.h"
#include "GanymedE/ECS/Singleton.h"
#include "GanymedE/Scene/SceneSingletons.h"
#include "GanymedE/Assets/AssetManager.h"
#include "GanymedE/Renderer/EditorCamera.h"
#include "GanymedE/Renderer/Camera.h"
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
		GE_PROFILE_FUNCTION();

		// Directional lights (the first shadow-caster claims the shadow map)
		for (auto [entity, worldTransform, light] : View<DirLightView>())
		{
			if (IsEditorHidden(entity))
				continue;
			const glm::mat4& world = worldTransform.World;
			glm::vec3 direction = glm::normalize(glm::vec3(world * glm::vec4(0.0f, 0.0f, -1.0f, 0.0f)));
			Renderer3D::SubmitDirectionalLight(direction, light.Color, light.Intensity, light.CastShadows);
		}

		// Point lights
		for (auto [entity, worldTransform, light] : View<PointLightView>())
		{
			if (IsEditorHidden(entity))
				continue;
			const glm::mat4& world = worldTransform.World;
			glm::vec3 position = glm::vec3(world[3]);
			Renderer3D::SubmitPointLight(position, light.Color, light.Intensity, light.Radius, light.Falloff);
		}

		// Spot lights
		for (auto [entity, worldTransform, light] : View<SpotLightView>())
		{
			if (IsEditorHidden(entity))
				continue;
			const glm::mat4& world = worldTransform.World;
			glm::vec3 position = glm::vec3(world[3]);
			glm::vec3 direction = glm::normalize(glm::vec3(world * glm::vec4(0.0f, 0.0f, -1.0f, 0.0f)));
			Renderer3D::SubmitSpotLight(position, direction, light.Color, light.Intensity, light.Range,
				glm::cos(light.InnerConeAngle), glm::cos(light.OuterConeAngle), light.Falloff);
		}

		// Sky light / environment (first one wins) — Phase 7 resolves this into a singleton once
		// instead of re-deciding every frame.
		for (auto [entity, sky] : View<SkyView>())
		{
			if (IsEditorHidden(entity))
				continue;
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
		GE_PROFILE_FUNCTION();

		for (auto [entity, worldTransform, meshComponent, animator] : View<MeshView>())
		{
			if (IsEditorHidden(entity))
				continue;
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

			// HasSkeleton() always goes through vs_PhongSkinned. SubmitMesh applies
			// LocalTransform with no palette; on a Meshy rig that is 0.01 and the
			// character draws at ~2 cm. The rest palette (Global * InverseBind at rest)
			// is what cancels that scale. An animator palette, when present and sized
			// to the rig, replaces it - ResolvePosePalette owns that choice, so sockets
			// and the skeleton overlay read the same pose this draws.
			if (mesh->HasSkeleton())
			{
				const std::vector<glm::mat4>& palette =
					ResolvePosePalette(*mesh, animator ? &animator->Palette : nullptr);
				Renderer3D::SubmitSkinnedMesh(mesh, worldTransform.World,
					palette.empty() ? nullptr : palette.data(), (uint32_t)palette.size(),
					(int)entity, overrides, overrideCount);
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
		GE_PROFILE_FUNCTION();

		ParticleRenderer::SetView(cameraPosition, cameraRight, cameraUp);

		for (auto [entity, worldTransform, emitter] : View<ParticleView>())
		{
			if (IsEditorHidden(entity))
				continue;
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
		GE_PROFILE_FUNCTION();

		for (auto [entity, worldTransform, sprite] : View<SpriteView>())
		{
			if (IsEditorHidden(entity))
				continue;
			Renderer2D::DrawQuad(worldTransform.World, sprite.Color, (int)entity);
		}
	}

	void RenderSystem::DrawColliderGizmos()
	{
		GE_PROFILE_FUNCTION();

		const glm::vec4 boxColor{ 0.2f, 0.9f, 0.35f, 1.0f };
		const glm::vec4 sphereColor{ 0.3f, 0.7f, 1.0f, 1.0f };
		const glm::vec4 capsuleColor{ 1.0f, 0.75f, 0.2f, 1.0f };

		for (auto [entity, worldTransform, collider] : View<BoxColliderView>())
		{
			if (IsEditorHidden(entity))
				continue;
			const glm::mat4& world = worldTransform.World;
			glm::mat4 colliderTransform = world
				* glm::translate(glm::mat4(1.0f), collider.Offset)
				* glm::scale(glm::mat4(1.0f), collider.HalfExtents * 2.0f);
			Renderer3D::DrawWireBox(colliderTransform, boxColor);
		}

		for (auto [entity, worldTransform, collider] : View<SphereColliderView>())
		{
			if (IsEditorHidden(entity))
				continue;
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
			if (IsEditorHidden(entity))
				continue;
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

	void RenderSystem::DrawMarkerGizmos()
	{
		GE_PROFILE_FUNCTION();

		ECS::SingletonAccessView<PhysicsSettings> settingsView{ m_Scene };
		if (!settingsView.Get()->ShowMarkers)
			return;

		for (auto [entity, worldTransform, marker] : View<MarkerView>())
		{
			if (IsEditorHidden(entity))
				continue;

			const glm::mat4& world = worldTransform.World;
			const glm::vec3 center = glm::vec3(world[3]);
			const float scale = glm::max(glm::length(glm::vec3(world[0])),
				glm::max(glm::length(glm::vec3(world[1])), glm::length(glm::vec3(world[2]))));
			const float radius = marker.Size * glm::max(scale, 1.0e-6f);

			// 16 segments: three rings still flush as one debug-line batch in EndScene.
			Renderer3D::DrawWireSphere(center, radius, marker.Color, 16);

			if (!marker.DrawForward)
				continue;

			glm::vec3 axis = glm::vec3(world[2]);
			const float length = glm::length(axis);
			if (length < 1.0e-6f)
				continue;

			const glm::vec3 forward = -axis / length;
			Renderer3D::DrawLine(center, center + forward * radius, marker.Color);
		}
	}

	namespace {

		bool WalkHits(Scene& scene, Entity start, UUID target, std::unordered_set<entt::entity>& seen)
		{
			Entity entity = start;
			while (entity)
			{
				if (!seen.insert((entt::entity)entity).second)
					break;
				if (entity.GetUUID() == target)
					return true;
				UUID parentID = entity.GetComponent<RelationshipComponent>().Parent;
				if (parentID == UUID{ 0 })
					break;
				entity = scene.FindEntityByUUID(parentID);
			}
			return false;
		}

		glm::vec3 JointOrigin(const glm::mat4& world)
		{
			return glm::vec3(world[3]);
		}

		glm::vec3 JointAxis(const glm::mat4& world, int column)
		{
			glm::vec3 axis = glm::vec3(world[column]);
			const float len = glm::length(axis);
			if (len > 1e-6f)
				return axis / len;
			return glm::vec3(column == 0 ? 1.0f : 0.0f, column == 1 ? 1.0f : 0.0f, column == 2 ? 1.0f : 0.0f);
		}

	}

	bool RenderSystem::SkeletonInSelection(Scene& scene, Entity skinned,
		const std::unordered_set<UUID>& selected)
	{
		std::unordered_set<entt::entity> seen;
		Entity walk = skinned;
		while (walk)
		{
			if (!seen.insert((entt::entity)walk).second)
				break;
			if (selected.count(walk.GetUUID()) != 0)
				return true;
			UUID parentID = walk.GetComponent<RelationshipComponent>().Parent;
			if (parentID == UUID{ 0 })
				break;
			walk = scene.FindEntityByUUID(parentID);
		}

		const UUID skinnedID = skinned.GetUUID();
		for (UUID id : selected)
		{
			Entity entity = scene.FindEntityByUUID(id);
			if (!entity)
				continue;
			seen.clear();
			if (WalkHits(scene, entity, skinnedID, seen))
				return true;
		}
		return false;
	}

	void RenderSystem::DrawSkeletonGizmos()
	{
		GE_PROFILE_FUNCTION();

		ECS::SingletonAccessView<PhysicsSettings> settingsView{ m_Scene };
		const PhysicsSettings& settings = *settingsView.Get();
		if (!settings.ShowSkeletons)
			return;

		const EditorViewFilter* filter = m_Scene.FindSingleton<EditorViewFilter>();
		const std::unordered_set<UUID>* selected = filter ? filter->SelectedEntities : nullptr;
		if (!settings.ShowAllSkeletons && (!selected || selected->empty()))
			return;

		const bool depthTest = !settings.SkeletonXRay;
		const glm::vec4 boneColor{ 0.45f, 0.78f, 0.95f, 1.0f };
		const glm::vec4 markerColor{ 0.85f, 0.92f, 1.0f, 1.0f };
		const glm::vec4 accent{ 0.694f, 0.510f, 0.929f, 1.0f };
		const glm::vec4 axisX{ 0.92f, 0.28f, 0.28f, 1.0f };
		const glm::vec4 axisY{ 0.32f, 0.82f, 0.38f, 1.0f };
		const glm::vec4 axisZ{ 0.32f, 0.48f, 0.95f, 1.0f };

		auto line = [&](const glm::vec3& a, const glm::vec3& b, const glm::vec4& color)
		{
			Renderer3D::DrawLine(a, b, color, depthTest);
		};

		const UUID highlightEntity = filter ? filter->HighlightSkeletonEntity : UUID{ 0 };
		const int32_t highlightJoint = filter ? filter->HighlightJoint : -1;

		for (auto [entity, worldTransform, meshComponent, animator] : View<MeshView>())
		{
			if (IsEditorHidden(entity))
				continue;

			const Ref<Mesh>& mesh = meshComponent.Mesh.Get();
			if (!mesh || !mesh->HasSkeleton())
				continue;

			Entity handle{ entity, &m_Scene };
			if (!settings.ShowAllSkeletons && !SkeletonInSelection(m_Scene, handle, *selected))
				continue;

			// The pose this entity is drawn in - an unanimated rig shows its rest skeleton.
			const std::vector<glm::mat4>& palette =
				ResolvePosePalette(*mesh, animator ? &animator->Palette : nullptr);
			const Skeleton& skeleton = mesh->GetSkeleton();
			const uint32_t jointCount = skeleton.JointCount();
			if (jointCount == 0 || palette.size() != jointCount)
				continue;

			m_JointWorld.resize(jointCount);
			m_JointOk.assign(jointCount, 0);
			m_JointHasChild.assign(jointCount, 0);
			m_BoneLength.assign(jointCount, 0.0f);

			const glm::mat4& entityWorld = worldTransform.World;
			for (uint32_t i = 0; i < jointCount; i++)
			{
				glm::mat4 local{ 1.0f };
				if (!TryGetJointFrame(*mesh, palette, (int32_t)i, local))
					continue;
				m_JointWorld[i] = entityWorld * local;
				m_JointOk[i] = 1;
			}

			for (uint32_t i = 0; i < jointCount; i++)
			{
				if (!m_JointOk[i])
					continue;
				const int32_t parent = skeleton.ParentIndices[i];
				if (parent >= 0 && (uint32_t)parent < jointCount && m_JointOk[(uint32_t)parent])
				{
					m_JointHasChild[(uint32_t)parent] = 1;
					m_BoneLength[i] = glm::distance(
						JointOrigin(m_JointWorld[(uint32_t)parent]), JointOrigin(m_JointWorld[i]));
				}
			}

			float meanBone = 0.0f;
			uint32_t meanCount = 0;
			for (uint32_t i = 0; i < jointCount; i++)
			{
				if (m_BoneLength[i] > 1e-6f)
				{
					meanBone += m_BoneLength[i];
					++meanCount;
				}
			}
			if (meanCount > 0)
				meanBone /= (float)meanCount;
			else
				meanBone = 0.05f;

			const bool thisHighlight = highlightEntity != UUID{ 0 }
				&& handle.GetUUID() == highlightEntity;

			for (uint32_t i = 0; i < jointCount; i++)
			{
				if (!m_JointOk[i])
					continue;

				const glm::vec3 origin = JointOrigin(m_JointWorld[i]);
				const bool selectedJoint = thisHighlight && highlightJoint == (int32_t)i;
				const glm::vec4 color = selectedJoint ? accent : boneColor;

				const int32_t parent = skeleton.ParentIndices[i];
				if (parent >= 0 && (uint32_t)parent < jointCount && m_JointOk[(uint32_t)parent])
					line(JointOrigin(m_JointWorld[(uint32_t)parent]), origin, color);

				float boneLen = m_BoneLength[i];
				if (boneLen < 1e-6f)
					boneLen = meanBone;
				const float marker = glm::clamp(boneLen * 0.15f, 0.012f, 0.07f);

				const glm::vec4 mark = selectedJoint ? accent : markerColor;
				line(origin - JointAxis(m_JointWorld[i], 0) * marker,
					origin + JointAxis(m_JointWorld[i], 0) * marker, mark);
				line(origin - JointAxis(m_JointWorld[i], 1) * marker,
					origin + JointAxis(m_JointWorld[i], 1) * marker, mark);
				line(origin - JointAxis(m_JointWorld[i], 2) * marker,
					origin + JointAxis(m_JointWorld[i], 2) * marker, mark);

				if (!m_JointHasChild[i])
				{
					const float stub = boneLen * 0.4f;
					line(origin, origin + JointAxis(m_JointWorld[i], 1) * stub, color);
				}

				if (selectedJoint)
				{
					const float triad = glm::max(marker * 2.5f, 0.08f);
					line(origin, origin + JointAxis(m_JointWorld[i], 0) * triad, axisX);
					line(origin, origin + JointAxis(m_JointWorld[i], 1) * triad, axisY);
					line(origin, origin + JointAxis(m_JointWorld[i], 2) * triad, axisZ);
				}
			}
		}
	}

	void RenderSystem::DrawPhysicsDebugOrGizmos(const glm::vec3& cameraPosition)
	{
		GE_PROFILE_FUNCTION();

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
		m_EditorHidden.clear();
		ECS::SingletonAccessView<RenderContext> renderView{ m_Scene };
		const RenderContext& context = *renderView.Get();

		auto renderScene3D = [&](const glm::vec3& cameraPosition, const glm::vec3& cameraRight,
			const glm::vec3& cameraUp)
		{
			SubmitLightsAndSky();
			SubmitMeshes();
			SubmitParticles(cameraPosition, cameraRight, cameraUp);
			DrawPhysicsDebugOrGizmos(cameraPosition);
			DrawMarkerGizmos();
			DrawSkeletonGizmos();
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

		RebuildEditorHidden();

		ECS::SingletonAccessView<RenderContext> renderView{ m_Scene };
		const auto ctx = renderView.Get();
		EditorCamera* editorCam = ctx->EditorViewCamera;
		GE_CORE_ASSERT(editorCam, "Editor update without an active editor camera");
		if (!editorCam)
			return;

		// Viewport dropdown: look through a scene CameraComponent when PreviewCamera
		// resolves. Stale UUIDs (deleted camera) fall back to the editor camera.
		const Camera* previewCam = nullptr;
		glm::mat4 previewWorld{ 1.0f };
		if (ctx->PreviewCamera != UUID{ 0 })
		{
			Entity entity = m_Scene.FindEntityByUUID(ctx->PreviewCamera);
			if (entity && entity.HasComponent<CameraComponent>())
			{
				previewCam = &entity.GetComponent<CameraComponent>().Camera;
				previewWorld = m_Scene.GetWorldSpaceTransform(entity);
			}
		}

		glm::vec3 camPos, camRight, camUp;
		if (previewCam)
		{
			Renderer3D::BeginScene(*previewCam, previewWorld);
			camPos = glm::vec3(previewWorld[3]);
			camRight = glm::vec3(previewWorld[0]);
			camUp = glm::vec3(previewWorld[1]);
			const float rl = glm::length(camRight);
			const float ul = glm::length(camUp);
			camRight = rl > 0.0f ? camRight / rl : glm::vec3(1.0f, 0.0f, 0.0f);
			camUp = ul > 0.0f ? camUp / ul : glm::vec3(0.0f, 1.0f, 0.0f);
		}
		else
		{
			Renderer3D::BeginScene(*editorCam);
			camPos = editorCam->GetPosition();
			camRight = editorCam->GetRightDirection();
			camUp = editorCam->GetUpDirection();
		}

		SubmitLightsAndSky();
		Renderer3D::DrawGrid();
		SubmitMeshes();
		SubmitParticles(camPos, camRight, camUp);

		ECS::SingletonAccessView<PhysicsSettings> settingsView{ m_Scene };
		if (settingsView.Get()->ShowColliderGizmos)
			DrawColliderGizmos();

		DrawMarkerGizmos();
		DrawSkeletonGizmos();

		if (const EditorBoundsOverlay* overlay = m_Scene.FindSingleton<EditorBoundsOverlay>())
		{
			for (const EditorBoundsOverlay::Box& box : overlay->Boxes)
				Renderer3D::DrawWireBox(box.Transform, box.Color);
			for (const EditorBoundsOverlay::Sphere& sphere : overlay->Spheres)
				Renderer3D::DrawWireSphere(sphere.Center, sphere.Radius, sphere.Color);
		}

		Renderer3D::EndScene();

		if (previewCam)
			Renderer2D::BeginScene(*previewCam, previewWorld);
		else
			Renderer2D::BeginScene(*editorCam);
		SubmitSprites();
		Renderer2D::EndScene();
	}

	void RenderSystem::RebuildEditorHidden()
	{
		m_EditorHidden.clear();
		const EditorViewFilter* filter = m_Scene.FindSingleton<EditorViewFilter>();
		if (!filter || !filter->HiddenEntities || filter->HiddenEntities->empty())
			return;

		std::vector<Entity> subtree;
		std::unordered_set<UUID> visited;
		for (UUID id : *filter->HiddenEntities)
		{
			Entity entity = m_Scene.FindEntityByUUID(id);
			if (!entity)
				continue;
			subtree.clear();
			visited.clear();
			m_Scene.CollectSubtree(entity, subtree, visited);
			for (Entity node : subtree)
				m_EditorHidden.insert(node.GetUUID());
		}
	}

	bool RenderSystem::IsEditorHidden(entt::entity entity) const
	{
		if (m_EditorHidden.empty())
			return false;
		if (!m_Scene.Reg().all_of<IDComponent>(entity))
			return false;
		return m_EditorHidden.count(m_Scene.Reg().get<IDComponent>(entity).ID) != 0;
	}

}

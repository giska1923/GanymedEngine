#pragma once

#include "GanymedE/ECS/System.h"
#include "GanymedE/ECS/Views.h"
#include "GanymedE/Scene/Components.h"

namespace GanymedE {

	// Everything that used to be inlined in Scene::OnUpdateRuntime / OnUpdateEditor: the primary
	// camera search, lights and sky, meshes, sprites, and collider gizmos.
	//
	// The view declarations below are the point: they are live documentation of exactly what
	// rendering reads, and in Phase 8 they become the scheduling metadata for free.
	class RenderSystem : public ECS::System<RenderSystem>
	{
	public:
		// The primary-camera search now lives in CameraSystem, which runs first and leaves the
		// answer in the RenderContext singleton.
		using MeshView       = ECS::IterView<ECS::EntityId, ECS::RO<WorldTransformComponent>, ECS::RO<StaticMeshComponent>>;
		using SpriteView     = ECS::IterView<ECS::EntityId, ECS::RO<WorldTransformComponent>, ECS::RO<SpriteRendererComponent>>;
		using DirLightView   = ECS::IterView<ECS::EntityId, ECS::RO<WorldTransformComponent>, ECS::RO<DirectionalLightComponent>>;
		using PointLightView = ECS::IterView<ECS::EntityId, ECS::RO<WorldTransformComponent>, ECS::RO<PointLightComponent>>;
		using SpotLightView  = ECS::IterView<ECS::EntityId, ECS::RO<WorldTransformComponent>, ECS::RO<SpotLightComponent>>;
		using SkyView        = ECS::IterView<ECS::RO<SkyLightComponent>>;
		using BoxColliderView     = ECS::IterView<ECS::EntityId, ECS::RO<WorldTransformComponent>, ECS::RO<BoxColliderComponent>>;
		using SphereColliderView  = ECS::IterView<ECS::EntityId, ECS::RO<WorldTransformComponent>, ECS::RO<SphereColliderComponent>>;
		using CapsuleColliderView = ECS::IterView<ECS::EntityId, ECS::RO<WorldTransformComponent>, ECS::RO<CapsuleColliderComponent>>;

		using Views = TypeList<
			MeshView,
			SpriteView,
			DirLightView,
			PointLightView,
			SpotLightView,
			SkyView,
			BoxColliderView,
			SphereColliderView,
			CapsuleColliderView
		>;

		using ECS::System<RenderSystem>::System;

		void OnUpdate(Timestep ts) override;
		void OnUpdateEditor(Timestep ts) override;
		const char* Name() const override { return "RenderSystem"; }

	private:
		void SubmitLightsAndSky();
		void SubmitMeshes();
		void SubmitSprites();
		void DrawColliderGizmos();

		// Jolt's own debug view when physics is running and enabled, otherwise authored gizmos.
		void DrawPhysicsDebugOrGizmos(const glm::vec3& cameraPosition);
	};
}

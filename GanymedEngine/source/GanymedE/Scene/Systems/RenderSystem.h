#pragma once

#include "GanymedE/Core/UUID.h"
#include "GanymedE/ECS/System.h"
#include "GanymedE/ECS/Views.h"
#include "GanymedE/Scene/Components.h"

#include <unordered_set>
#include <vector>

namespace GanymedE {

	class Entity;
	class Material;
	class Scene;

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
		// The animator is optional, not a second view: one iteration covers both draw
		// paths, and declaring the read here is what finally makes the ordering against
		// AnimationSystem enforceable. Until this, the two shared no component, so
		// ValidateOrdering had nothing to check and the correct order was only a
		// convention in Scene's registration list.
		using MeshView       = ECS::IterView<ECS::EntityId, ECS::RO<WorldTransformComponent>, ECS::RO<StaticMeshComponent>, ECS::OptRO<AnimatorComponent>>;
		using SpriteView     = ECS::IterView<ECS::EntityId, ECS::RO<WorldTransformComponent>, ECS::RO<SpriteRendererComponent>>;
		using DirLightView   = ECS::IterView<ECS::EntityId, ECS::RO<WorldTransformComponent>, ECS::RO<DirectionalLightComponent>>;
		using PointLightView = ECS::IterView<ECS::EntityId, ECS::RO<WorldTransformComponent>, ECS::RO<PointLightComponent>>;
		using SpotLightView  = ECS::IterView<ECS::EntityId, ECS::RO<WorldTransformComponent>, ECS::RO<SpotLightComponent>>;
		using SkyView        = ECS::IterView<ECS::EntityId, ECS::RO<SkyLightComponent>>;
		using BoxColliderView     = ECS::IterView<ECS::EntityId, ECS::RO<WorldTransformComponent>, ECS::RO<BoxColliderComponent>>;
		using SphereColliderView  = ECS::IterView<ECS::EntityId, ECS::RO<WorldTransformComponent>, ECS::RO<SphereColliderComponent>>;
		using CapsuleColliderView = ECS::IterView<ECS::EntityId, ECS::RO<WorldTransformComponent>, ECS::RO<CapsuleColliderComponent>>;
		// Iterated in Phase 3: billboards go to ParticleRenderer, mesh particles
		// ride SubmitMesh. The declaration itself was Phase 2 (ordering lock).
		using ParticleView = ECS::IterView<ECS::EntityId, ECS::RO<WorldTransformComponent>, ECS::RO<ParticleEmitterComponent>>;
		using MarkerView   = ECS::IterView<ECS::EntityId, ECS::RO<WorldTransformComponent>, ECS::RO<MarkerComponent>>;
		// The skeleton overlay draws two-hand IK's markers and its out-of-reach lines from what
		// AnimationSystem's pass recorded. Declared so ValidateOrdering holds the reader after it.
		using HandIKAccess = ECS::AccessView<ECS::RO<TwoHandIKComponent>>;

		using Views = TypeList<
			MeshView,
			SpriteView,
			DirLightView,
			PointLightView,
			SpotLightView,
			SkyView,
			BoxColliderView,
			SphereColliderView,
			CapsuleColliderView,
			ParticleView,
			MarkerView,
			HandIKAccess
		>;

		using ECS::System<RenderSystem>::System;

		void OnUpdate(Timestep ts) override;
		void OnUpdateEditor(Timestep ts) override;
		const char* Name() const override { return "RenderSystem"; }

		// Whether the skeleton overlay draws `skinned` for this selection: the rig or any
		// ancestor is selected (select the capsule, see the body's bones), or the rig is an
		// ancestor of something selected (select the rifle, see the hand). The overlay owns this
		// rule; editor joint picking calls it so a bone can only be clicked where one is drawn.
		static bool SkeletonInSelection(Scene& scene, Entity skinned,
			const std::unordered_set<UUID>& selected);

	private:
		void SubmitLightsAndSky();
		void SubmitMeshes();
		void SubmitParticles(const glm::vec3& cameraPosition, const glm::vec3& cameraRight,
			const glm::vec3& cameraUp);
		void SubmitSprites();
		void DrawColliderGizmos();
		void DrawMarkerGizmos();
		void DrawSkeletonGizmos();

		void RebuildEditorHidden();
		bool IsEditorHidden(entt::entity entity) const;

		// Jolt's own debug view when physics is running and enabled, otherwise authored
		// gizmos - and those only when PhysicsSettings::ShowColliderGizmos is set.
		// Edit used to call DrawColliderGizmos unconditionally; it now reads the same flag.
		void DrawPhysicsDebugOrGizmos(const glm::vec3& cameraPosition);

		// Reused across entities within one SubmitMeshes pass, so resolving material overrides
		// costs no allocation after the first frame that needs it.
		std::vector<Ref<Material>> m_ResolvedOverrides;

		// Pose scratch for DrawSkeletonGizmos. Sized to the current rig, reused.
		std::vector<glm::mat4> m_JointWorld;
		std::vector<uint8_t> m_JointOk;
		std::vector<uint8_t> m_JointHasChild;
		std::vector<float> m_BoneLength;

		// Filled only for OnUpdateEditor from EditorViewFilter; empty during play/runtime.
		std::unordered_set<UUID> m_EditorHidden;

		// Throttle for the no-camera error. Primed above the interval so the very first
		// cameraless frame reports immediately instead of after a five-second silence.
		static constexpr float kNoCameraLogInterval = 5.0f;
		float m_NoCameraLogTimer = kNoCameraLogInterval;
	};
}

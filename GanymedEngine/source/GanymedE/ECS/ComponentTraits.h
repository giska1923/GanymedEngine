#pragma once

#include "TypeList.h"
#include "GanymedE/Scene/Components.h"

namespace GanymedE {

	// ---- Opt-in per-component flags ----
	// Specialize to enable ECS features on a component type, e.g. (Phase 2+):
	//   template<> struct ComponentTraits<TransformComponent> { static constexpr bool TrackChanges = true; };
	template<typename T>
	struct ComponentTraits
	{
		static constexpr bool TrackChanges = false;   // enables ChangeView on T
		static constexpr bool EnableInit   = false;   // records component creation, for InitView on T
		static constexpr bool EnableFini   = false;   // keeps removed instances readable for one
		                                              // frame (graveyard), for FiniView on T
	};

	// Tracked from Phase 2 so the signal hookup and per-frame buffer rotation are actually
	// exercised. Nothing consumes the log until ChangeView lands (Phase 6) — note that
	// PhysicsScene::SyncTransforms still writes transforms straight through the registry, so the
	// log under-reports until that write is routed through Modify() in Phase 12.
	// The two inputs to the world-transform cache. TransformSystem's ChangeView reacts to either,
	// so only entities whose local transform or parenting actually moved get recomputed.
	template<> struct ComponentTraits<TransformComponent>
	{
		static constexpr bool TrackChanges = true;
		static constexpr bool EnableInit   = false;
		static constexpr bool EnableFini   = false;
	};

	template<> struct ComponentTraits<RelationshipComponent>
	{
		static constexpr bool TrackChanges = true;
		static constexpr bool EnableInit   = false;
		static constexpr bool EnableFini   = false;
	};

	// Script teardown is what FiniView expresses in Phase 6: when a NativeScriptComponent goes
	// away, its instance still needs OnDestroy called and deleting. The graveyard keeps the removed
	// instance readable for the one frame that takes.
	template<> struct ComponentTraits<NativeScriptComponent>
	{
		static constexpr bool TrackChanges = false;
		static constexpr bool EnableInit   = true;
		static constexpr bool EnableFini   = true;
	};

	// ---- Every user-facing component, in one place ----
	// This is the single source of truth: anything that must be applied to "all components"
	// (Scene::Copy, and later serialization / editor menus / change-buffer hookup) iterates this
	// list instead of hand-maintaining a parallel list of its own.
	//
	// IDComponent and TagComponent are intentionally NOT here: they are entity identity, created
	// by CreateEntityWithUUID and never copied generically.
	using ComponentList = TypeList<
		TransformComponent,
		WorldTransformComponent,
		RelationshipComponent,
		SpriteRendererComponent,
		StaticMeshComponent,
		CameraComponent,
		DirectionalLightComponent,
		PointLightComponent,
		SpotLightComponent,
		SkyLightComponent,
		NativeScriptComponent,
		RigidBodyComponent,
		BoxColliderComponent,
		SphereColliderComponent,
		CapsuleColliderComponent
	>;
}

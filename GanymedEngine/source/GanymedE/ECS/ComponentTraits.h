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

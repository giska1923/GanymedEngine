#pragma once

#include <glm/glm.hpp>

#include "GanymedE/Core/UUID.h"
#include "GanymedE/ECS/SingletonTraits.h"
#include "GanymedE/Physics/PhysicsScene.h"
#include "GanymedE/Renderer/EditorCamera.h"

#include <cstdint>
#include <unordered_set>
#include <vector>

// Scene-wide state that is genuinely singular. These used to be either members of Scene (which
// made Scene the dumping ground for anything a system needed) or "first one wins" scans over a
// component that only ever had one instance.

namespace GanymedE {

	class Camera;

	// Resolved once per update by CameraSystem, consumed by RenderSystem. Replaces both the
	// primary-camera scan that used to sit inline in the render path and the fallbackCamera
	// parameter that had to be threaded through Scene::OnUpdateRuntime.
	struct RenderContext
	{
		// The scene's primary camera for this update, or null if the scene has none.
		// Rewritten every update: never hold on to it across frames.
		const Camera* MainCamera = nullptr;
		glm::mat4 CameraTransform{ 1.0f };

		// The editor's own camera: the view camera in edit mode, and the fallback in play mode
		// when the scene has no primary camera. Null outside the editor.
		EditorCamera* EditorViewCamera = nullptr;

		// Editor viewport "look through this scene camera". UUID{0} means EditorViewCamera.
		// Not serialized; Scene::Copy default-constructs 0. RenderSystem::OnUpdateEditor
		// reads it; the runtime path ignores it.
		UUID PreviewCamera{ 0 };
	};

	struct PhysicsSettings
	{
		PhysicsDebugDrawSettings DebugDraw;

		// Authored collider wireframes during play, when Jolt debug draw is off.
		// Default false because a shipped game must not draw them over the scene; the
		// editor opts in. Nothing carries this across Scene::Copy (singletons are not
		// copied - the new Scene's ctor default-constructs its own), so the editor
		// re-asserts it on the play scene every frame alongside DebugDraw. Edit mode
		// used to call DrawColliderGizmos unconditionally; it now reads this flag too.
		bool ShowColliderGizmos = false;

		// Marker wireframes (sphere + optional forward). Same editor-opt-in shape as
		// ShowColliderGizmos: engine default off, the editor pushes it every frame because
		// Scene::Copy does not carry singletons. Lives here rather than a one-bool singleton
		// because this is already the bag those flags are pushed through; the name is debt.
		bool ShowMarkers = false;

		// Skeleton overlay. Same editor-opt-in shape. ShowAllSkeletons draws every posed
		// rig; otherwise only the current selection (and its hierarchy — select the capsule,
		// see the body's bones). SkeletonXRay skips the depth test so bones inside the mesh
		// are visible; off keeps occlusion as information.
		bool ShowSkeletons = false;
		bool ShowAllSkeletons = false;
		bool SkeletonXRay = true;

		float FixedTimestep = 1.0f / 60.0f;
		int MaxStepsPerFrame = 5;          // spiral-of-death guard
	};

	// Editor-only extra wire boxes (collider audit overlay). Not serialized, not copied.
	// RenderSystem::OnUpdateEditor draws them; play/runtime ignore the singleton.
	struct EditorBoundsOverlay
	{
		struct Box
		{
			glm::mat4 Transform{ 1.0f };
			glm::vec4 Color{ 1.0f };
		};
		std::vector<Box> Boxes;

		struct Sphere
		{
			glm::vec3 Center{ 0.0f };
			float Radius = 1.0f;
			glm::vec4 Color{ 1.0f };
		};
		std::vector<Sphere> Spheres;
	};

	// Editor outliner eye-toggle. Pointer into editor-owned state; never serialized,
	// not copied by Scene::Copy. Null means "draw everything" (runtime, play mode, or
	// an editor that has not asserted a set this frame). RenderSystem consults this
	// only from OnUpdateEditor, so Play still draws hidden entities.
	struct EditorViewFilter
	{
		const std::unordered_set<UUID>* HiddenEntities = nullptr;

		// Skeleton visualizer: draw only these entities' hierarchy unless ShowAllSkeletons.
		// Pointer into editor-owned state, same contract as HiddenEntities.
		const std::unordered_set<UUID>* SelectedEntities = nullptr;

		// Joint to accent + triad + labels. UUID{0} / -1 means none. S3 writes picking;
		// until then the editor fills this from a selected BoneAttachmentComponent.
		UUID HighlightSkeletonEntity{ 0 };
		int32_t HighlightJoint = -1;
	};

	// Change-tracked so a system can react to the camera moving rather than recomputing
	// camera-dependent work every frame.
	template<> struct SingletonTraits<RenderContext> { static constexpr bool TrackChanges = true; };
}

#pragma once

#include <glm/glm.hpp>

#include "GanymedE/ECS/SingletonTraits.h"
#include "GanymedE/Physics/PhysicsScene.h"
#include "GanymedE/Renderer/EditorCamera.h"

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
	};

	struct PhysicsSettings
	{
		PhysicsDebugDrawSettings DebugDraw;

		float FixedTimestep = 1.0f / 60.0f;
		int MaxStepsPerFrame = 5;          // spiral-of-death guard
	};

	// Change-tracked so a system can react to the camera moving rather than recomputing
	// camera-dependent work every frame.
	template<> struct SingletonTraits<RenderContext> { static constexpr bool TrackChanges = true; };
}

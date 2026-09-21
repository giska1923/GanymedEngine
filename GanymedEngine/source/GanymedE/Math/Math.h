#pragma once

#include <glm/glm.hpp>

namespace GanymedE::Math {

	// Decomposes an affine transform into translation, euler-angle rotation (radians) and scale.
	// Ignores perspective and skew, which our TransformComponent can never produce.
	bool DecomposeTransform(const glm::mat4& transform, glm::vec3& outTranslation, glm::vec3& outRotation, glm::vec3& outScale);

	// A parametric ray: Origin + t * Direction, t in [0, MaxDistance].
	// Direction is unit length when produced by ScreenPointToRay; MaxDistance is then metres.
	struct Ray
	{
		glm::vec3 Origin{ 0.0f };
		glm::vec3 Direction{ 0.0f, 0.0f, -1.0f };
		float MaxDistance = 1.0e30f;
	};

	// Clip-space xy in [-1, 1] through inverse(viewProjection) onto the near and far planes.
	//
	// nearClipZ / farClipZ are the clip-space z of those planes, not camera distances:
	// 0 and 1 under zero-to-one depth (D3D / Vulkan / Metal), -1 and 1 under OpenGL's
	// homogeneous depth. Matches Projection::HomogeneousDepth() — passed in rather than
	// queried, so this file does not depend on the renderer. Defaults are the workspace's
	// GLM_FORCE_DEPTH_ZERO_TO_ONE convention, which is also what HomogeneousDepth() reports
	// before the GPU is up.
	//
	// Origin is the unprojected near point, not the camera position, so an orthographic
	// projection (M5) produces parallel rays instead of a pinhole. MaxDistance is the
	// near-to-far length, which is the far clip expressed along this ray.
	Ray ScreenPointToRay(const glm::mat4& inverseViewProjection, const glm::vec2& ndc,
		float nearClipZ = 0.0f, float farClipZ = 1.0f);
}

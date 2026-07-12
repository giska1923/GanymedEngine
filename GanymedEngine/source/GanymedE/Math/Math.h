#pragma once

#include <glm/glm.hpp>

namespace GanymedE::Math {

	// Decomposes an affine transform into translation, euler-angle rotation (radians) and scale.
	// Ignores perspective and skew, which our TransformComponent can never produce.
	bool DecomposeTransform(const glm::mat4& transform, glm::vec3& outTranslation, glm::vec3& outRotation, glm::vec3& outScale);
}

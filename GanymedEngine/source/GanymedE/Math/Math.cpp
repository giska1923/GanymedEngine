#include "gepch.h"
#include "Math.h"

#include <glm/gtc/epsilon.hpp>
#include <glm/gtc/constants.hpp>

namespace GanymedE::Math {

	// Simplified from glm::decompose (matrix_decompose.inl)
	bool DecomposeTransform(const glm::mat4& transform, glm::vec3& outTranslation, glm::vec3& outRotation, glm::vec3& outScale)
	{
		using namespace glm;
		using T = float;

		mat4 localMatrix(transform);

		// Normalize the matrix
		if (epsilonEqual(localMatrix[3][3], static_cast<T>(0), epsilon<T>()))
			return false;

		// Isolate perspective (assumed absent, just clear it)
		if (
			epsilonNotEqual(localMatrix[0][3], static_cast<T>(0), epsilon<T>()) ||
			epsilonNotEqual(localMatrix[1][3], static_cast<T>(0), epsilon<T>()) ||
			epsilonNotEqual(localMatrix[2][3], static_cast<T>(0), epsilon<T>()))
		{
			localMatrix[0][3] = localMatrix[1][3] = localMatrix[2][3] = static_cast<T>(0);
			localMatrix[3][3] = static_cast<T>(1);
		}

		// Translation
		outTranslation = vec3(localMatrix[3]);
		localMatrix[3] = vec4(0, 0, 0, localMatrix[3].w);

		vec3 row[3];

		// Scale
		for (length_t i = 0; i < 3; ++i)
			for (length_t j = 0; j < 3; ++j)
				row[i][j] = localMatrix[i][j];

		outScale.x = length(row[0]);
		row[0] = row[0] / outScale.x;
		outScale.y = length(row[1]);
		row[1] = row[1] / outScale.y;
		outScale.z = length(row[2]);
		row[2] = row[2] / outScale.z;

		// Rotation as euler angles
		outRotation.y = asin(-row[0][2]);
		if (cos(outRotation.y) != 0)
		{
			outRotation.x = atan2(row[1][2], row[2][2]);
			outRotation.z = atan2(row[0][1], row[0][0]);
		}
		else
		{
			outRotation.x = atan2(-row[2][0], row[1][1]);
			outRotation.z = 0;
		}

		return true;
	}
}

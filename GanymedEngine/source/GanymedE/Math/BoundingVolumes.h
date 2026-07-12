#pragma once

#include <glm/glm.hpp>

namespace GanymedE {

	struct AABB
	{
		glm::vec3 Min{ 0.0f };
		glm::vec3 Max{ 0.0f };

		AABB() = default;
		AABB(const glm::vec3& min, const glm::vec3& max)
			: Min(min), Max(max) {}

		void Grow(const glm::vec3& point)
		{
			Min = glm::min(Min, point);
			Max = glm::max(Max, point);
		}

		// Transformed axis-aligned bounds of the 8 corners
		AABB Transformed(const glm::mat4& transform) const
		{
			glm::vec3 corners[8] = {
				{ Min.x, Min.y, Min.z }, { Max.x, Min.y, Min.z },
				{ Min.x, Max.y, Min.z }, { Max.x, Max.y, Min.z },
				{ Min.x, Min.y, Max.z }, { Max.x, Min.y, Max.z },
				{ Min.x, Max.y, Max.z }, { Max.x, Max.y, Max.z }
			};

			glm::vec3 first = glm::vec3(transform * glm::vec4(corners[0], 1.0f));
			AABB result(first, first);
			for (int i = 1; i < 8; i++)
				result.Grow(glm::vec3(transform * glm::vec4(corners[i], 1.0f)));
			return result;
		}
	};

	// Six view-frustum planes (normals point inward), extracted from a
	// view-projection matrix (Gribb/Hartmann). Plane: dot(Normal, p) + D >= 0 is inside.
	struct Frustum
	{
		glm::vec4 Planes[6]{}; // xyz = normal, w = distance

		static Frustum FromViewProjection(const glm::mat4& viewProjection)
		{
			Frustum frustum;
			const glm::mat4& m = viewProjection;

			// Rows of the matrix (glm is column-major: row(i) = (m[0][i], m[1][i], m[2][i], m[3][i]))
			auto row = [&m](int i) {
				return glm::vec4(m[0][i], m[1][i], m[2][i], m[3][i]);
			};

			frustum.Planes[0] = row(3) + row(0); // left
			frustum.Planes[1] = row(3) - row(0); // right
			frustum.Planes[2] = row(3) + row(1); // bottom
			frustum.Planes[3] = row(3) - row(1); // top
			frustum.Planes[4] = row(3) + row(2); // near
			frustum.Planes[5] = row(3) - row(2); // far

			for (auto& plane : frustum.Planes)
			{
				float length = glm::length(glm::vec3(plane));
				if (length > 0.0f)
					plane /= length;
			}
			return frustum;
		}

		// Conservative AABB test: false only when the box is fully outside a plane
		bool Intersects(const AABB& aabb) const
		{
			for (const auto& plane : Planes)
			{
				glm::vec3 normal = glm::vec3(plane);
				// Corner of the box furthest along the plane normal
				glm::vec3 positive{
					normal.x >= 0.0f ? aabb.Max.x : aabb.Min.x,
					normal.y >= 0.0f ? aabb.Max.y : aabb.Min.y,
					normal.z >= 0.0f ? aabb.Max.z : aabb.Min.z
				};
				if (glm::dot(normal, positive) + plane.w < 0.0f)
					return false;
			}
			return true;
		}
	};

}

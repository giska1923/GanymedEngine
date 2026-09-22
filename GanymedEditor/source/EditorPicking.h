#pragma once

#include "GanymedE/Core/UUID.h"
#include "GanymedE/Math/Math.h"
#include "GanymedE/Scene/Entity.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <unordered_set>

namespace GanymedE {

	struct SurfaceHit
	{
		Entity Hit;                 // invalid on a miss and on a work-plane fallback
		glm::vec3 Point{ 0.0f };    // world
		glm::vec3 Normal{ 0.0f, 1.0f, 0.0f }; // world, always facing the ray
		float Distance = 0.0f;
		uint32_t Submesh = 0;
		bool FromWorkPlane = false;

		bool IsHit() const { return Hit || FromWorkPlane; }
	};

	struct RaycastFilter
	{
		const std::unordered_set<UUID>* HiddenEntities = nullptr;
		UUID Exclude{ 0 };          // placement preview; UUID() is random, so this is UUID{0}
		const std::unordered_set<UUID>* ExcludeSet = nullptr; // scatter group / in-stroke instances
		float GridHeight = 0.0f;
		uint32_t TriangleBudget = 250000;
	};

	// CPU ray against resident static-mesh triangles. Reads WorldTransformComponent, so the
	// caller must have run TransformSystem this frame (or accept last frame's cache).
	SurfaceHit RaycastScene(const Ref<Scene>& scene, const Math::Ray& ray, const RaycastFilter& filter = {});

}

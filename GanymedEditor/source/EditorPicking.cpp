#include "EditorPicking.h"

#include "GanymedE/Core/Log.h"
#include "GanymedE/Debug/Instrumentor.h"
#include "GanymedE/Math/BoundingVolumes.h"
#include "GanymedE/Renderer/Mesh.h"
#include "GanymedE/Scene/Components.h"
#include "GanymedE/Scene/Scene.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace GanymedE {

	namespace {

		constexpr float kRayEpsilon = 1.0e-8f;

		// Kay–Kajiya slab. tMin is clamped to 0 (a ray, not a line). outEntryAxis is -1 when
		// the origin is already inside the box.
		bool IntersectRayAABB(const glm::vec3& origin, const glm::vec3& dir, float maxT,
			const AABB& box, float& outTMin, int& outEntryAxis)
		{
			float tMin = 0.0f;
			float tMax = maxT;
			int entryAxis = -1;

			for (int i = 0; i < 3; ++i)
			{
				const float o = origin[i];
				const float d = dir[i];
				const float minB = box.Min[i];
				const float maxB = box.Max[i];

				if (std::abs(d) < kRayEpsilon)
				{
					if (o < minB || o > maxB)
						return false;
					continue;
				}

				const float invD = 1.0f / d;
				float t0 = (minB - o) * invD;
				float t1 = (maxB - o) * invD;
				if (t0 > t1)
					std::swap(t0, t1);

				if (t0 > tMin)
				{
					tMin = t0;
					entryAxis = i;
				}
				if (t1 < tMax)
					tMax = t1;
				if (tMin > tMax)
					return false;
			}

			outTMin = tMin;
			outEntryAxis = entryAxis;
			return true;
		}

		// Möller–Trumbore, double-sided. Direction need not be unit: t is in the same
		// parameterisation, so a ray transformed by inverse(world) without renormalising
		// still reports world-space t.
		bool IntersectRayTriangle(const glm::vec3& origin, const glm::vec3& dir, float maxT,
			const glm::vec3& v0, const glm::vec3& v1, const glm::vec3& v2,
			float& outT, glm::vec3& outLocalNormal)
		{
			const glm::vec3 e1 = v1 - v0;
			const glm::vec3 e2 = v2 - v0;
			const glm::vec3 pvec = glm::cross(dir, e2);
			const float det = glm::dot(e1, pvec);
			if (std::abs(det) < kRayEpsilon)
				return false;

			const float invDet = 1.0f / det;
			const glm::vec3 tvec = origin - v0;
			const float u = glm::dot(tvec, pvec) * invDet;
			if (u < 0.0f || u > 1.0f)
				return false;

			const glm::vec3 qvec = glm::cross(tvec, e1);
			const float v = glm::dot(dir, qvec) * invDet;
			if (v < 0.0f || u + v > 1.0f)
				return false;

			const float t = glm::dot(e2, qvec) * invDet;
			if (t < 0.0f || t > maxT)
				return false;

			outT = t;
			outLocalNormal = glm::cross(e1, e2);
			return true;
		}

		glm::vec3 FaceRay(const glm::vec3& normal, const glm::vec3& rayDir)
		{
			glm::vec3 n = normal;
			const float len2 = glm::dot(n, n);
			if (len2 > 0.0f)
				n /= std::sqrt(len2);
			else
				n = glm::vec3(0.0f, 1.0f, 0.0f);

			// Flip by ray direction, not by winding: a negative-scaled entity inverts winding
			// and would otherwise report an inward normal.
			if (glm::dot(n, rayDir) > 0.0f)
				n = -n;
			return n;
		}

		glm::vec3 AABBEntryNormal(int entryAxis, const glm::vec3& rayDir)
		{
			if (entryAxis < 0 || entryAxis > 2)
				return FaceRay(glm::vec3(0.0f, 1.0f, 0.0f), rayDir);

			glm::vec3 n(0.0f);
			n[entryAxis] = rayDir[entryAxis] > 0.0f ? -1.0f : 1.0f;
			return n;
		}

		void WarnTriangleBudgetOnce(const Mesh& mesh, uint32_t triangles, uint32_t budget)
		{
			static std::unordered_set<std::string> s_Warned;
			std::string key = mesh.GetPath();
			if (key.empty())
				key = std::to_string(reinterpret_cast<uintptr_t>(&mesh));
			if (!s_Warned.insert(key).second)
				return;

			GE_CORE_WARN("Mesh '{0}' exceeds triangle budget ({1} > {2}) — surface raycast using AABB",
				key, triangles, budget);
		}

		uint32_t MeshTriangleCount(const Mesh& mesh)
		{
			uint32_t triangles = 0;
			for (const Submesh& submesh : mesh.GetSubmeshes())
				triangles += submesh.IndexCount / 3;
			return triangles;
		}

		struct Candidate
		{
			entt::entity Handle{ entt::null };
			float TMin = 0.0f;
			int EntryAxis = -1;
			glm::mat4 World{ 1.0f };
			Ref<Mesh> Mesh;
		};

		struct NarrowHit
		{
			bool Hit = false;
			float T = 0.0f;
			uint32_t Submesh = 0;
			glm::vec3 WorldNormal{ 0.0f, 1.0f, 0.0f };
		};

		NarrowHit TraceMesh(const Math::Ray& worldRay, const Candidate& candidate, uint32_t triangleBudget, float maxT)
		{
			const Mesh& mesh = *candidate.Mesh;
			const uint32_t triangles = MeshTriangleCount(mesh);
			if (triangles > triangleBudget)
			{
				WarnTriangleBudgetOnce(mesh, triangles, triangleBudget);
				NarrowHit hit;
				hit.Hit = true;
				hit.T = candidate.TMin;
				hit.WorldNormal = AABBEntryNormal(candidate.EntryAxis, worldRay.Direction);
				return hit;
			}

			const auto& vertices = mesh.GetVertices();
			const auto& indices = mesh.GetIndices();
			const auto& submeshes = mesh.GetSubmeshes();

			NarrowHit best;
			best.T = maxT;

			for (uint32_t s = 0; s < (uint32_t)submeshes.size(); ++s)
			{
				const Submesh& submesh = submeshes[s];
				if (submesh.IndexCount < 3)
					continue;

				const glm::mat4 meshLocal = candidate.World * submesh.LocalTransform;
				if (std::abs(glm::determinant(glm::mat3(meshLocal))) < kRayEpsilon)
					continue;

				const glm::mat4 invMeshLocal = glm::inverse(meshLocal);
				const glm::vec3 localOrigin = glm::vec3(invMeshLocal * glm::vec4(worldRay.Origin, 1.0f));
				const glm::vec3 localDir = glm::mat3(invMeshLocal) * worldRay.Direction;

				float subTMin = 0.0f;
				int unusedAxis = -1;
				if (!IntersectRayAABB(localOrigin, localDir, best.T, submesh.Bounds, subTMin, unusedAxis))
					continue;

				const glm::mat3 normalMatrix = glm::transpose(glm::inverse(glm::mat3(meshLocal)));
				const uint32_t indexEnd = submesh.BaseIndex + submesh.IndexCount;
				if (indexEnd > (uint32_t)indices.size())
					continue;

				for (uint32_t i = submesh.BaseIndex; i + 2 < indexEnd; i += 3)
				{
					const uint32_t i0 = submesh.BaseVertex + indices[i];
					const uint32_t i1 = submesh.BaseVertex + indices[i + 1];
					const uint32_t i2 = submesh.BaseVertex + indices[i + 2];
					if (i0 >= vertices.size() || i1 >= vertices.size() || i2 >= vertices.size())
						continue;

					float t = 0.0f;
					glm::vec3 localNormal(0.0f);
					if (!IntersectRayTriangle(localOrigin, localDir, best.T,
						vertices[i0].Position, vertices[i1].Position, vertices[i2].Position,
						t, localNormal))
						continue;

					best.Hit = true;
					best.T = t;
					best.Submesh = s;
					best.WorldNormal = normalMatrix * localNormal;
				}
			}

			return best;
		}

		SurfaceHit IntersectWorkPlane(const Math::Ray& ray, float gridHeight)
		{
			if (std::abs(ray.Direction.y) < kRayEpsilon)
				return {};

			const float t = (gridHeight - ray.Origin.y) / ray.Direction.y;
			if (t < 0.0f || t > ray.MaxDistance)
				return {};

			SurfaceHit hit;
			hit.Point = ray.Origin + t * ray.Direction;
			hit.Point.y = gridHeight;
			hit.Normal = FaceRay(glm::vec3(0.0f, 1.0f, 0.0f), ray.Direction);
			hit.Distance = t;
			hit.FromWorkPlane = true;
			return hit;
		}

	}

	SurfaceHit RaycastScene(const Ref<Scene>& scene, const Math::Ray& ray, const RaycastFilter& filter)
	{
		GE_PROFILE_FUNCTION();

		if (!scene)
			return {};

		if (glm::dot(ray.Direction, ray.Direction) < kRayEpsilon)
			return {};

		std::vector<Candidate> candidates;
		auto view = scene->Reg().view<WorldTransformComponent, StaticMeshComponent>(
			entt::exclude<AnimatorComponent>);
		candidates.reserve(view.size_hint());

		for (auto entityHandle : view)
		{
			Entity entity{ entityHandle, scene.get() };
			const UUID id = entity.GetUUID();
			if (id == filter.Exclude)
				continue;
			if (filter.HiddenEntities && filter.HiddenEntities->count(id) != 0)
				continue;

			auto& meshComponent = view.get<StaticMeshComponent>(entityHandle);
			const Ref<Mesh>& mesh = meshComponent.Mesh.Get();
			if (!mesh)
				continue;

			const glm::mat4& world = view.get<WorldTransformComponent>(entityHandle).World;
			const AABB worldBounds = mesh->GetBounds().Transformed(world);

			float tMin = 0.0f;
			int entryAxis = -1;
			if (!IntersectRayAABB(ray.Origin, ray.Direction, ray.MaxDistance, worldBounds, tMin, entryAxis))
				continue;

			candidates.push_back({ entityHandle, tMin, entryAxis, world, mesh });
		}

		std::sort(candidates.begin(), candidates.end(),
			[](const Candidate& a, const Candidate& b) { return a.TMin < b.TMin; });

		float bestT = ray.MaxDistance;
		entt::entity bestHandle = entt::null;
		uint32_t bestSubmesh = 0;
		glm::vec3 bestNormal(0.0f, 1.0f, 0.0f);

		for (const Candidate& candidate : candidates)
		{
			if (candidate.TMin > bestT)
				break;

			NarrowHit narrow = TraceMesh(ray, candidate, filter.TriangleBudget, bestT);
			if (!narrow.Hit || narrow.T >= bestT)
				continue;

			bestT = narrow.T;
			bestHandle = candidate.Handle;
			bestSubmesh = narrow.Submesh;
			bestNormal = narrow.WorldNormal;
		}

		if (bestHandle == entt::null)
			return IntersectWorkPlane(ray, filter.GridHeight);

		SurfaceHit hit;
		hit.Hit = Entity{ bestHandle, scene.get() };
		hit.Point = ray.Origin + bestT * ray.Direction;
		hit.Normal = FaceRay(bestNormal, ray.Direction);
		hit.Distance = bestT;
		hit.Submesh = bestSubmesh;
		return hit;
	}

}

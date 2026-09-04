#pragma once

#include "GanymedE/Renderer/Camera.h"
#include "GanymedE/Renderer/EditorCamera.h"
#include "GanymedE/Renderer/OrthographicCamera.h"
#include "GanymedE/Renderer/Mesh.h"
#include "GanymedE/Renderer/Material.h"
#include "GanymedE/Renderer/Environment.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace GanymedE {

	class Renderer3D
	{
	public:
		static void Init();
		static void Shutdown();

		static void BeginScene(const Camera& camera, const glm::mat4& transform);
		static void BeginScene(const EditorCamera& camera);
		static void EndScene();

		static void SubmitMesh(const Ref<Mesh>& mesh, uint32_t submeshIndex, const Ref<Material>& material,
			const glm::mat4& transform, int entityID = -1);

		// `materialOverrides` is indexed by Submesh::MaterialIndex, not by submesh: a null entry
		// or an index past `overrideCount` falls back to the mesh's own material. Passing the
		// array down rather than looping submeshes at the call site is what lets the skinned
		// path honour overrides too - its palette staging is internal, so a caller cannot
		// reproduce it.
		static void SubmitMesh(const Ref<Mesh>& mesh, const glm::mat4& transform, int entityID = -1,
			const Ref<Material>* materialOverrides = nullptr, uint32_t overrideCount = 0);

		// Draws the mesh with a joint palette (AnimatorComponent::Palette) instead of
		// the static path. The palette is copied here, so the caller may reuse or
		// rebuild its storage immediately. Skinned submeshes cannot batch - each is
		// one draw call with its own palette upload - so this falls back to
		// SubmitMesh when there is nothing to skin with.
		static void SubmitSkinnedMesh(const Ref<Mesh>& mesh, const glm::mat4& transform,
			const glm::mat4* palette, uint32_t jointCount, int entityID = -1,
			const Ref<Material>* materialOverrides = nullptr, uint32_t overrideCount = 0);

		// Analytic lights (submit between BeginScene and EndScene)
		static void SubmitDirectionalLight(const glm::vec3& direction, const glm::vec3& color, float intensity, bool castShadows);
		static void SubmitPointLight(const glm::vec3& position, const glm::vec3& color, float intensity, float radius, float falloff);
		static void SubmitSpotLight(const glm::vec3& position, const glm::vec3& direction, const glm::vec3& color,
			float intensity, float range, float innerConeCos, float outerConeCos, float falloff);

		// Environment / ambient. Procedural fallback when no HDR path is provided.
		static void SubmitSkyLight(const glm::vec3& skyColor, const glm::vec3& groundColor, float intensity, bool drawSkybox);
		static void SubmitEnvironment(const Ref<Environment>& environment, float intensity, bool drawSkybox);

		static void DrawSkybox();
		static void DrawGrid();

		// Immediate-ish debug drawing (accumulated and flushed in EndScene)
		static void DrawLine(const glm::vec3& p0, const glm::vec3& p1, const glm::vec4& color = glm::vec4(1.0f));
		static void DrawWireBox(const glm::mat4& transform, const glm::vec4& color = glm::vec4(0.2f, 0.9f, 0.35f, 1.0f));
		static void DrawWireSphere(const glm::vec3& center, float radius, const glm::vec4& color = glm::vec4(0.3f, 0.7f, 1.0f, 1.0f), int segments = 24);
		static void DrawWireCapsule(const glm::vec3& center, const glm::quat& rotation, float radius, float halfHeight,
			const glm::vec4& color = glm::vec4(1.0f, 0.75f, 0.2f, 1.0f), int segments = 16);

		struct Statistics
		{
			uint32_t DrawCalls = 0;
			uint32_t MeshCount = 0;        // submitted submesh draw commands
			uint32_t CulledMeshes = 0;     // rejected by camera frustum culling
			uint32_t InstancedDraws = 0;   // draw calls that batched > 1 instance
			uint32_t TransparentMeshes = 0;
			uint32_t SkinnedDraws = 0;     // draw calls that uploaded a joint palette
			uint32_t ParticleEmitters = 0;       // billboard emitters that produced a draw
			uint32_t ParticleBillboards = 0;     // quads that actually drew (truncation-honest)
			uint32_t ParticleDrawCalls = 0;      // one per visible billboard emitter
			uint32_t ParticleCulledEmitters = 0; // skipped by the Phase 2 AABB
		};
		static void ResetStats();
		static Statistics GetStats();

		static bool FrustumIntersects(const AABB& bounds);
		static void AddParticleStats(uint32_t emitters, uint32_t billboards, uint32_t drawCalls);
		static void AddCulledParticleEmitter();
	};

}

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

		static void SubmitMesh(const Ref<Mesh>& mesh, const glm::mat4& transform, int entityID = -1);
		static void SubmitMesh(const Ref<Mesh>& mesh, uint32_t submeshIndex, const Ref<Material>& material,
			const glm::mat4& transform, int entityID = -1);

		// Analytic lights (submit between BeginScene and EndScene)
		static void SubmitDirectionalLight(const glm::vec3& direction, const glm::vec3& color, float intensity, bool castShadows);
		static void SubmitPointLight(const glm::vec3& position, const glm::vec3& color, float intensity, float radius, float falloff);
		static void SubmitSpotLight(const glm::vec3& position, const glm::vec3& direction, const glm::vec3& color,
			float intensity, float range, float innerConeCos, float outerConeCos, float falloff);

		// Environment / ambient. Procedural fallback when no HDR path is provided.
		static void SubmitSkyLight(const glm::vec3& skyColor, const glm::vec3& groundColor, float intensity, bool drawSkybox);
		static void SubmitEnvironment(const Ref<Environment>& environment, float intensity, bool drawSkybox);

		// Loads and caches an HDR environment (relative to the assets/ folder).
		static Ref<Environment> LoadEnvironment(const std::string& path);

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
			uint32_t MeshCount = 0;
		};
		static void ResetStats();
		static Statistics GetStats();
	};

}

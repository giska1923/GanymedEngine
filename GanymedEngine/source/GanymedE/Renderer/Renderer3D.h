#pragma once

#include "GanymedE/Renderer/Camera.h"
#include "GanymedE/Renderer/EditorCamera.h"
#include "GanymedE/Renderer/OrthographicCamera.h"
#include "GanymedE/Renderer/Mesh.h"
#include "GanymedE/Renderer/Material.h"

#include <glm/glm.hpp>

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

		static void DrawGrid();

		struct Statistics
		{
			uint32_t DrawCalls = 0;
			uint32_t MeshCount = 0;
		};
		static void ResetStats();
		static Statistics GetStats();
	};

}

#pragma once

#include "GanymedE/Core/Core.h"
#include "GanymedE/Renderer/Material.h"
#include "GanymedE/Renderer/VertexArray.h"
#include "GanymedE/Renderer/Buffer.h"

#include <glm/glm.hpp>

#include <string>
#include <vector>

namespace GanymedE {

	struct MeshVertex
	{
		glm::vec3 Position;
		glm::vec3 Normal;
		glm::vec3 Tangent;
		glm::vec2 TexCoord;
	};

	struct Submesh
	{
		uint32_t BaseVertex = 0;
		uint32_t BaseIndex = 0;
		uint32_t IndexCount = 0;
		uint32_t MaterialIndex = 0;
		glm::mat4 LocalTransform{ 1.0f };
		std::string Name;
	};

	class Mesh
	{
	public:
		Mesh() = default;
		Mesh(const std::vector<MeshVertex>& vertices, const std::vector<uint32_t>& indices,
			const std::vector<Submesh>& submeshes, const std::vector<Ref<Material>>& materials);

		const std::vector<MeshVertex>& GetVertices() const { return m_Vertices; }
		const std::vector<uint32_t>& GetIndices() const { return m_Indices; }
		const std::vector<Submesh>& GetSubmeshes() const { return m_Submeshes; }
		const std::vector<Ref<Material>>& GetMaterials() const { return m_Materials; }
		Ref<Material> GetMaterial(uint32_t index) const;

		const Ref<VertexArray>& GetVertexArray() const { return m_VertexArray; }
		const std::string& GetPath() const { return m_Path; }
		void SetPath(const std::string& path) { m_Path = path; }

		static Ref<Mesh> Create(const std::vector<MeshVertex>& vertices, const std::vector<uint32_t>& indices,
			const std::vector<Submesh>& submeshes, const std::vector<Ref<Material>>& materials);
	private:
		void Build();
	private:
		std::vector<MeshVertex> m_Vertices;
		std::vector<uint32_t> m_Indices;
		std::vector<Submesh> m_Submeshes;
		std::vector<Ref<Material>> m_Materials;

		Ref<VertexArray> m_VertexArray;
		Ref<VertexBuffer> m_VertexBuffer;
		Ref<IndexBuffer> m_IndexBuffer;

		std::string m_Path;
	};

}

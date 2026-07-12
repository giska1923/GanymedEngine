#include "gepch.h"
#include "Mesh.h"

namespace GanymedE {

	Mesh::Mesh(const std::vector<MeshVertex>& vertices, const std::vector<uint32_t>& indices,
		const std::vector<Submesh>& submeshes, const std::vector<Ref<Material>>& materials)
		: m_Vertices(vertices), m_Indices(indices), m_Submeshes(submeshes), m_Materials(materials)
	{
		Build();
	}

	Ref<Mesh> Mesh::Create(const std::vector<MeshVertex>& vertices, const std::vector<uint32_t>& indices,
		const std::vector<Submesh>& submeshes, const std::vector<Ref<Material>>& materials)
	{
		return CreateRef<Mesh>(vertices, indices, submeshes, materials);
	}

	Ref<Material> Mesh::GetMaterial(uint32_t index) const
	{
		if (index < m_Materials.size())
			return m_Materials[index];
		return nullptr;
	}

	void Mesh::Build()
	{
		m_VertexArray = VertexArray::Create();

		m_VertexBuffer = VertexBuffer::Create((float*)m_Vertices.data(), (uint32_t)(m_Vertices.size() * sizeof(MeshVertex)));
		m_VertexBuffer->SetLayout({
			{ ShaderDataType::Float3, "a_Position" },
			{ ShaderDataType::Float3, "a_Normal" },
			{ ShaderDataType::Float3, "a_Tangent" },
			{ ShaderDataType::Float2, "a_TexCoord" }
		});
		m_VertexArray->AddVertexBuffer(m_VertexBuffer);

		m_IndexBuffer = IndexBuffer::Create(m_Indices.data(), (uint32_t)m_Indices.size());
		m_VertexArray->SetIndexBuffer(m_IndexBuffer);
	}

}

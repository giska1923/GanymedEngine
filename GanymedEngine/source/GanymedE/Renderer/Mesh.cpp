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

		// Per-instance transform + entity ID (attribute locations 4..8, divisor 1)
		m_InstanceBuffer = VertexBuffer::Create(MaxInstancesPerDraw * sizeof(MeshInstanceData));
		m_InstanceBuffer->SetLayout({
			{ ShaderDataType::Mat4, "a_InstanceTransform", false, true },
			{ ShaderDataType::Int,  "a_InstanceEntityID",  false, true }
		});
		m_VertexArray->AddVertexBuffer(m_InstanceBuffer);

		m_IndexBuffer = IndexBuffer::Create(m_Indices.data(), (uint32_t)m_Indices.size());
		m_VertexArray->SetIndexBuffer(m_IndexBuffer);

		ComputeBounds();
	}

	void Mesh::ComputeBounds()
	{
		bool meshFirst = true;
		for (Submesh& submesh : m_Submeshes)
		{
			bool first = true;
			for (uint32_t i = 0; i < submesh.IndexCount; i++)
			{
				uint32_t vertexIndex = submesh.BaseVertex + m_Indices[submesh.BaseIndex + i];
				if (vertexIndex >= m_Vertices.size())
					continue;

				const glm::vec3& p = m_Vertices[vertexIndex].Position;
				if (first)
				{
					submesh.Bounds = AABB(p, p);
					first = false;
				}
				else
				{
					submesh.Bounds.Grow(p);
				}
			}

			// Whole-mesh bounds include the submesh's local transform
			AABB worldish = submesh.Bounds.Transformed(submesh.LocalTransform);
			if (meshFirst)
			{
				m_Bounds = worldish;
				meshFirst = false;
			}
			else
			{
				m_Bounds.Grow(worldish.Min);
				m_Bounds.Grow(worldish.Max);
			}
		}
	}

	void Mesh::SetInstanceData(const MeshInstanceData* data, uint32_t count)
	{
		if (!m_InstanceBuffer || !data || count == 0)
			return;

		if (count > MaxInstancesPerDraw)
			count = MaxInstancesPerDraw;
		m_InstanceBuffer->SetData(data, count * sizeof(MeshInstanceData));
	}

}

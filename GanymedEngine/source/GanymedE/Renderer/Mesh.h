#pragma once

#include "GanymedE/Core/Core.h"
#include "GanymedE/Math/BoundingVolumes.h"
#include "GanymedE/Renderer/Material.h"
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
		AABB Bounds; // local-space bounds (before LocalTransform), rebuilt on load
	};

	// Per-instance data for instanced mesh draws. bgfx reads this as i_data0..4:
	// four vec4s of transform plus one for the entity ID.
	//
	// EntityID occupies a whole vec4 rather than a bare int for two reasons:
	// bgfx requires the instance stride to be a multiple of 16 bytes (mat4 + int
	// would be 68), and instance data is delivered to the shader as vec4s, so it
	// has to be float anyway.
	struct MeshInstanceData
	{
		glm::mat4 Transform{ 1.0f };
		glm::vec4 EntityID{ -1.0f };
	};

	static_assert(sizeof(MeshInstanceData) % 16 == 0,
		"bgfx instance data stride must be a multiple of 16 bytes");

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

		const Geometry& GetGeometry() const { return m_Geometry; }
		const std::string& GetPath() const { return m_Path; }
		void SetPath(const std::string& path) { m_Path = path; }

		const AABB& GetBounds() const { return m_Bounds; }

		// Stage per-instance transforms/IDs for the next instanced draw of this
		// mesh. Unlike the old GL path this does not touch the GPU: bgfx wants
		// instance data allocated from its transient pool at submit time, so the
		// data is held CPU-side until the draw call copies it.
		void SetInstanceData(const MeshInstanceData* data, uint32_t count);
		const std::vector<MeshInstanceData>& GetInstanceData() const { return m_InstanceData; }
		static constexpr uint32_t MaxInstancesPerDraw = 1024;

		static Ref<Mesh> Create(const std::vector<MeshVertex>& vertices, const std::vector<uint32_t>& indices,
			const std::vector<Submesh>& submeshes, const std::vector<Ref<Material>>& materials);
	private:
		void Build();
		void ComputeBounds();
	private:
		std::vector<MeshVertex> m_Vertices;
		std::vector<uint32_t> m_Indices;
		std::vector<Submesh> m_Submeshes;
		std::vector<Ref<Material>> m_Materials;
		AABB m_Bounds;

		Geometry m_Geometry;
		std::vector<MeshInstanceData> m_InstanceData;

		std::string m_Path;
	};

}

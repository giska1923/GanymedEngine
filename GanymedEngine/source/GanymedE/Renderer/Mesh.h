#pragma once

#include "GanymedE/Core/Core.h"
#include "GanymedE/Math/BoundingVolumes.h"
#include "GanymedE/Renderer/Animation.h"
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

		// Skinned primitives keep their vertices in skin space - glTF places them via
		// jointMatrix = globalJointTransform * inverseBindMatrix, and the spec says
		// the skinned mesh node's own transform is ignored. Static primitives are
		// baked to world space at import as before. This flag is the single gate:
		// the color pass, the shadow pass and bounds must all honor it.
		bool IsSkinned = false;
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
		Mesh(const std::vector<MeshVertex>& vertices, const std::vector<uint32_t>& indices,
			const std::vector<Submesh>& submeshes, const std::vector<Ref<Material>>& materials,
			std::vector<SkinVertex> skinVertices, Skeleton skeleton, std::vector<AnimationClip> clips);

		const std::vector<MeshVertex>& GetVertices() const { return m_Vertices; }
		const std::vector<uint32_t>& GetIndices() const { return m_Indices; }
		const std::vector<Submesh>& GetSubmeshes() const { return m_Submeshes; }
		const std::vector<Ref<Material>>& GetMaterials() const { return m_Materials; }
		Ref<Material> GetMaterial(uint32_t index) const;

		// Skeleton and clips live inside the Mesh asset: a .glb carries mesh, skin and
		// clips in one file and v1 has no retargeting, so separate clip assets would
		// buy nothing but registry surgery. AnimatorComponent references clips by name.
		bool HasSkeleton() const { return !m_Skeleton.IsEmpty(); }
		const Skeleton& GetSkeleton() const { return m_Skeleton; }
		const std::vector<SkinVertex>& GetSkinVertices() const { return m_SkinVertices; }

		const std::vector<AnimationClip>& GetClips() const { return m_Clips; }
		const AnimationClip* FindClip(const std::string& name) const;

		const Geometry& GetGeometry() const { return m_Geometry; }

		// Vertex stream 1 for skinned draws, parallel to the stream-0 vertices.
		// Null on a mesh with no skin data.
		const Ref<VertexBuffer>& GetSkinVertexBuffer() const { return m_SkinGeometry; }

		// How far a skinned submesh's bind-pose AABB is padded to stand in for the posed
		// one, as a fraction of the box's LARGEST extent - not of each axis. A limb can
		// swing about as far as the rig is long, so a narrow axis needs the same absolute
		// slack as a wide one: CesiumMan stands with its arms down (X extent 0.31 against
		// a height of 1.51) and its walk cycle leaves a per-axis 50% pad on both sides.
		//
		// Exact posed bounds would mean skinning every vertex on the CPU each frame to
		// decide one culling test, which is not a trade worth making. The failure mode
		// here is a character popping at the screen edge if a clip swings wider than this.
		static constexpr float SkinnedBoundsPadding = 0.25f;
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
		static Ref<Mesh> Create(const std::vector<MeshVertex>& vertices, const std::vector<uint32_t>& indices,
			const std::vector<Submesh>& submeshes, const std::vector<Ref<Material>>& materials,
			std::vector<SkinVertex> skinVertices, Skeleton skeleton, std::vector<AnimationClip> clips);
	private:
		void Build();
		void ComputeBounds();
	private:
		std::vector<MeshVertex> m_Vertices;
		std::vector<uint32_t> m_Indices;
		std::vector<Submesh> m_Submeshes;
		std::vector<Ref<Material>> m_Materials;
		AABB m_Bounds;

		// Empty, or exactly parallel to m_Vertices - see SkinVertex.
		std::vector<SkinVertex> m_SkinVertices;
		Skeleton m_Skeleton;
		std::vector<AnimationClip> m_Clips;

		Geometry m_Geometry;
		Ref<VertexBuffer> m_SkinGeometry;
		std::vector<MeshInstanceData> m_InstanceData;

		std::string m_Path;
	};

}

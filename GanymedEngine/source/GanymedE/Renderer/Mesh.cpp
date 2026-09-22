#include "gepch.h"
#include "Mesh.h"

#include <glm/gtc/matrix_transform.hpp>

namespace GanymedE {

	Mesh::Mesh(const std::vector<MeshVertex>& vertices, const std::vector<uint32_t>& indices,
		const std::vector<Submesh>& submeshes, const std::vector<Ref<Material>>& materials)
		: m_Vertices(vertices), m_Indices(indices), m_Submeshes(submeshes), m_Materials(materials)
	{
		Build();
	}

	Mesh::Mesh(const std::vector<MeshVertex>& vertices, const std::vector<uint32_t>& indices,
		const std::vector<Submesh>& submeshes, const std::vector<Ref<Material>>& materials,
		std::vector<SkinVertex> skinVertices, Skeleton skeleton, std::vector<AnimationClip> clips)
		: m_Vertices(vertices), m_Indices(indices), m_Submeshes(submeshes), m_Materials(materials),
		  m_SkinVertices(std::move(skinVertices)), m_Skeleton(std::move(skeleton)), m_Clips(std::move(clips))
	{
		// Both vertex streams are bound with one startVertex, so a partially filled
		// skin stream would read garbage for the tail of the mesh.
		if (!m_SkinVertices.empty() && m_SkinVertices.size() != m_Vertices.size())
		{
			GE_CORE_ERROR("Skin vertex count ({0}) does not match vertex count ({1}) - dropping skin data",
				m_SkinVertices.size(), m_Vertices.size());
			m_SkinVertices.clear();
		}

		Build();
	}

	Ref<Mesh> Mesh::Create(const std::vector<MeshVertex>& vertices, const std::vector<uint32_t>& indices,
		const std::vector<Submesh>& submeshes, const std::vector<Ref<Material>>& materials)
	{
		return CreateRef<Mesh>(vertices, indices, submeshes, materials);
	}

	Ref<Mesh> Mesh::Create(const std::vector<MeshVertex>& vertices, const std::vector<uint32_t>& indices,
		const std::vector<Submesh>& submeshes, const std::vector<Ref<Material>>& materials,
		std::vector<SkinVertex> skinVertices, Skeleton skeleton, std::vector<AnimationClip> clips)
	{
		return CreateRef<Mesh>(vertices, indices, submeshes, materials,
			std::move(skinVertices), std::move(skeleton), std::move(clips));
	}

	Ref<Material> Mesh::GetMaterial(uint32_t index) const
	{
		if (index < m_Materials.size())
			return m_Materials[index];
		return nullptr;
	}

	const AnimationClip* Mesh::FindClip(const std::string& name) const
	{
		for (const AnimationClip& clip : m_Clips)
		{
			if (clip.Name == name)
				return &clip;
		}
		return nullptr;
	}

	void Mesh::Build()
	{
		m_Geometry.Vertices = VertexBuffer::Create(
			m_Vertices.data(),
			(uint32_t)(m_Vertices.size() * sizeof(MeshVertex)),
			{
				{ ShaderDataType::Float3, "a_Position" },
				{ ShaderDataType::Float3, "a_Normal" },
				{ ShaderDataType::Float3, "a_Tangent" },
				{ ShaderDataType::Float2, "a_TexCoord" }
			});

		// Skin attributes ride a second stream so static meshes pay nothing for
		// them: widening MeshVertex would add 32 bytes to every vertex in the
		// engine to serve the few that are rigged.
		if (!m_SkinVertices.empty())
		{
			m_SkinGeometry = VertexBuffer::Create(
				m_SkinVertices.data(),
				(uint32_t)(m_SkinVertices.size() * sizeof(SkinVertex)),
				{
					{ ShaderDataType::Float4, "a_JointIndices" },
					{ ShaderDataType::Float4, "a_JointWeights" }
				});
		}

		// The per-instance transform/ID buffer is gone: bgfx allocates instance
		// data from a transient pool at submit time instead of keeping a
		// divisor-1 vertex buffer around.
		m_InstanceData.reserve(MaxInstancesPerDraw);

		m_Geometry.Indices = IndexBuffer::Create(m_Indices.data(), (uint32_t)m_Indices.size());

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

			// A skinned submesh's vertices are the bind pose; the palette moves them
			// at draw time, so the measured box is not the box that gets drawn. Pad
			// it rather than compute the real thing per frame.
			if (submesh.IsSkinned && !first)
			{
				const glm::vec3 extent = submesh.Bounds.Max - submesh.Bounds.Min;
				const float pad = glm::max(glm::max(extent.x, extent.y), extent.z) * SkinnedBoundsPadding;
				submesh.Bounds = AABB(submesh.Bounds.Min - pad, submesh.Bounds.Max + pad);
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
		m_InstanceData.clear();

		if (!data || count == 0)
			return;

		if (count > MaxInstancesPerDraw)
			count = MaxInstancesPerDraw;

		m_InstanceData.assign(data, data + count);
	}

	namespace {

		bool ComputeJointFrame(const Skeleton& skeleton, const glm::mat4& skinTransform,
			const std::vector<glm::mat4>& palette, int32_t joint, glm::mat4& outFrame)
		{
			if (joint < 0 || (size_t)joint >= skeleton.InverseBind.size()
				|| (size_t)joint >= palette.size())
			{
				return false;
			}

			const glm::mat4& inverseBind = skeleton.InverseBind[(size_t)joint];
			const float det = glm::determinant(inverseBind);
			if (glm::abs(det) < 1e-8f)
				return false;

			// inverse(InverseBind) is LocalTransform * bindGlobal, so it is exactly this frame in
			// the bind pose: translation in metres, basis carrying LocalTransform's scale. That
			// scale is cancelled for *vertices* by the 1/scale inside the palette, and nothing
			// cancels it for a socket - left in, an attached entity renders at 1% and Offset
			// silently means centimetres.
			const glm::mat4 bindGlobal = glm::inverse(inverseBind);
			glm::mat4 jointGlobal = skinTransform * palette[(size_t)joint] * bindGlobal;

			// Divided out per column rather than normalised to unit length, so a clip that scales
			// the joint still scales what is attached to it - the palette's scale is relative to
			// bind, and only the bind part is the authoring artifact. For a rig whose mesh node is
			// identity every column is already 1 and this loop does nothing.
			for (int column = 0; column < 3; column++)
			{
				const float bindScale = glm::length(glm::vec3(bindGlobal[column]));
				if (bindScale > 1e-6f)
					jointGlobal[column] /= bindScale;
			}

			outFrame = jointGlobal;
			return true;
		}

		void VerifyJointFrameOnce()
		{
			static bool done = false;
			if (done)
				return;
			done = true;

			// Centimetre-authored joint, metre-authored vertices, LocalTransform 0.01 between
			// them — the Meshy case that put a socket at 141 m instead of 1.41. Palette is the
			// bind-pose skinning matrix Global_cm * InverseBind, matching AnimationSystem.
			const glm::mat4 skin = glm::scale(glm::mat4(1.0f), glm::vec3(0.01f));
			const glm::mat4 globalCm = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 141.4f, 0.0f));
			const glm::mat4 inverseBind = glm::inverse(skin * globalCm);

			Skeleton skeleton;
			skeleton.ParentIndices = { -1 };
			skeleton.InverseBind = { inverseBind };
			skeleton.LocalRestPose = { JointPose{} };
			skeleton.JointNames = { "RightHand" };

			const std::vector<glm::mat4> palette{ globalCm * inverseBind };

			glm::mat4 frame{ 1.0f };
			if (!ComputeJointFrame(skeleton, skin, palette, 0, frame))
			{
				GE_CORE_ERROR("TryGetJointFrame self-check: centimetre case returned false");
				return;
			}

			const glm::vec3 origin = glm::vec3(frame[3]);
			const float expectedY = 141.4f * 0.01f;
			if (glm::abs(origin.y - expectedY) > 1e-4f || glm::abs(origin.x) > 1e-4f
				|| glm::abs(origin.z) > 1e-4f)
			{
				GE_CORE_ERROR("TryGetJointFrame self-check: expected origin (0, {0}, 0), got "
					"({1}, {2}, {3})", expectedY, origin.x, origin.y, origin.z);
			}

			for (int column = 0; column < 3; column++)
			{
				const float axisLen = glm::length(glm::vec3(frame[column]));
				if (glm::abs(axisLen - 1.0f) > 1e-4f)
				{
					GE_CORE_ERROR("TryGetJointFrame self-check: expected unit basis, column {0} "
						"length {1}", column, axisLen);
				}
			}

			glm::mat4 ignored{ 1.0f };
			if (ComputeJointFrame(skeleton, skin, palette, -1, ignored)
				|| ComputeJointFrame(skeleton, skin, palette, 1, ignored))
			{
				GE_CORE_ERROR("TryGetJointFrame self-check: out-of-range joint should fail");
			}

			Skeleton singular = skeleton;
			singular.InverseBind[0] = glm::mat4(0.0f);
			if (ComputeJointFrame(singular, skin, palette, 0, ignored))
				GE_CORE_ERROR("TryGetJointFrame self-check: singular InverseBind should fail");
		}

	}

	bool TryGetJointFrame(const Mesh& mesh, const std::vector<glm::mat4>& palette,
		int32_t joint, glm::mat4& outFrame)
	{
		VerifyJointFrameOnce();

		glm::mat4 skinTransform{ 1.0f };
		for (const Submesh& submesh : mesh.GetSubmeshes())
		{
			if (submesh.IsSkinned)
			{
				skinTransform = submesh.LocalTransform;
				break;
			}
		}

		return ComputeJointFrame(mesh.GetSkeleton(), skinTransform, palette, joint, outFrame);
	}

}

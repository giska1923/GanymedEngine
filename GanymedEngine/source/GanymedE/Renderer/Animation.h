#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace GanymedE {

	// A joint's transform as TRS rather than a matrix: rotation keys have to slerp,
	// and pose blending (a later milestone) needs poses, not matrices.
	struct JointPose
	{
		glm::vec3 Translation{ 0.0f };
		glm::quat Rotation{ 1.0f, 0.0f, 0.0f, 0.0f };
		glm::vec3 Scale{ 1.0f };

		glm::mat4 ToMatrix() const
		{
			return glm::translate(glm::mat4(1.0f), Translation)
				* glm::mat4_cast(Rotation)
				* glm::scale(glm::mat4(1.0f), Scale);
		}
	};

	// Joints are flat arrays, never entities. TransformComponent stores Euler angles
	// (lossy for quaternion keys) and the hierarchy walk does per-level UUID lookups;
	// a 60-joint rig would pay all of that per joint per frame and flood the
	// serializer and ComponentList for no benefit. A flat array is also the shape the
	// bone palette has to be uploaded in anyway.
	struct Skeleton
	{
		// One uniform mat4[MaxBones] in the skinning shader. Humanoid rigs run 60-90
		// joints; rigs over the limit are warned about at import and clamped at upload.
		static constexpr uint32_t MaxBones = 128;

		// Sorted parents-before-children, so one forward pass composes globals.
		// glTF does NOT guarantee this for skin.joints, so the importer topologically
		// sorts and remaps the JOINTS_0 indices to match.
		std::vector<int32_t>     ParentIndices;   // -1 = root joint
		std::vector<glm::mat4>   InverseBind;
		std::vector<JointPose>   LocalRestPose;
		std::vector<std::string> JointNames;

		// World transform of whatever sits ABOVE the root joints in the glTF scene
		// graph - Blender's "Armature" node, a Y-up correction node, and so on.
		// The inverse bind matrices come from the file and already account for it, so
		// leaving it out of the global composition would skin the mesh by its inverse.
		//
		// Kept out of LocalRestPose because animation channels replace joint locals
		// wholesale and would otherwise overwrite it.
		// Seeds root joints instead of identity when composing globals. Folds two things the
		// inverse binds assume: the transform of whatever sits above the root joints, and the
		// inverse of the skinned mesh node's transform, which glTF requires be cancelled rather
		// than applied. Kept out of LocalRestPose so animation channels cannot overwrite it.
		glm::mat4 RootTransform{ 1.0f };

		bool IsEmpty() const { return ParentIndices.empty(); }
		uint32_t JointCount() const { return (uint32_t)ParentIndices.size(); }
	};

	struct AnimationClip
	{
		struct Channel
		{
			enum class Path : uint8_t { Translation, Rotation, Scale };
			enum class Interp : uint8_t { Linear, Step };

			uint32_t Joint = 0;
			Path     Target = Path::Translation;
			Interp   Mode = Interp::Linear;

			// Times are sorted, sampled by binary search. Values are xyz for
			// translation/scale and a quaternion in **xyzw** order for rotation -
			// glTF's layout, not glm::quat's (w, x, y, z) constructor order.
			std::vector<float>     Times;
			std::vector<glm::vec4> Values;
		};

		std::string Name;
		float Duration = 0.0f;
		std::vector<Channel> Channels;
	};

	// Skin weights ride a second vertex stream rather than widening MeshVertex:
	// widening taxes every static mesh 32 bytes a vertex, forces a cache migration
	// for all existing content, and touches the one struct the cache memcpy's whole.
	//
	// Joint indices are floats because AttribTypeFromShaderType forces every
	// attribute to Float; Uint8 packing is an optimization, not a requirement.
	//
	// A mesh's skin vertices are either empty or exactly parallel to its
	// MeshVertex array - both bgfx streams are bound with the same startVertex, so
	// vertices belonging to static submeshes of a mixed file carry zeroed entries.
	struct SkinVertex
	{
		glm::vec4 JointIndices{ 0.0f };
		glm::vec4 JointWeights{ 0.0f };
	};

}

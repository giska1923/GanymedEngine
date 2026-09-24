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

	// Joint globals at `time`, in the rig's own joint space - **not** mesh space: that is one more
	// multiply by Mesh::GetSkinTransform(), and on a centimetre rig the difference is 100x. A
	// channel overwrites only the path it drives; everything else stays on LocalRestPose. Roots
	// are seeded from RootTransform.
	//
	// This is the pose AnimationSystem samples before multiplying InverseBind to make a
	// skinning palette. The clip inspector needs the same pose (head / hips / root at t=0),
	// so both call this rather than re-deriving the sampler.
	//
	// False if LocalRestPose / InverseBind / ParentIndices disagree on JointCount.
	// Does not log: AnimationSystem attributes that to the entity.
	// Defined in AnimationSystem.cpp — that was already the sampler, and the inspector is
	// the second caller.
	bool SampleClipGlobals(const Skeleton& skeleton, const AnimationClip* clip, float time,
		std::vector<JointPose>& localsScratch, std::vector<glm::mat4>& outGlobals);

	// Palette[i] = Global[i] * InverseBind[i] at `time`. `clip == nullptr` is the rest pose.
	// Mesh caches that rest result as GetRestPalette so a rigged mesh can be submitted through
	// vs_PhongSkinned without an AnimatorComponent — SubmitMesh is not equivalent once
	// LocalTransform is a unit conversion (Meshy 0.01).
	//
	// Same failure rule as SampleClipGlobals: false and empty outPalette, no log.
	bool BuildSkinningPalette(const Skeleton& skeleton, const AnimationClip* clip, float time,
		std::vector<JointPose>& localsScratch, std::vector<glm::mat4>& globalsScratch,
		std::vector<glm::mat4>& outPalette);

	// Evaluated form of AimOffsetComponent. Animation.h does not include the component;
	// the system copies the fields across. Subtree pointers are caller-owned and must
	// stay valid for the duration of ApplyAimOffset. Each subtree includes its joint.
	struct AimOffsetChain
	{
		static constexpr int MaxJoints = 4;

		int Count = 0;
		int32_t Joints[MaxJoints]{};
		float Weights[MaxJoints]{};
		float PitchLimit = 1.0f;
		float YawLimit = 1.5707963267948966f;

		const uint32_t* Subtrees[MaxJoints]{};
		uint32_t SubtreeCounts[MaxJoints]{};
	};

	// Rotates each chain joint — and everything below it — about that joint's current
	// origin, in the space SampleClipGlobals writes, by its normalised share of yaw then
	// pitch. Operates on globals, after sampling and before InverseBind. SampleClipGlobals
	// itself stays pure: the clip inspector measures the clip, not the clip plus whatever
	// aim the entity last had.
	//
	// `upSkin` and `rightSkin` are character axes already in that space. Positive pitch
	// looks up (right-hand about `rightSkin`). Positive yaw turns the chest toward the
	// character's left (right-hand about `upSkin`). The pitch axis is `rightSkin` turned
	// by the full clamped yaw, so pitching while twisted tilts along the aim rather than
	// along the hips. Angles are clamped to the chain's limits here, so Lua and the
	// editor preview share one rule.
	//
	// Pitch and yaw of zero leave `globals` untouched, bit for bit — the early-out is the
	// guarantee, not glm::rotate(0).
	void ApplyAimOffset(const Skeleton& skeleton, const AimOffsetChain& chain,
		const glm::vec3& upSkin, const glm::vec3& rightSkin, float pitch, float yaw,
		std::vector<glm::mat4>& globals);

	// One arm for SolveTwoBone: shoulder, elbow and wrist joints, e.g. RightArm, RightForeArm,
	// RightHand. Lower must be in Upper's subtree and End in Lower's, or the solve is refused.
	// Subtree pointers are caller-owned, [0] Upper, [1] Lower, [2] End, each including its
	// joint, and must stay valid for the duration of SolveTwoBone.
	struct TwoBoneChain
	{
		int32_t Upper = -1;
		int32_t Lower = -1;
		int32_t End = -1;

		// Bend direction, in the space SampleClipGlobals writes, used when the pole is within a
		// few degrees of the shoulder→target line and so no longer defines a plane.
		glm::vec3 PoleHint{ 0.0f, -1.0f, 0.0f };

		const uint32_t* Subtrees[3]{};
		uint32_t SubtreeCounts[3]{};
	};

	struct TwoBoneResult
	{
		// False: the chain, a bone length or an input was unusable, and globals are untouched.
		// Reached and Stretch mean nothing then.
		bool Valid = false;
		// Both describe the full-weight target, whatever the weight, so a readout shows where
		// the marker is relative to the arm rather than where a faded solve happened to land.
		// Reached is false exactly when the distance had to be clamped.
		bool Reached = false;
		float Stretch = 0.0f; // shoulder→target distance / (upper + lower length); > 1 is out of reach
	};

	// Analytic two-bone IK over globals, after sampling and the aim offset, before InverseBind.
	// Moves End's origin onto `target` where the arm can reach it — otherwise as far along the
	// shoulder→target line as it does — with the elbow in the plane of shoulder, target and
	// `pole`, then turns End to `targetRotation`. Upper, Lower and End each turn with their whole
	// subtree, about their own origin, so bone lengths and joint scale are kept and End's
	// children follow. `target` and `pole` are positions, `targetRotation` a unit orientation,
	// all in the space SampleClipGlobals writes.
	//
	// Bone lengths come from the current globals, not the rest pose. `weight` in [0, 1] blends
	// the goal from the current wrist frame to the target (position lerp, rotation slerp) and
	// then solves fully; weight 0 leaves `globals` untouched, bit for bit.
	TwoBoneResult SolveTwoBone(const Skeleton& skeleton, const TwoBoneChain& chain,
		const glm::vec3& target, const glm::quat& targetRotation, const glm::vec3& pole,
		float weight, std::vector<glm::mat4>& globals);

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

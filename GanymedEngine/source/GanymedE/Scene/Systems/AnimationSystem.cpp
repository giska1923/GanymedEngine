#include "gepch.h"
#include "AnimationSystem.h"

#include "GanymedE/Assets/AssetManager.h"
#include "GanymedE/Renderer/Mesh.h"
#include "GanymedE/Scene/Entity.h"
#include "GanymedE/Scene/Scene.h"

#ifdef GE_DEBUG
#include <chrono>
#endif
#include <cmath>

namespace GanymedE {

	namespace {

		struct KeyPair
		{
			size_t Lower = 0;
			size_t Upper = 0;
			float Alpha = 0.0f;
		};

		// Times are sorted, so binary search rather than the linear scan a naive sampler uses -
		// a 3s clip at 30fps is ~100 keys per channel, times ~60 channels, every frame.
		KeyPair FindKeys(const std::vector<float>& times, float time)
		{
			KeyPair key;

			if (times.size() < 2 || time <= times.front())
				return key;

			if (time >= times.back())
			{
				key.Lower = key.Upper = times.size() - 1;
				return key;
			}

			key.Upper = (size_t)(std::upper_bound(times.begin(), times.end(), time) - times.begin());
			key.Lower = key.Upper - 1;

			const float span = times[key.Upper] - times[key.Lower];
			key.Alpha = span > 0.0f ? (time - times[key.Lower]) / span : 0.0f;
			return key;
		}

		int32_t ResolveJointIndex(const std::vector<std::string>& names, const std::string& joint,
			int32_t resolved)
		{
			if (joint.empty())
				return -1;

			if (resolved >= 0 && (size_t)resolved < names.size() && names[(size_t)resolved] == joint)
				return resolved;

			for (int32_t i = 0; i < (int32_t)names.size(); i++)
			{
				if (names[(size_t)i] == joint)
					return i;
			}

			return -1;
		}

		// Columns of a linear transform, Gram-Schmidt'd into a rotation. Scale in
		// GetSkinTransform (Meshy's 0.01) must not survive into the aim axes; a reflection
		// keeps its handedness via the last column's sign.
		glm::mat3 OrthonormalRotation(const glm::mat3& linear)
		{
			glm::vec3 x = linear[0];
			glm::vec3 y = linear[1];
			const float lx = glm::length(x);
			if (lx < 1e-8f)
				return glm::mat3(1.0f);
			x /= lx;

			y -= x * glm::dot(y, x);
			const float ly = glm::length(y);
			if (ly < 1e-8f)
				return glm::mat3(1.0f);
			y /= ly;

			glm::vec3 z = glm::cross(x, y);
			if (glm::dot(z, glm::vec3(linear[2])) < 0.0f)
				z = -z;
			return glm::mat3(x, y, z);
		}

		glm::vec3 ModelForwardVector(AimOffsetComponent::Axis forward)
		{
			switch (forward)
			{
				case AimOffsetComponent::Axis::NegZ: return { 0.0f, 0.0f, -1.0f };
				case AimOffsetComponent::Axis::PosX: return { 1.0f, 0.0f, 0.0f };
				case AimOffsetComponent::Axis::NegX: return { -1.0f, 0.0f, 0.0f };
				case AimOffsetComponent::Axis::PosZ:
				default: return { 0.0f, 0.0f, 1.0f };
			}
		}

		// Mesh-space character axes into the space SampleClipGlobals writes. right is
		// forward × up, which is -X when the rig faces +Z — the same -X the Meshy spine
		// already has, and the axis a positive pitch rotates about to look up.
		bool AimAxesInSkinSpace(const glm::mat4& skin, AimOffsetComponent::Axis forward,
			glm::vec3& upSkin, glm::vec3& rightSkin)
		{
			const float det = glm::determinant(skin);
			// det of a uniform scale is s^3, so Meshy's 0.01 is 1e-6 and still has to pass.
			// 1e-20 rejects a zero axis and a non-finite matrix, not a unit conversion.
			if (!std::isfinite(det) || std::abs(det) < 1e-20f)
				return false;

			const glm::mat3 rotation = OrthonormalRotation(glm::mat3(glm::inverse(skin)));
			const glm::vec3 upMesh{ 0.0f, 1.0f, 0.0f };
			const glm::vec3 rightMesh = glm::cross(ModelForwardVector(forward), upMesh);
			upSkin = rotation * upMesh;
			rightSkin = rotation * rightMesh;
			if (glm::length(upSkin) < 1e-8f || glm::length(rightSkin) < 1e-8f)
				return false;

			upSkin = glm::normalize(upSkin);
			rightSkin = glm::normalize(rightSkin);
			return true;
		}

		void BuildSubtree(const Skeleton& skeleton, int32_t joint, std::vector<uint32_t>& out)
		{
			out.clear();
			const uint32_t count = skeleton.JointCount();
			if (joint < 0 || (uint32_t)joint >= count)
				return;

			for (uint32_t k = 0; k < count; k++)
			{
				for (int32_t p = (int32_t)k; p >= 0; )
				{
					if (p == joint)
					{
						out.push_back(k);
						break;
					}

					if ((uint32_t)p >= count)
						break;

					const int32_t parent = skeleton.ParentIndices[(uint32_t)p];
					if (parent == p)
						break;
					p = parent;
				}
			}
		}

		void BindSubtrees(const Skeleton& skeleton, AimOffsetChain& chain,
			std::array<std::vector<uint32_t>, AimOffsetChain::MaxJoints>& storage)
		{
			for (int i = 0; i < AimOffsetChain::MaxJoints; i++)
			{
				if (i < chain.Count)
					BuildSubtree(skeleton, chain.Joints[i], storage[(size_t)i]);
				else
					storage[(size_t)i].clear();

				chain.Subtrees[i] = storage[(size_t)i].data();
				chain.SubtreeCounts[i] = (uint32_t)storage[(size_t)i].size();
			}
		}

#ifdef GE_DEBUG
		bool SameMatrix(const glm::mat4& a, const glm::mat4& b)
		{
			for (int c = 0; c < 4; c++)
			{
				for (int r = 0; r < 4; r++)
				{
					if (a[c][r] != b[c][r])
						return false;
				}
			}
			return true;
		}

		float RotationDelta(const glm::mat4& rest, const glm::mat4& posed)
		{
			const glm::quat delta = glm::normalize(
				glm::inverse(glm::quat_cast(rest)) * glm::quat_cast(posed));
			return glm::angle(delta);
		}

		Skeleton MakeProbeSkeleton()
		{
			Skeleton skeleton;
			const int count = 4;
			skeleton.ParentIndices = { -1, 0, 1, 2 };
			skeleton.InverseBind.assign(count, glm::mat4(1.0f));
			skeleton.LocalRestPose.resize(count);
			skeleton.JointNames = { "Hips", "SpineA", "SpineB", "Neck" };
			for (int i = 0; i < count; i++)
				skeleton.LocalRestPose[(size_t)i].Translation = { 0.0f, i == 0 ? 1.0f : 0.2f, 0.0f };
			return skeleton;
		}

		AimOffsetChain ProbeChain(const Skeleton& skeleton, std::array<int32_t, 2> joints,
			std::array<float, 2> weights,
			std::array<std::vector<uint32_t>, AimOffsetChain::MaxJoints>& storage)
		{
			AimOffsetChain chain;
			chain.Count = 2;
			chain.Joints[0] = joints[0];
			chain.Joints[1] = joints[1];
			chain.Weights[0] = weights[0];
			chain.Weights[1] = weights[1];
			chain.PitchLimit = 1.0f;
			chain.YawLimit = 1.5707963267948966f;
			BindSubtrees(skeleton, chain, storage);
			return chain;
		}

		void FailProbe(const std::string& message)
		{
			GE_CORE_ERROR("Aim offset probe failed: {0}", message);
			GE_CORE_ASSERT(false, "Aim offset probe failed");
		}

		// The A1 checks that do not need a scene: bit-identical zero, pitch sign and
		// magnitude, the clamp, and that a twisted pitch rotates about the yawed right.
		// Runs once, from the first Evaluate, so a wrong axis fails the editor at boot
		// rather than in a screenshot.
		void RunAimOffsetProbes()
		{
			const Skeleton skeleton = MakeProbeSkeleton();
			std::vector<JointPose> locals;
			std::vector<glm::mat4> rest;
			if (!SampleClipGlobals(skeleton, nullptr, 0.0f, locals, rest))
			{
				FailProbe("probe skeleton did not sample");
				return;
			}

			const glm::vec3 up{ 0.0f, 1.0f, 0.0f };
			const glm::vec3 right{ -1.0f, 0.0f, 0.0f };
			const int neck = 3;
			const int hips = 0;

			std::array<std::vector<uint32_t>, AimOffsetChain::MaxJoints> storage;
			{
				std::vector<glm::mat4> posed = rest;
				AimOffsetChain chain = ProbeChain(skeleton, { 1, 2 }, { 0.25f, 0.75f }, storage);
				ApplyAimOffset(skeleton, chain, up, right, 0.0f, 0.0f, posed);
				for (size_t i = 0; i < rest.size(); i++)
				{
					if (!SameMatrix(rest[i], posed[i]))
					{
						FailProbe("pitch = yaw = 0 changed a global");
						return;
					}
				}
			}

			{
				std::vector<glm::mat4> posed = rest;
				AimOffsetChain chain = ProbeChain(skeleton, { 1, 2 }, { 0.25f, 0.75f }, storage);
				ApplyAimOffset(skeleton, chain, up, right, 0.3f, 0.0f, posed);

				if (SameMatrix(rest[hips], posed[hips]) == false)
				{
					FailProbe("pitch rotated the hips, which are not in the chain");
					return;
				}

				const float neckDelta = RotationDelta(rest[(size_t)neck], posed[(size_t)neck]);
				if (std::abs(neckDelta - 0.3f) > 1e-4f)
				{
					FailProbe("neck pitch delta is " + std::to_string(neckDelta) + ", expected 0.3");
					return;
				}

				const glm::vec3 forward = glm::vec3(posed[(size_t)neck][2]);
				if (!(forward.y > 0.0f))
				{
					FailProbe("positive pitch did not raise the neck's +Z");
					return;
				}
			}

			{
				std::vector<glm::mat4> posed = rest;
				AimOffsetChain chain = ProbeChain(skeleton, { 1, 2 }, { 0.25f, 0.75f }, storage);
				ApplyAimOffset(skeleton, chain, up, right, 2.0f, 0.0f, posed);
				const float neckDelta = RotationDelta(rest[(size_t)neck], posed[(size_t)neck]);
				if (std::abs(neckDelta - 1.0f) > 1e-4f)
				{
					FailProbe("clamped neck pitch delta is " + std::to_string(neckDelta) + ", expected 1");
					return;
				}
			}

			{
				std::vector<glm::mat4> posed = rest;
				AimOffsetChain chain = ProbeChain(skeleton, { 1, 2 }, { 1.0f, 0.0f }, storage);
				chain.Count = 1;
				BindSubtrees(skeleton, chain, storage);
				ApplyAimOffset(skeleton, chain, up, right, 0.0f, 0.5f, posed);
				const glm::vec3 forward = glm::vec3(posed[(size_t)neck][2]);
				if (!(forward.x > 0.0f))
				{
					FailProbe("positive yaw did not turn +Z toward +X (the character's left)");
					return;
				}
			}

			{
				std::vector<glm::mat4> posed = rest;
				AimOffsetChain chain = ProbeChain(skeleton, { 2, 2 }, { 1.0f, 0.0f }, storage);
				chain.Count = 1;
				chain.Joints[0] = 2;
				BindSubtrees(skeleton, chain, storage);
				ApplyAimOffset(skeleton, chain, up, right, 0.3f, 1.0f, posed);

				const glm::vec3 pitchAxis = glm::angleAxis(1.0f, up) * right;
				const glm::quat expected = glm::normalize(
					glm::angleAxis(0.3f, pitchAxis) * glm::angleAxis(1.0f, up));
				const glm::quat wrong = glm::normalize(
					glm::angleAxis(0.3f, right) * glm::angleAxis(1.0f, up));
				const glm::quat actual = glm::normalize(
					glm::inverse(glm::quat_cast(rest[2])) * glm::quat_cast(posed[2]));

				// 2 acos(|dot|) is the rotation angle, and the absolute value ignores the
				// q / -q double cover quat_cast and angleAxis do not agree on.
				const auto AngleBetween = [](const glm::quat& a, const glm::quat& b)
				{
					const float d = glm::clamp(std::abs(glm::dot(a, b)), 0.0f, 1.0f);
					return 2.0f * std::acos(d);
				};
				const float expectedError = AngleBetween(expected, actual);
				const float wrongError = AngleBetween(wrong, actual);
				if (expectedError > 1e-4f || !(wrongError > 1e-3f))
				{
					FailProbe("twisted pitch error " + std::to_string(expectedError)
						+ " (wrong-axis error " + std::to_string(wrongError) + ")");
					return;
				}
			}

			{
				glm::vec3 upSkin, rightSkin;
				if (!AimAxesInSkinSpace(glm::mat4(1.0f), AimOffsetComponent::Axis::PosZ, upSkin, rightSkin)
					|| glm::length(upSkin - up) > 1e-5f
					|| glm::length(rightSkin - right) > 1e-5f)
				{
					FailProbe("identity skin, +Z forward, did not produce up=+Y right=-X");
					return;
				}

				const glm::mat4 scaled = glm::scale(glm::mat4(1.0f), glm::vec3(0.01f));
				if (!AimAxesInSkinSpace(scaled, AimOffsetComponent::Axis::PosZ, upSkin, rightSkin)
					|| glm::length(upSkin - up) > 1e-5f
					|| glm::length(rightSkin - right) > 1e-5f)
				{
					FailProbe("0.01 skin scale changed the aim axes");
					return;
				}
			}

			{
				Skeleton line;
				const int count = 32;
				line.ParentIndices.resize(count);
				line.InverseBind.assign(count, glm::mat4(1.0f));
				line.LocalRestPose.resize(count);
				for (int i = 0; i < count; i++)
				{
					line.ParentIndices[(size_t)i] = i == 0 ? -1 : i - 1;
					line.LocalRestPose[(size_t)i].Translation = { 0.0f, 0.05f, 0.0f };
				}

				std::vector<glm::mat4> globals;
				if (!SampleClipGlobals(line, nullptr, 0.0f, locals, globals))
				{
					FailProbe("timing skeleton did not sample");
					return;
				}

				AimOffsetChain chain;
				chain.Count = 3;
				chain.Joints[0] = 4;
				chain.Joints[1] = 5;
				chain.Joints[2] = 6;
				chain.Weights[0] = 0.2f;
				chain.Weights[1] = 0.3f;
				chain.Weights[2] = 0.5f;
				chain.PitchLimit = 1.0f;
				chain.YawLimit = 1.5707963267948966f;
				BindSubtrees(line, chain, storage);

				const auto start = std::chrono::steady_clock::now();
				constexpr int iterations = 2000;
				for (int i = 0; i < iterations; i++)
					ApplyAimOffset(line, chain, up, right, 0.3f, 0.4f, globals);
				const auto elapsed = std::chrono::steady_clock::now() - start;
				const double microseconds = std::chrono::duration<double, std::micro>(elapsed).count() / iterations;
				GE_CORE_INFO("Aim offset probe: {0} us/call over {1} iterations, 32-joint line, 3 chain joints (Debug)",
					microseconds, iterations);
			}
		}
#endif

	}

	void AnimationSystem::OnRuntimeStart()
	{
		// Play starts at the head of the clip whatever the editor was scrubbed to. The runtime
		// scene is a copy, so the editor scene keeps its scrub position for when play stops.
		for (auto [entity, animator, meshComponent, aim] : View<AnimView>())
		{
			(void)entity;
			(void)meshComponent;
			(void)aim;
			animator.Time = 0.0f;
		}

		m_WarnedClips.clear();
		m_WarnedAimJoints.clear();
		m_WarnedAimAxes.clear();
		m_AimSubtrees.clear();
	}

	void AnimationSystem::OnUpdate(Timestep ts)
	{
		Evaluate(ts, true);
	}

	void AnimationSystem::OnUpdateEditor(Timestep ts)
	{
		Evaluate(ts, false);
	}

	void AnimationSystem::Evaluate(Timestep ts, bool advanceTime)
	{
#ifdef GE_DEBUG
		static bool s_ProbesRun = false;
		if (!s_ProbesRun)
		{
			s_ProbesRun = true;
			RunAimOffsetProbes();
		}
#endif

		for (auto [entity, animator, meshComponent, aim] : View<AnimView>())
		{
			const Ref<Mesh>& mesh = meshComponent.Mesh.Get();
			if (!mesh || !mesh->HasSkeleton())
			{
				// An animator on a static mesh is a user error, not a crash. Empty palette
				// used to mean "draw static"; RenderSystem now skins any HasSkeleton() mesh
				// from GetRestPalette() when the animator palette is empty.
				animator.Palette.clear();
				if (aim)
					aim->Resolved.fill(-1);
				continue;
			}

			const AnimationClip* clip = ResolveClip(entity, *mesh, animator.Clip);

			if (advanceTime && animator.Playing && clip && clip->Duration > 0.0f)
			{
				animator.Time += ts * animator.Speed;

				if (animator.Loop)
				{
					animator.Time = std::fmod(animator.Time, clip->Duration);
					if (animator.Time < 0.0f) // negative Speed rewinds past the start
						animator.Time += clip->Duration;
				}
				else
				{
					animator.Time = std::clamp(animator.Time, 0.0f, clip->Duration);
				}
			}

			const Skeleton& skeleton = mesh->GetSkeleton();
			if (!SampleClipGlobals(skeleton, clip, animator.Time, m_Locals, m_Globals))
			{
				GE_CORE_ERROR("Skeleton arrays disagree on joint count - skipping palette");
				animator.Palette.clear();
				continue;
			}

			// After sampling, before InverseBind. SampleClipGlobals stays the clip-only
			// path the inspector calls; the bend is entity state and does not belong in it.
			if (aim)
				ApplyAim(entity, *aim, *mesh);

			const uint32_t jointCount = skeleton.JointCount();
			animator.Palette.resize(jointCount);
			for (uint32_t i = 0; i < jointCount; i++)
				animator.Palette[i] = m_Globals[i] * skeleton.InverseBind[i];
		}
	}

	const AnimationClip* AnimationSystem::ResolveClip(entt::entity entity, const Mesh& mesh,
		const std::string& name)
	{
		// No clip selected is a legitimate state - hold the bind pose, quietly.
		if (name.empty())
			return nullptr;

		if (const AnimationClip* clip = mesh.FindClip(name))
		{
			m_WarnedClips.erase(entity);
			return clip;
		}

		// The failure mode of referencing clips by name: a rename in the DCC silently detaches
		// the animator on re-export. Warn per distinct bad name, so fixing one typo into another
		// is still reported, but a permanently broken reference does not spam every frame.
		auto [it, inserted] = m_WarnedClips.try_emplace(entity, name);
		if (inserted || it->second != name)
		{
			it->second = name;
			GE_CORE_WARN("Animator references clip '{0}', which mesh '{1}' does not have - "
				"holding the bind pose", name, mesh.GetPath());
		}

		return nullptr;
	}

	bool SampleClipGlobals(const Skeleton& skeleton, const AnimationClip* clip, float time,
		std::vector<JointPose>& localsScratch, std::vector<glm::mat4>& outGlobals)
	{
		const uint32_t jointCount = skeleton.JointCount();
		if (skeleton.LocalRestPose.size() != jointCount
			|| skeleton.InverseBind.size() != jointCount
			|| skeleton.ParentIndices.size() != jointCount)
		{
			outGlobals.clear();
			return false;
		}

		localsScratch = skeleton.LocalRestPose;

		if (clip)
		{
			for (const AnimationClip::Channel& channel : clip->Channels)
			{
				if (channel.Joint >= jointCount || channel.Times.empty() || channel.Values.empty()
					|| channel.Values.size() != channel.Times.size())
				{
					continue;
				}

				KeyPair key = FindKeys(channel.Times, time);
				if (channel.Mode == AnimationClip::Channel::Interp::Step)
					key.Alpha = 0.0f;

				const glm::vec4& a = channel.Values[key.Lower];
				const glm::vec4& b = channel.Values[key.Upper];
				JointPose& pose = localsScratch[channel.Joint];

				switch (channel.Target)
				{
					case AnimationClip::Channel::Path::Translation:
						pose.Translation = glm::mix(glm::vec3(a), glm::vec3(b), key.Alpha);
						break;

					case AnimationClip::Channel::Path::Rotation:
						// Values are xyzw (glTF's order); glm::quat's ctor takes (w, x, y, z).
						// slerp, not lerp: lerping quaternions makes rotation speed dip through
						// the middle of every key interval, and glm::slerp also picks the short
						// way round when consecutive keys land on opposite hemispheres.
						pose.Rotation = glm::normalize(glm::slerp(
							glm::quat(a.w, a.x, a.y, a.z),
							glm::quat(b.w, b.x, b.y, b.z), key.Alpha));
						break;

					case AnimationClip::Channel::Path::Scale:
						pose.Scale = glm::mix(glm::vec3(a), glm::vec3(b), key.Alpha);
						break;
				}
			}
		}

		outGlobals.resize(jointCount);
		for (uint32_t i = 0; i < jointCount; i++)
		{
			const int32_t parent = skeleton.ParentIndices[i];
			const glm::mat4& base = (parent >= 0 && (uint32_t)parent < jointCount)
				? outGlobals[(uint32_t)parent] : skeleton.RootTransform;
			outGlobals[i] = base * localsScratch[i].ToMatrix();
		}

		return true;
	}

	bool BuildSkinningPalette(const Skeleton& skeleton, const AnimationClip* clip, float time,
		std::vector<JointPose>& localsScratch, std::vector<glm::mat4>& globalsScratch,
		std::vector<glm::mat4>& outPalette)
	{
		if (!SampleClipGlobals(skeleton, clip, time, localsScratch, globalsScratch))
		{
			outPalette.clear();
			return false;
		}

		const uint32_t jointCount = skeleton.JointCount();
		outPalette.resize(jointCount);
		for (uint32_t i = 0; i < jointCount; i++)
			outPalette[i] = globalsScratch[i] * skeleton.InverseBind[i];
		return true;
	}

	void ApplyAimOffset(const Skeleton& skeleton, const AimOffsetChain& chain,
		const glm::vec3& upSkin, const glm::vec3& rightSkin, float pitch, float yaw,
		std::vector<glm::mat4>& globals)
	{
		GE_PROFILE_SCOPE("AnimationSystem::ApplyAimOffset");

		if (chain.Count <= 0 || globals.size() != skeleton.JointCount())
			return;

		const auto Limit = [](float limit)
		{
			return std::isfinite(limit) ? std::abs(limit) : 0.0f;
		};
		if (!std::isfinite(pitch))
			pitch = 0.0f;
		if (!std::isfinite(yaw))
			yaw = 0.0f;
		pitch = std::clamp(pitch, -Limit(chain.PitchLimit), Limit(chain.PitchLimit));
		yaw = std::clamp(yaw, -Limit(chain.YawLimit), Limit(chain.YawLimit));
		if (pitch == 0.0f && yaw == 0.0f)
			return;

		float weightSum = 0.0f;
		float weights[AimOffsetChain::MaxJoints]{};
		const int count = std::min(chain.Count, AimOffsetChain::MaxJoints);
		for (int i = 0; i < count; i++)
		{
			// A negative weight would bend the other way. Shares are non-negative;
			// normalisation is what makes them not have to sum to 1.
			weights[i] = chain.Weights[i] > 0.0f ? chain.Weights[i] : 0.0f;
			weightSum += weights[i];
		}
		if (weightSum <= 1e-8f)
			return;
		for (int i = 0; i < count; i++)
			weights[i] /= weightSum;

		if (glm::length(upSkin) < 1e-8f || glm::length(rightSkin) < 1e-8f)
			return;
		const glm::vec3 up = glm::normalize(upSkin);
		const glm::vec3 right = glm::normalize(rightSkin);
		// Full yaw, not this joint's share. Every joint pitches about the aimed right.
		const glm::vec3 pitchAxis = glm::normalize(glm::angleAxis(yaw, up) * right);

		const uint32_t jointCount = skeleton.JointCount();
		for (int i = 0; i < count; i++)
		{
			const int32_t joint = chain.Joints[i];
			if (joint < 0 || (uint32_t)joint >= jointCount || (uint32_t)joint >= globals.size())
				continue;
			if (!chain.Subtrees[i] || chain.SubtreeCounts[i] == 0)
				continue;

			const float yawJ = weights[i] * yaw;
			const float pitchJ = weights[i] * pitch;
			const glm::mat4 rotation = glm::rotate(glm::mat4(1.0f), pitchJ, pitchAxis)
				* glm::rotate(glm::mat4(1.0f), yawJ, up);

			const glm::vec3 pivot = glm::vec3(globals[(uint32_t)joint][3]);
			const glm::mat4 aboutPivot = glm::translate(glm::mat4(1.0f), pivot)
				* rotation
				* glm::translate(glm::mat4(1.0f), -pivot);

			for (uint32_t n = 0; n < chain.SubtreeCounts[i]; n++)
			{
				const uint32_t k = chain.Subtrees[i][n];
				if (k < globals.size())
					globals[k] = aboutPivot * globals[k];
			}
		}
	}

	void AnimationSystem::ApplyAim(entt::entity entity, AimOffsetComponent& aim, const Mesh& mesh)
	{
		const Skeleton& skeleton = mesh.GetSkeleton();
		const std::vector<std::string>& names = skeleton.JointNames;

		bool unresolved = false;
		AimOffsetChain chain;
		chain.PitchLimit = aim.PitchLimit;
		chain.YawLimit = aim.YawLimit;

		for (int i = 0; i < AimOffsetComponent::MaxJoints; i++)
		{
			if (aim.Joints[i].empty())
			{
				for (int j = i; j < AimOffsetComponent::MaxJoints; j++)
					aim.Resolved[j] = -1;
				break;
			}

			const int32_t index = ResolveJointIndex(names, aim.Joints[i], aim.Resolved[i]);
			aim.Resolved[i] = index;
			if (index < 0)
			{
				unresolved = true;
				auto& warned = m_WarnedAimJoints[entity];
				if (warned.insert(aim.Joints[i]).second)
				{
					Entity wrapped{ entity, &m_Scene };
					GE_CORE_WARN("Aim offset on '{0}' references joint '{1}', which mesh '{2}' "
						"does not have - leaving the clip unbent",
						wrapped.GetName(), aim.Joints[i], mesh.GetPath());
				}
				continue;
			}

			auto warned = m_WarnedAimJoints.find(entity);
			if (warned != m_WarnedAimJoints.end())
				warned->second.erase(aim.Joints[i]);

			if (chain.Count < AimOffsetChain::MaxJoints)
			{
				chain.Joints[chain.Count] = index;
				chain.Weights[chain.Count] = aim.Weights[i];
				chain.Count++;
			}
		}

		// Any miss skips the whole chain. A half-applied spine is a worse pose than the clip.
		if (!aim.Enabled || unresolved || chain.Count == 0)
			return;

		glm::vec3 upSkin, rightSkin;
		if (!AimAxesInSkinSpace(mesh.GetSkinTransform(), aim.ModelForward, upSkin, rightSkin))
		{
			if (m_WarnedAimAxes.insert(entity).second)
			{
				Entity wrapped{ entity, &m_Scene };
				GE_CORE_WARN("Aim offset on '{0}' cannot read a rotation out of mesh '{1}' "
					"skin transform - leaving the clip unbent",
					wrapped.GetName(), mesh.GetPath());
			}
			return;
		}
		m_WarnedAimAxes.erase(entity);

		AimSubtreeCache& cache = m_AimSubtrees[entity];
		bool dirty = cache.Parents != skeleton.ParentIndices.data()
			|| cache.JointCount != skeleton.JointCount()
			|| cache.Count != chain.Count;
		if (!dirty)
		{
			for (int i = 0; i < chain.Count; i++)
			{
				if (cache.Joints[(size_t)i] != chain.Joints[i])
					dirty = true;
			}
		}
		if (dirty)
		{
			cache.Parents = skeleton.ParentIndices.data();
			cache.JointCount = skeleton.JointCount();
			cache.Count = chain.Count;
			for (int i = 0; i < AimOffsetChain::MaxJoints; i++)
			{
				cache.Joints[(size_t)i] = i < chain.Count ? chain.Joints[i] : -1;
				if (i < chain.Count)
					BuildSubtree(skeleton, chain.Joints[i], cache.Subtrees[(size_t)i]);
				else
					cache.Subtrees[(size_t)i].clear();
			}
		}

		for (int i = 0; i < chain.Count; i++)
		{
			chain.Subtrees[i] = cache.Subtrees[(size_t)i].data();
			chain.SubtreeCounts[i] = (uint32_t)cache.Subtrees[(size_t)i].size();
		}

		ApplyAimOffset(skeleton, chain, upSkin, rightSkin, aim.Pitch, aim.Yaw, m_Globals);
	}
}

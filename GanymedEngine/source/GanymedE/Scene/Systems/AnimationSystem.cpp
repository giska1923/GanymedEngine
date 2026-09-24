#include "gepch.h"
#include "AnimationSystem.h"

#include "GanymedE/Assets/AssetManager.h"
#include "GanymedE/Renderer/Mesh.h"
#include "GanymedE/Scene/Entity.h"
#include "GanymedE/Scene/Scene.h"

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

		// The one clamp rule for aim angles. ApplyAimOffset bends the spine with it and the aim
		// lock points the barrel with it, so the spine and the weapon cannot disagree on a limit.
		float ClampAimAngle(float angle, float limit)
		{
			const float bound = std::isfinite(limit) ? std::abs(limit) : 0.0f;
			return std::isfinite(angle) ? std::clamp(angle, -bound, bound) : 0.0f;
		}

		// The direction the aim offset aims, in mesh space: ModelForward turned by yaw about up,
		// then by pitch about the right turned by that yaw - the construction ApplyAimOffset
		// splits across the spine. Up is +Y; right is forward x up.
		glm::vec3 AimDirectionInMesh(const AimOffsetComponent& aim)
		{
			const float pitch = ClampAimAngle(aim.Pitch, aim.PitchLimit);
			const float yaw = ClampAimAngle(aim.Yaw, aim.YawLimit);
			const glm::vec3 up{ 0.0f, 1.0f, 0.0f };
			const glm::vec3 forward = ModelForwardVector(aim.ModelForward);
			const glm::vec3 pitchAxis = glm::angleAxis(yaw, up) * glm::cross(forward, up);
			return glm::normalize(glm::angleAxis(pitch, pitchAxis) * (glm::angleAxis(yaw, up) * forward));
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

		glm::vec3 Origin(const glm::mat4& global)
		{
			return glm::vec3(global[3]);
		}

		// A joint's orientation with its scale stripped - a centimetre rig carries 100 in
		// its globals' linear part, and the IK target is a unit orientation.
		glm::quat JointRotation(const glm::mat4& global)
		{
			return glm::normalize(glm::quat_cast(OrthonormalRotation(glm::mat3(global))));
		}

		bool IsFinite(const glm::vec3& v)
		{
			return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
		}

		glm::vec3 AnyPerpendicular(const glm::vec3& unit)
		{
			const glm::vec3 other = std::abs(unit.x) < 0.9f ? glm::vec3(1.0f, 0.0f, 0.0f) : glm::vec3(0.0f, 1.0f, 0.0f);
			return glm::normalize(glm::cross(unit, other));
		}

		// Shortest-arc rotation taking unit `from` onto unit `to`. Shortest arc is the point:
		// it swings a bone without adding twist about the bone's own axis.
		glm::quat RotationBetween(const glm::vec3& from, const glm::vec3& to)
		{
			const float cosine = glm::dot(from, to);
			if (cosine < -1.0f + 1e-6f)
				return glm::angleAxis(glm::pi<float>(), AnyPerpendicular(from));

			// Half-angle form: (1 + cos, from × to) normalises to the rotation with no trig.
			const glm::vec3 axis = glm::cross(from, to);
			return glm::normalize(glm::quat(1.0f + cosine, axis.x, axis.y, axis.z));
		}

		bool SubtreeContains(const uint32_t* subtree, uint32_t count, int32_t joint)
		{
			for (uint32_t n = 0; n < count; n++)
			{
				if ((int32_t)subtree[n] == joint)
					return true;
			}
			return false;
		}

		void RotateSubtree(std::vector<glm::mat4>& globals, const uint32_t* subtree, uint32_t count,
			const glm::vec3& pivot, const glm::quat& rotation)
		{
			const glm::mat4 aboutPivot = glm::translate(glm::mat4(1.0f), pivot)
				* glm::mat4_cast(rotation)
				* glm::translate(glm::mat4(1.0f), -pivot);

			for (uint32_t n = 0; n < count; n++)
			{
				const uint32_t k = subtree[n];
				if (k < globals.size())
					globals[k] = aboutPivot * globals[k];
			}
		}

		// Rewritten on every evaluation, so they describe this frame's pose or nothing. That is
		// what lets Scene::Copy and undo carry them without a sweep of their own.
		void ClearHandIKResults(TwoHandIKComponent& ik)
		{
			ik.Status.fill(TwoHandIKComponent::HandStatus::NotEvaluated);
			ik.Reached.fill(false);
			ik.Stretch.fill(0.0f);
			ik.Weapon = UUID{ 0 };
			ik.Markers.fill(UUID{ 0 });
			ik.AimLockState = TwoHandIKComponent::AimLockStatus::Off;
			ik.AimLockAngle = 0.0f;
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
		// magnitude, the clamp, and that a twisted pitch rotates about the yawed right -
		// on one joint, and across two, where per-joint shares used to miss the target.
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

			// Yaw and pitch together across more than one joint - the case a one-joint chain
			// cannot catch, because the per-joint shares only disagree when they interleave.
			// The neck must end on the same target as the one-joint case above.
			{
				std::vector<glm::mat4> posed = rest;
				AimOffsetChain chain = ProbeChain(skeleton, { 1, 2 }, { 0.25f, 0.75f }, storage);
				ApplyAimOffset(skeleton, chain, up, right, 0.3f, 1.0f, posed);

				const glm::vec3 pitchAxis = glm::angleAxis(1.0f, up) * right;
				const glm::quat expected = glm::normalize(
					glm::angleAxis(0.3f, pitchAxis) * glm::angleAxis(1.0f, up));
				const glm::quat actual = glm::normalize(
					glm::inverse(glm::quat_cast(rest[(size_t)neck])) * glm::quat_cast(posed[(size_t)neck]));
				const float d = glm::clamp(std::abs(glm::dot(expected, actual)), 0.0f, 1.0f);
				const float error = 2.0f * std::acos(d);
				if (error > 1e-4f)
				{
					FailProbe("two-joint yaw+pitch misses the target by " + std::to_string(error) + " rad");
					return;
				}
			}
		}

		void FailTwoBoneProbe(float unit, const std::string& message)
		{
			GE_CORE_ERROR("Two-bone IK probe failed (unit {0}): {1}", unit, message);
			GE_CORE_ASSERT(false, "Two-bone IK probe failed");
		}

		// Angle of a⁻¹b from its vector part. The 2 acos(|dot|) form the aim probes use is
		// quantised near zero - one float step below 1 is already 7e-4 rad - so it cannot
		// resolve a 1e-4 tolerance.
		float RotationError(const glm::quat& a, const glm::quat& b)
		{
			const glm::quat delta = glm::normalize(glm::inverse(a) * b);
			return 2.0f * std::atan2(glm::length(glm::vec3(delta.x, delta.y, delta.z)), std::abs(delta.w));
		}

		// Chest, a bent arm hanging off it, a hand tip below the wrist that must follow the hand,
		// and a head that must not move. `unit` scales RootTransform: 1 is a metre rig, 100 a
		// centimetre rig whose globals carry the scale in their linear part, which the rotation
		// read has to strip.
		Skeleton MakeArmProbeSkeleton(float unit)
		{
			Skeleton skeleton;
			skeleton.ParentIndices = { -1, 0, 1, 2, 3, 0 };
			skeleton.JointNames = { "Chest", "Arm", "ForeArm", "Hand", "HandTip", "Head" };
			skeleton.InverseBind.assign(skeleton.ParentIndices.size(), glm::mat4(1.0f));
			skeleton.LocalRestPose.resize(skeleton.ParentIndices.size());
			skeleton.LocalRestPose[0].Translation = { 0.0f, 1.4f, 0.0f };
			skeleton.LocalRestPose[1].Translation = { 0.2f, 0.0f, 0.0f };
			skeleton.LocalRestPose[2].Translation = { 0.3f, 0.0f, 0.0f };
			skeleton.LocalRestPose[3].Translation = { 0.0f, -0.25f, 0.0f };
			skeleton.LocalRestPose[3].Rotation = glm::angleAxis(0.4f, glm::normalize(glm::vec3(1.0f, 1.0f, 0.0f)));
			skeleton.LocalRestPose[4].Translation = { 0.0f, -0.08f, 0.0f };
			skeleton.LocalRestPose[5].Translation = { 0.0f, 0.3f, 0.0f };
			skeleton.RootTransform = glm::scale(glm::mat4(1.0f), glm::vec3(unit));
			return skeleton;
		}

		// H1's verification table: weight 0, a reachable target with its rotation, a target past
		// reach, the pole's side, a degenerate pole, and a partial weight - run on a metre rig
		// and a centimetre rig, every tolerance scaled by the unit.
		void RunTwoBoneProbes()
		{
			for (const float unit : { 1.0f, 100.0f })
			{
				const Skeleton skeleton = MakeArmProbeSkeleton(unit);
				std::vector<JointPose> locals;
				std::vector<glm::mat4> rest;
				if (!SampleClipGlobals(skeleton, nullptr, 0.0f, locals, rest))
				{
					FailTwoBoneProbe(unit, "probe skeleton did not sample");
					return;
				}

				const int32_t chest = 0, arm = 1, foreArm = 2, hand = 3, handTip = 4, head = 5;
				std::array<std::vector<uint32_t>, 3> subtrees;
				BuildSubtree(skeleton, arm, subtrees[0]);
				BuildSubtree(skeleton, foreArm, subtrees[1]);
				BuildSubtree(skeleton, hand, subtrees[2]);

				TwoBoneChain chain;
				chain.Upper = arm;
				chain.Lower = foreArm;
				chain.End = hand;
				chain.PoleHint = { 0.0f, -1.0f, 0.0f };
				for (int i = 0; i < 3; i++)
				{
					chain.Subtrees[i] = subtrees[(size_t)i].data();
					chain.SubtreeCounts[i] = (uint32_t)subtrees[(size_t)i].size();
				}

				const glm::vec3 shoulder = Origin(rest[arm]);
				const float upperLength = 0.3f * unit;
				const float lowerLength = 0.25f * unit;
				const float tipLength = 0.08f * unit;
				const glm::vec3 clipElbow = Origin(rest[foreArm]);
				const glm::quat someRotation = glm::angleAxis(1.1f, glm::normalize(glm::vec3(0.3f, 1.0f, -0.5f)));

				const auto P = [&](int32_t joint, const std::vector<glm::mat4>& globals) { return Origin(globals[(size_t)joint]); };
				const auto AllFinite = [](const std::vector<glm::mat4>& globals)
				{
					for (const glm::mat4& m : globals)
					{
						for (int c = 0; c < 4; c++)
						{
							if (!IsFinite(glm::vec3(m[c])) || !std::isfinite(m[c][3]))
								return false;
						}
					}
					return true;
				};
				const auto LengthsKept = [&](const std::vector<glm::mat4>& globals)
				{
					return std::abs(glm::length(P(foreArm, globals) - P(arm, globals)) - upperLength) < 1e-5f * unit
						&& std::abs(glm::length(P(hand, globals) - P(foreArm, globals)) - lowerLength) < 1e-5f * unit
						&& std::abs(glm::length(P(handTip, globals) - P(hand, globals)) - tipLength) < 1e-5f * unit;
				};
				const auto BendAngle = [&](const std::vector<glm::mat4>& globals)
				{
					const glm::vec3 upper = glm::normalize(P(foreArm, globals) - P(arm, globals));
					const glm::vec3 lower = glm::normalize(P(hand, globals) - P(foreArm, globals));
					return std::atan2(glm::length(glm::cross(upper, lower)), glm::dot(upper, lower));
				};

				{
					std::vector<glm::mat4> posed = rest;
					const TwoBoneResult r = SolveTwoBone(skeleton, chain, shoulder + glm::vec3(0.15f, -0.35f, 0.2f) * unit,
						someRotation, clipElbow, 0.0f, posed);
					if (!r.Valid)
					{
						FailTwoBoneProbe(unit, "weight 0 on a valid chain reported invalid");
						return;
					}
					for (size_t i = 0; i < rest.size(); i++)
					{
						if (!SameMatrix(rest[i], posed[i]))
						{
							FailTwoBoneProbe(unit, "weight 0 changed a global");
							return;
						}
					}
				}

				{
					std::vector<glm::mat4> posed = rest;
					const glm::vec3 target = shoulder + glm::vec3(0.15f, -0.35f, 0.2f) * unit;
					const TwoBoneResult r = SolveTwoBone(skeleton, chain, target, someRotation, clipElbow, 1.0f, posed);
					const float miss = glm::length(P(hand, posed) - target);
					if (!r.Valid || !r.Reached || miss > 1e-4f * unit)
					{
						FailTwoBoneProbe(unit, "reachable target missed by " + std::to_string(miss / unit) + " m");
						return;
					}
					if (!LengthsKept(posed))
					{
						FailTwoBoneProbe(unit, "a reachable solve changed a bone length");
						return;
					}
					if (!SameMatrix(rest[chest], posed[chest]) || !SameMatrix(rest[head], posed[head])
						|| glm::length(P(arm, posed) - shoulder) > 1e-6f * unit)
					{
						FailTwoBoneProbe(unit, "the solve moved the shoulder or a joint outside the arm");
						return;
					}
					const float rotationError = RotationError(someRotation, JointRotation(posed[hand]));
					if (rotationError > 1e-4f)
					{
						FailTwoBoneProbe(unit, "wrist rotation misses the target by " + std::to_string(rotationError) + " rad");
						return;
					}
				}

				{
					std::vector<glm::mat4> posed = rest;
					const glm::vec3 direction = glm::normalize(glm::vec3(0.3f, -0.2f, 0.9f));
					const TwoBoneResult r = SolveTwoBone(skeleton, chain, shoulder + direction * unit,
						someRotation, clipElbow, 1.0f, posed);
					const glm::vec3 fullLength = shoulder + direction * (upperLength + lowerLength);
					const float offLine = glm::length(P(hand, posed) - fullLength);
					const float bend = BendAngle(posed);
					if (!r.Valid || r.Reached || std::abs(r.Stretch - 1.0f / 0.55f) > 1e-4f)
					{
						FailTwoBoneProbe(unit, "target past reach: Reached " + std::to_string(r.Reached)
							+ ", Stretch " + std::to_string(r.Stretch) + ", expected " + std::to_string(1.0f / 0.55f));
						return;
					}
					if (offLine > 1e-4f * unit || bend > glm::radians(1.0f))
					{
						FailTwoBoneProbe(unit, "past reach: wrist " + std::to_string(offLine / unit)
							+ " m off the full-length point, elbow bent " + std::to_string(glm::degrees(bend)) + " deg");
						return;
					}
				}

				{
					const glm::vec3 target = shoulder + glm::vec3(0.0f, -0.1f, 0.4f) * unit;
					std::vector<glm::mat4> left = rest;
					std::vector<glm::mat4> right = rest;
					const TwoBoneResult rl = SolveTwoBone(skeleton, chain, target, someRotation,
						shoulder + glm::vec3(0.3f, 0.0f, 0.2f) * unit, 1.0f, left);
					const TwoBoneResult rr = SolveTwoBone(skeleton, chain, target, someRotation,
						shoulder + glm::vec3(-0.3f, 0.0f, 0.2f) * unit, 1.0f, right);
					const float leftSide = (P(foreArm, left) - shoulder).x;
					const float rightSide = (P(foreArm, right) - shoulder).x;
					if (!rl.Reached || !rr.Reached || !(leftSide > 0.0f) || !(rightSide < 0.0f))
					{
						FailTwoBoneProbe(unit, "elbow did not follow the pole: +X pole put it at x "
							+ std::to_string(leftSide / unit) + ", -X pole at " + std::to_string(rightSide / unit));
						return;
					}
					const float angleDifference = std::abs(BendAngle(left) - BendAngle(right));
					if (angleDifference > 1e-4f)
					{
						FailTwoBoneProbe(unit, "the pole changed the elbow angle by " + std::to_string(angleDifference) + " rad");
						return;
					}
				}

				{
					// On the reach line in front of the target, and on the shoulder itself.
					const glm::vec3 target = shoulder + glm::vec3(0.0f, 0.0f, 0.4f) * unit;
					for (const glm::vec3& pole : { shoulder + glm::vec3(0.0f, 0.0f, 1.0f) * unit, shoulder })
					{
						std::vector<glm::mat4> posed = rest;
						const TwoBoneResult r = SolveTwoBone(skeleton, chain, target, someRotation, pole, 1.0f, posed);
						if (!AllFinite(posed))
						{
							FailTwoBoneProbe(unit, "a pole on the reach line produced a non-finite global");
							return;
						}
						const float miss = glm::length(P(hand, posed) - target);
						const float drop = (P(foreArm, posed) - shoulder).y;
						if (!r.Reached || miss > 1e-4f * unit || !(drop < 0.0f))
						{
							FailTwoBoneProbe(unit, "degenerate pole: wrist missed by " + std::to_string(miss / unit)
								+ " m, elbow height " + std::to_string(drop / unit) + " (hint is down)");
							return;
						}
					}
				}

				{
					std::vector<glm::mat4> posed = rest;
					const glm::vec3 target = shoulder + glm::vec3(0.15f, -0.35f, 0.2f) * unit;
					SolveTwoBone(skeleton, chain, target, someRotation, clipElbow, 0.5f, posed);
					const glm::vec3 halfway = glm::mix(P(hand, rest), target, 0.5f);
					const float miss = glm::length(P(hand, posed) - halfway);
					if (miss > 1e-4f * unit)
					{
						FailTwoBoneProbe(unit, "weight 0.5 missed the halfway goal by " + std::to_string(miss / unit) + " m");
						return;
					}
				}

				{
					// ForeArm is not under Hand: not one limb, refused, untouched.
					TwoBoneChain wrong = chain;
					wrong.Upper = hand;
					wrong.Subtrees[0] = subtrees[2].data();
					wrong.SubtreeCounts[0] = (uint32_t)subtrees[2].size();
					std::vector<glm::mat4> posed = rest;
					const TwoBoneResult r = SolveTwoBone(skeleton, wrong, shoulder, someRotation, clipElbow, 1.0f, posed);
					bool untouched = true;
					for (size_t i = 0; i < rest.size(); i++)
						untouched = untouched && SameMatrix(rest[i], posed[i]);
					if (r.Valid || !untouched)
					{
						FailTwoBoneProbe(unit, "a chain that is not one limb was solved");
						return;
					}
				}
			}
		}
#endif

	}

	void AnimationSystem::OnRuntimeStart()
	{
		// Play starts at the head of the clip whatever the editor was scrubbed to. The runtime
		// scene is a copy, so the editor scene keeps its scrub position for when play stops.
		for (auto [entity, animator, meshComponent, aim, ik] : View<AnimView>())
		{
			(void)entity;
			(void)meshComponent;
			(void)aim;
			(void)ik;
			animator.Time = 0.0f;
		}

		m_WarnedClips.clear();
		m_WarnedAimJoints.clear();
		m_WarnedAimAxes.clear();
		m_AimSubtrees.clear();
		m_WarnedHandIK.clear();
		m_HandIKSubtrees.clear();
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
			RunTwoBoneProbes();
		}
#endif

		for (auto [entity, animator, meshComponent, aim, ik] : View<AnimView>())
		{
			if (ik)
				ClearHandIKResults(*ik);

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

			// After the palette, not before: the weapon frame is read from it through
			// TryGetJointFrame - the same function and the same entry BoneAttachmentSystem
			// draws the weapon from later this frame. The pass then rewrites only the arms.
			if (ik)
				ApplyTwoHandIK(entity, *ik, aim ? &*aim : nullptr, *mesh, animator.Palette);
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

		pitch = ClampAimAngle(pitch, chain.PitchLimit);
		yaw = ClampAimAngle(yaw, chain.YawLimit);
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
		// Full yaw, not this joint's share. The chain pitches about the aimed right.
		const glm::vec3 pitchAxis = glm::normalize(glm::angleAxis(yaw, up) * right);

		// The whole chain's target, then split as one axis-angle. Rotations about a shared
		// axis commute, so the weighted shares compose to exactly this target in any order.
		// Giving each joint its own "yaw share, then pitch share" does not: across joints the
		// two interleave, and with three joints at 1/6, 1/3, 1/2 the chest missed the aim by
		// 7.8 degrees at a 90-degree twist and 0.3 pitch, and by 26 at both limits.
		glm::quat target = glm::normalize(glm::angleAxis(pitch, pitchAxis) * glm::angleAxis(yaw, up));
		if (target.w < 0.0f)
			target = -target; // the short way round, so the angle below is in [0, pi]
		const float angle = glm::angle(target);
		if (!(angle > 1e-7f))
			return;
		const glm::vec3 axis = glm::axis(target);

		const uint32_t jointCount = skeleton.JointCount();
		for (int i = 0; i < count; i++)
		{
			const int32_t joint = chain.Joints[i];
			if (joint < 0 || (uint32_t)joint >= jointCount || (uint32_t)joint >= globals.size())
				continue;
			if (!chain.Subtrees[i] || chain.SubtreeCounts[i] == 0)
				continue;

			const glm::mat4 rotation = glm::mat4_cast(glm::angleAxis(weights[i] * angle, axis));

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

	TwoBoneResult SolveTwoBone(const Skeleton& skeleton, const TwoBoneChain& chain,
		const glm::vec3& target, const glm::quat& targetRotation, const glm::vec3& pole,
		float weight, std::vector<glm::mat4>& globals)
	{
		GE_PROFILE_SCOPE("AnimationSystem::SolveTwoBone");

		TwoBoneResult result;

		const uint32_t jointCount = skeleton.JointCount();
		if (globals.size() != jointCount)
			return result;

		const auto InRange = [jointCount](int32_t joint)
		{
			return joint >= 0 && (uint32_t)joint < jointCount;
		};
		if (!InRange(chain.Upper) || !InRange(chain.Lower) || !InRange(chain.End))
			return result;
		for (int i = 0; i < 3; i++)
		{
			if (!chain.Subtrees[i] || chain.SubtreeCounts[i] == 0)
				return result;
		}

		// Turning Upper's subtree has to carry the elbow and the wrist, and Lower's the wrist.
		// A chain that is not one limb would move the wrist by rotations that do not reach it.
		if (!SubtreeContains(chain.Subtrees[0], chain.SubtreeCounts[0], chain.Lower)
			|| !SubtreeContains(chain.Subtrees[1], chain.SubtreeCounts[1], chain.End))
		{
			return result;
		}

		if (!IsFinite(target) || !IsFinite(pole)
			|| !std::isfinite(targetRotation.w) || !IsFinite(glm::vec3(targetRotation.x, targetRotation.y, targetRotation.z))
			|| glm::length(targetRotation) < 1e-6f)
		{
			return result;
		}

		const glm::vec3 shoulder = Origin(globals[(uint32_t)chain.Upper]);
		const glm::vec3 elbow = Origin(globals[(uint32_t)chain.Lower]);
		const glm::vec3 wrist = Origin(globals[(uint32_t)chain.End]);

		// Tolerances are fractions of the chain, never absolute: the same arm is ~0.55 in a
		// metre rig and ~55 in a centimetre one.
		const float upperLength = glm::length(elbow - shoulder);
		const float lowerLength = glm::length(wrist - elbow);
		const float chainLength = upperLength + lowerLength;
		if (!std::isfinite(chainLength) || !(chainLength > 0.0f)
			|| upperLength < 1e-4f * chainLength || lowerLength < 1e-4f * chainLength)
		{
			return result;
		}

		// Clamped a hair inside the reach so the elbow keeps a side and the law of cosines
		// stays off the ends of acos's domain. At 1e-5 of the chain a clamped arm is still
		// straight to about half a degree.
		const float epsilon = 1e-5f * chainLength;
		const float minReach = std::abs(upperLength - lowerLength) + epsilon;
		const float maxReach = chainLength - epsilon;

		const float targetDistance = glm::length(target - shoulder);
		result.Valid = true;
		result.Reached = targetDistance >= minReach && targetDistance <= maxReach;
		result.Stretch = targetDistance / chainLength;

		if (!std::isfinite(weight))
			weight = 0.0f;
		weight = std::clamp(weight, 0.0f, 1.0f);
		// The early-out is the bit-identical guarantee, not a solve that happens to land on
		// the clip's own wrist.
		if (weight <= 0.0f)
			return result;

		const glm::quat fullRotation = glm::normalize(targetRotation);
		const glm::vec3 goal = weight >= 1.0f ? target : glm::mix(wrist, target, weight);
		const glm::quat goalRotation = weight >= 1.0f ? fullRotation
			: glm::normalize(glm::slerp(JointRotation(globals[(uint32_t)chain.End]), fullRotation, weight));

		// The reach axis. A goal on the shoulder has none; keep the current wrist's, then the
		// upper bone's, which the length check above guarantees exists.
		glm::vec3 reach = goal - shoulder;
		float distance = glm::length(reach);
		if (distance > 1e-6f * chainLength)
			reach /= distance;
		else if (glm::length(wrist - shoulder) > 1e-6f * chainLength)
			reach = glm::normalize(wrist - shoulder);
		else
			reach = (elbow - shoulder) / upperLength;
		distance = std::clamp(distance, minReach, maxReach);

		// The bend plane holds shoulder, goal and pole. When the pole is within ~3 degrees of
		// the reach line (sin 3° = 0.0523), or behind it on the line, the plane is noise and
		// the elbow would flip frame to frame; the chain's hint takes over.
		const auto Perpendicular = [&reach](const glm::vec3& v)
		{
			return v - reach * glm::dot(v, reach);
		};
		glm::vec3 bend = Perpendicular(pole - shoulder);
		if (!(glm::length(bend) > 0.0523f * glm::length(pole - shoulder)))
		{
			bend = Perpendicular(chain.PoleHint);
			if (!IsFinite(bend) || !(glm::length(bend) > 1e-3f * glm::length(chain.PoleHint)))
				bend = AnyPerpendicular(reach);
		}
		bend = glm::normalize(bend);

		// Law of cosines for the angle at the shoulder between the reach line and the upper bone.
		const float cosShoulder = std::clamp(
			(upperLength * upperLength + distance * distance - lowerLength * lowerLength)
				/ (2.0f * upperLength * distance),
			-1.0f, 1.0f);
		const float sinShoulder = std::sqrt(std::max(0.0f, 1.0f - cosShoulder * cosShoulder));
		const glm::vec3 elbowGoal = shoulder + upperLength * (cosShoulder * reach + sinShoulder * bend);
		const glm::vec3 wristGoal = shoulder + distance * reach;

		// Swing the upper arm so the elbow lands, then the forearm about the elbow it now has so
		// the wrist lands, then turn the hand in place. Positions are re-read between steps
		// rather than trusted from the plan, so float error does not accumulate across them.
		RotateSubtree(globals, chain.Subtrees[0], chain.SubtreeCounts[0], shoulder,
			RotationBetween((elbow - shoulder) / upperLength, glm::normalize(elbowGoal - shoulder)));

		const glm::vec3 elbowNow = Origin(globals[(uint32_t)chain.Lower]);
		const glm::vec3 wristNow = Origin(globals[(uint32_t)chain.End]);
		RotateSubtree(globals, chain.Subtrees[1], chain.SubtreeCounts[1], elbowNow,
			RotationBetween(glm::normalize(wristNow - elbowNow), glm::normalize(wristGoal - elbowNow)));

		const glm::quat handNow = JointRotation(globals[(uint32_t)chain.End]);
		RotateSubtree(globals, chain.Subtrees[2], chain.SubtreeCounts[2],
			Origin(globals[(uint32_t)chain.End]), glm::normalize(goalRotation * glm::inverse(handNow)));

		return result;
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

		// Compared by content, not by the vector's address: a hot-reloaded mesh can be
		// allocated where the old one was, with a different topology, and an address check
		// would keep bending the old subtrees. A spine's worth of ints a frame is nothing.
		AimSubtreeCache& cache = m_AimSubtrees[entity];
		bool dirty = cache.Parents != skeleton.ParentIndices
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
			cache.Parents = skeleton.ParentIndices;
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

	void AnimationSystem::WarnHandIK(entt::entity entity, const std::string& message)
	{
		if (m_WarnedHandIK[entity].insert(message).second)
			GE_CORE_WARN("{0}", message);
	}

	void AnimationSystem::ApplyTwoHandIK(entt::entity entity, TwoHandIKComponent& ik,
		const AimOffsetComponent* aim, const Mesh& mesh, std::vector<glm::mat4>& palette)
	{
		GE_PROFILE_SCOPE("AnimationSystem::ApplyTwoHandIK");

		using HandStatus = TwoHandIKComponent::HandStatus;
		const auto Both = [&ik](HandStatus status) { ik.Status.fill(status); };

		// Weight 0 still resolves and measures - the solver reports reach without touching the
		// pose - so a readout works with the hands off. Enabled is the off switch.
		if (!ik.Enabled)
		{
			Both(HandStatus::Disabled);
			return;
		}
		const float weights[2] = { ik.RightWeight, ik.LeftWeight };

		const Skeleton& skeleton = mesh.GetSkeleton();
		const std::vector<std::string>& names = skeleton.JointNames;
		const Entity self{ entity, &m_Scene };
		// Built only on a failure path; a solved frame allocates no strings.
		const auto Where = [&self]() { return "Two-hand IK on '" + self.GetName() + "'"; };
		auto access = View<WeaponAccess>();

		// The weapon: the first child whose socket is aimed at this rig. Target zero means the
		// socket's parent, which for a child is this entity.
		Entity weapon;
		const BoneAttachmentComponent* socket = nullptr;
		if (auto relationship = access.FindOne<RelationshipComponent>(self))
		{
			for (UUID childID : relationship->Children)
			{
				Entity child = m_Scene.FindEntityByUUID(childID);
				if (!child)
					continue;

				auto attachment = access.FindOne<BoneAttachmentComponent>(child);
				if (!attachment || (attachment->Target != UUID{ 0 } && attachment->Target != self.GetUUID()))
					continue;

				weapon = child;
				socket = attachment.Get();
				break;
			}
		}
		if (!weapon || !socket)
		{
			Both(HandStatus::NoWeapon);
			WarnHandIK(entity, Where() + " finds no weapon: no child has a BoneAttachmentComponent "
				"on this rig - leaving the arms on the clip");
			return;
		}

		int32_t& anchor = ik.Resolved[6];
		anchor = ResolveJointIndex(names, socket->Joint, anchor);
		glm::mat4 anchorFrame{ 1.0f };
		ik.Weapon = weapon.GetUUID();
		if (anchor < 0 || !TryGetJointFrame(mesh, palette, anchor, anchorFrame))
		{
			Both(HandStatus::NoWeaponFrame);
			WarnHandIK(entity, Where() + ": weapon '" + weapon.GetName() + "' is socketed to joint '"
				+ socket->Joint + "', which mesh '" + mesh.GetPath() + "' does not resolve - "
				"leaving the arms on the clip");
			return;
		}

		// Mesh space, exactly as BoneAttachmentSystem builds it minus the rig's world matrix:
		// the anchor's frame from this palette, times the socket's offset at the weapon's scale.
		auto weaponTransform = access.FindOne<TransformComponent>(weapon);
		if (!weaponTransform)
		{
			Both(HandStatus::NoWeaponFrame);
			return;
		}
		glm::mat4 weaponFrame = anchorFrame * socket->OffsetMatrix(weaponTransform->Scale);

		// Markers are direct children of the weapon, found by name.
		const auto ChildNamed = [&](const std::string& name)
		{
			if (auto weaponRelationship = access.FindOne<RelationshipComponent>(weapon))
			{
				for (UUID childID : weaponRelationship->Children)
				{
					Entity child = m_Scene.FindEntityByUUID(childID);
					if (child && child.GetName() == name && access.FindOne<TransformComponent>(child))
						return child;
				}
			}
			return Entity{};
		};
		const Entity markers[2] = { ChildNamed(ik.RightMarker), ChildNamed(ik.LeftMarker) };

		// Aim lock, before the hands: they are solved onto the weapon where it will be drawn.
		// A look-at with up, not a shortest arc from the current barrel. The barrel goes onto the
		// aim, and the weapon's up (AimMarker's +Y) stays as near the character's up as that
		// allows, so a chest that rolls through a run does not cant the rifle. The cost is that
		// under a full lock the socket's authored rotation no longer matters; only where it puts
		// the pivot does. The pivot is the right-hand marker, so the lock turns the weapon in the
		// grip rather than swinging the grip away from the hand.
		const float lockWeight = std::isfinite(ik.AimLock) ? std::clamp(ik.AimLock, 0.0f, 1.0f) : 0.0f;
		bool lockClean = true; // a failed lock must keep its warning armed like a failed hand
		if (lockWeight > 0.0f)
		{
			const Entity aimMarker = ChildNamed(ik.AimMarker);
			if (!aim)
			{
				ik.AimLockState = TwoHandIKComponent::AimLockStatus::NoAimOffset;
				lockClean = false;
				WarnHandIK(entity, Where() + " has an aim lock but no AimOffsetComponent to aim with - "
					"leaving the weapon on its socket");
			}
			else if (!aimMarker)
			{
				ik.AimLockState = TwoHandIKComponent::AimLockStatus::NoAimMarker;
				lockClean = false;
				WarnHandIK(entity, Where() + ": weapon '" + weapon.GetName() + "' has no child named '"
					+ ik.AimMarker + "' to aim - leaving the weapon on its socket");
			}
			else
			{
				const glm::mat4 muzzleFrame = weaponFrame
					* access.FindOne<TransformComponent>(aimMarker)->GetLocalTransform();
				const glm::mat3 muzzleBasis = OrthonormalRotation(glm::mat3(muzzleFrame));
				const glm::vec3 aimDirection = AimDirectionInMesh(*aim);

				// The target basis: -Z on the aim, +Y as near mesh up as it can be, X completing a
				// right-handed frame. Aiming straight up or down has no such up; there the shortest
				// arc from the current barrel is the only rotation that means anything.
				glm::quat delta;
				const glm::vec3 z = -aimDirection;
				const glm::vec3 x = glm::cross(glm::vec3(0.0f, 1.0f, 0.0f), z);
				if (glm::length(x) > 1e-4f)
				{
					const glm::vec3 xn = glm::normalize(x);
					const glm::mat3 target(xn, glm::cross(z, xn), z);
					delta = glm::normalize(glm::quat_cast(target * glm::transpose(muzzleBasis)));
				}
				else
				{
					delta = RotationBetween(-muzzleBasis[2], aimDirection);
				}
				if (delta.w < 0.0f)
					delta = -delta; // the short way round, so the blend and the angle agree
				ik.AimLockAngle = 2.0f * std::atan2(glm::length(glm::vec3(delta.x, delta.y, delta.z)), delta.w);

				const glm::quat turn = lockWeight >= 1.0f ? delta
					: glm::normalize(glm::slerp(glm::quat(1.0f, 0.0f, 0.0f, 0.0f), delta, lockWeight));
				const glm::vec3 pivot = markers[0]
					? glm::vec3((weaponFrame * access.FindOne<TransformComponent>(markers[0])->GetLocalTransform())[3])
					: glm::vec3(weaponFrame[3]);
				weaponFrame = glm::translate(glm::mat4(1.0f), pivot) * glm::mat4_cast(turn)
					* glm::translate(glm::mat4(1.0f), -pivot) * weaponFrame;

				ik.LockedWeaponFrame = weaponFrame;
				ik.AimLockState = TwoHandIKComponent::AimLockStatus::Locked;
			}
		}

		// Markers are authored in mesh space (the joint frames TryGetJointFrame reports: unit
		// basis, metres); the solver works where the globals live. A position comes back through
		// inverse(skin); an orientation through its rotation part only. TryGetJointFrame divides
		// the bind scale back out, so a joint frame's basis is skin rotation × global rotation.
		const glm::mat4 skin = mesh.GetSkinTransform();
		const float skinDet = glm::determinant(skin);
		if (!std::isfinite(skinDet) || std::abs(skinDet) < 1e-20f)
		{
			Both(HandStatus::NoWeaponFrame);
			WarnHandIK(entity, Where() + " cannot invert mesh '" + mesh.GetPath() + "' skin transform - "
				"leaving the arms on the clip");
			return;
		}
		const glm::mat4 meshToSkin = glm::inverse(skin);
		const glm::mat3 meshToSkinRotation = glm::transpose(OrthonormalRotation(glm::mat3(skin)));
		const glm::vec3 upSkin = glm::normalize(meshToSkinRotation * glm::vec3(0.0f, 1.0f, 0.0f));

		const std::string* chainNames[2][3] = {
			{ &ik.RightUpper, &ik.RightLower, &ik.RightEnd },
			{ &ik.LeftUpper, &ik.LeftLower, &ik.LeftEnd } };
		const std::string* markerNames[2] = { &ik.RightMarker, &ik.LeftMarker };
		const char* handNames[2] = { "right", "left" };

		bool chainResolved[2] = { true, true };
		for (int hand = 0; hand < 2; hand++)
		{
			for (int slot = 0; slot < 3; slot++)
			{
				int32_t& index = ik.Resolved[(size_t)(hand * 3 + slot)];
				index = ResolveJointIndex(names, *chainNames[hand][slot], index);
				if (index < 0)
				{
					chainResolved[hand] = false;
					ik.Status[(size_t)hand] = HandStatus::NoJoint;
					WarnHandIK(entity, Where() + " references joint '" + *chainNames[hand][slot]
						+ "', which mesh '" + mesh.GetPath() + "' does not have - leaving the "
						+ handNames[hand] + " hand on the clip");
				}
			}
		}

		// Content-compared, like the aim cache: a hot-reloaded mesh can land at the old address.
		HandIKSubtreeCache& cache = m_HandIKSubtrees[entity];
		bool dirty = cache.Parents != skeleton.ParentIndices;
		for (size_t i = 0; i < cache.Joints.size(); i++)
			dirty = dirty || cache.Joints[i] != ik.Resolved[i];
		if (dirty)
		{
			cache.Parents = skeleton.ParentIndices;
			for (size_t i = 0; i < cache.Joints.size(); i++)
			{
				cache.Joints[i] = ik.Resolved[i];
				BuildSubtree(skeleton, cache.Joints[i], cache.Subtrees[i]);
			}
		}

		bool clean = chainResolved[0] && chainResolved[1] && lockClean;
		for (int hand = 0; hand < 2; hand++)
		{
			if (!chainResolved[hand])
				continue;

			const std::vector<uint32_t>& armSubtree = cache.Subtrees[(size_t)(hand * 3)];

			// A weapon socketed inside this arm (the old RightHand setup) would move with the
			// solve that is meant to reach it. That hand carries the weapon; the other still solves.
			if (SubtreeContains(armSubtree.data(), (uint32_t)armSubtree.size(), anchor))
			{
				clean = false;
				ik.Status[(size_t)hand] = HandStatus::WeaponInArm;
				WarnHandIK(entity, Where() + ": weapon '" + weapon.GetName() + "' is socketed to '"
					+ socket->Joint + "', inside the " + handNames[hand] + " arm - that hand carries "
					"the weapon and is not solved");
				continue;
			}

			const Entity marker = markers[hand];
			if (!marker)
			{
				clean = false;
				ik.Status[(size_t)hand] = HandStatus::NoMarker;
				WarnHandIK(entity, Where() + ": weapon '" + weapon.GetName() + "' has no child named '"
					+ *markerNames[hand] + "' - leaving the " + handNames[hand] + " hand on the clip");
				continue;
			}
			ik.Markers[(size_t)hand] = marker.GetUUID();
			auto markerTransform = access.FindOne<TransformComponent>(marker);

			const glm::mat4 markerFrame = weaponFrame * markerTransform->GetLocalTransform();
			const glm::vec3 target = glm::vec3(meshToSkin * glm::vec4(glm::vec3(markerFrame[3]), 1.0f));
			const glm::quat targetRotation = glm::normalize(glm::quat_cast(
				meshToSkinRotation * OrthonormalRotation(glm::mat3(markerFrame))));

			TwoBoneChain chain;
			chain.Upper = ik.Resolved[(size_t)(hand * 3 + 0)];
			chain.Lower = ik.Resolved[(size_t)(hand * 3 + 1)];
			chain.End = ik.Resolved[(size_t)(hand * 3 + 2)];
			for (int slot = 0; slot < 3; slot++)
			{
				const std::vector<uint32_t>& subtree = cache.Subtrees[(size_t)(hand * 3 + slot)];
				chain.Subtrees[slot] = subtree.data();
				chain.SubtreeCounts[slot] = (uint32_t)subtree.size();
			}

			// Only used when the clip's elbow sits on the reach line. Down and out: away from
			// the weapon's joint, which is on the spine, with the vertical taken out.
			const glm::vec3 shoulder = Origin(m_Globals[(size_t)chain.Upper]);
			glm::vec3 outward = shoulder - Origin(m_Globals[(size_t)anchor]);
			const float outwardLength = glm::length(outward);
			outward -= upSkin * glm::dot(outward, upSkin);
			chain.PoleHint = glm::length(outward) > 1e-3f * outwardLength
				? glm::normalize(glm::normalize(outward) - upSkin) : -upSkin;

			// The clip's own elbow is the pole, so the arm keeps the clip's style.
			const glm::vec3 pole = Origin(m_Globals[(size_t)chain.Lower]);

			const TwoBoneResult result = SolveTwoBone(skeleton, chain, target, targetRotation, pole,
				weights[hand], m_Globals);
			ik.Status[(size_t)hand] = result.Valid ? HandStatus::Solved : HandStatus::Unsolvable;
			ik.Reached[(size_t)hand] = result.Reached;
			ik.Stretch[(size_t)hand] = result.Stretch;
			if (!result.Valid)
			{
				clean = false;
				WarnHandIK(entity, Where() + " cannot solve the " + std::string(handNames[hand]) + " arm '"
					+ *chainNames[hand][0] + "/" + *chainNames[hand][1] + "/" + *chainNames[hand][2]
					+ "': not one limb, or a zero-length bone - leaving it on the clip");
				continue;
			}

			// The same expression as Evaluate's palette loop, so an entry is bit-identical to
			// what a full rebuild after the solve would write. The anchor is outside this arm
			// (checked above), so the entry the weapon frame came from is not touched. Weight 0
			// moved nothing, and rewriting nothing keeps the palette bit-identical.
			if (!(std::isfinite(weights[hand]) && weights[hand] > 0.0f))
				continue;
			for (uint32_t k : armSubtree)
				palette[k] = m_Globals[k] * skeleton.InverseBind[k];
		}

		if (clean)
			m_WarnedHandIK.erase(entity);
	}
}

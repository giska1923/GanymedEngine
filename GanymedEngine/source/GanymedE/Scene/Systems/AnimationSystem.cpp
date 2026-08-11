#include "gepch.h"
#include "AnimationSystem.h"

#include "GanymedE/Assets/AssetManager.h"
#include "GanymedE/Renderer/Mesh.h"
#include "GanymedE/Scene/Scene.h"

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

	}

	void AnimationSystem::OnRuntimeStart()
	{
		// Play starts at the head of the clip whatever the editor was scrubbed to. The runtime
		// scene is a copy, so the editor scene keeps its scrub position for when play stops.
		for (auto [entity, animator, meshComponent] : View<AnimView>())
		{
			(void)entity;
			(void)meshComponent;
			animator.Time = 0.0f;
		}

		m_WarnedClips.clear();
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
		for (auto [entity, animator, meshComponent] : View<AnimView>())
		{
			Ref<Mesh> mesh = AssetManager::GetAsset<Mesh>(meshComponent.Mesh);
			if (!mesh || !mesh->HasSkeleton())
			{
				// An animator on a static mesh is a user error, not a crash. An empty palette is
				// also how RenderSystem tells "draw this the static way".
				animator.Palette.clear();
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

			BuildPalette(mesh->GetSkeleton(), clip, animator.Time, animator.Palette);
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

	void AnimationSystem::BuildPalette(const Skeleton& skeleton, const AnimationClip* clip, float time,
		std::vector<glm::mat4>& outPalette)
	{
		const uint32_t jointCount = skeleton.JointCount();
		if (skeleton.LocalRestPose.size() != jointCount || skeleton.InverseBind.size() != jointCount)
		{
			GE_CORE_ERROR("Skeleton arrays disagree on joint count - skipping palette");
			outPalette.clear();
			return;
		}

		// Start from the rest pose: a channel only overwrites the one path it drives, and a joint
		// with no channels at all has to keep its authored transform.
		m_Locals = skeleton.LocalRestPose;

		if (clip)
		{
			for (const AnimationClip::Channel& channel : clip->Channels)
			{
				if (channel.Joint >= jointCount || channel.Times.empty() || channel.Values.empty())
					continue;

				KeyPair key = FindKeys(channel.Times, time);
				if (channel.Mode == AnimationClip::Channel::Interp::Step)
					key.Alpha = 0.0f; // hold the left key

				const glm::vec4& a = channel.Values[key.Lower];
				const glm::vec4& b = channel.Values[key.Upper];
				JointPose& pose = m_Locals[channel.Joint];

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

		m_Globals.resize(jointCount);
		outPalette.resize(jointCount);

		// One forward pass - the importer sorts joints parents-before-children precisely so this
		// needs no recursion. Roots start from RootTransform, not identity: it carries whatever
		// sits above the skeleton in the glTF scene graph, which the inverse binds already
		// include (see Animation.h).
		for (uint32_t i = 0; i < jointCount; i++)
		{
			const int32_t parent = skeleton.ParentIndices[i];
			const glm::mat4& base = parent >= 0 ? m_Globals[parent] : skeleton.RootTransform;

			m_Globals[i] = base * m_Locals[i].ToMatrix();
			outPalette[i] = m_Globals[i] * skeleton.InverseBind[i];
		}
	}
}

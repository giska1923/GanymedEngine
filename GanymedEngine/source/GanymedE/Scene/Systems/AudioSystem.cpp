#include "gepch.h"
#include "AudioSystem.h"

#include "GanymedE/Assets/AssetManager.h"
#include "GanymedE/Assets/AssetPaths.h"
#include "GanymedE/Audio/AudioEngine.h"
#include "GanymedE/ECS/Singleton.h"
#include "GanymedE/Scene/Scene.h"
#include "GanymedE/Scene/SceneSingletons.h"

namespace GanymedE {

	namespace {

		// A zero or degenerate basis vector would make glm::normalize produce NaN, and a NaN
		// reaching miniaudio's spatializer poisons the mix from a background thread - the worst
		// kind of bug to trace back to a scale of 0 on an entity someone hid.
		glm::vec3 SafeDirection(const glm::vec3& axis, const glm::vec3& fallback)
		{
			const float lengthSquared = glm::dot(axis, axis);
			return lengthSquared > 1e-8f ? axis / std::sqrt(lengthSquared) : fallback;
		}

	}

	void AudioSystem::OnRuntimeStart()
	{
		m_Voices.clear();
		m_WarnedSources.clear();
		m_WarnedMultipleListeners = false;
		m_LoggedCameraFallback = false;

		for (auto [entity, source, worldTransform] : View<EmitterView>())
		{
			if (!source.PlayOnStart)
				continue;

			const VoiceId voice = EnsureVoice(entity, source);
			if (voice == InvalidVoiceId)
				continue;

			AudioEngine::SetVolume(voice, source.Volume);
			AudioEngine::SetPitch(voice, source.Pitch);
			if (source.Spatialize)
				AudioEngine::SetPosition(voice, glm::vec3(worldTransform.World[3]));

			AudioEngine::Play(voice);
		}
	}

	void AudioSystem::OnRuntimeStop()
	{
		for (auto& [entity, voice] : m_Voices)
		{
			(void)entity;
			AudioEngine::DestroyVoice(voice);
		}
		m_Voices.clear();

		// Stopping play silences everything - the PhysicsScene lifetime rule applied to sound.
		// The loop above covers what this system owns; StopAll also drops one-shots, which are
		// fired and forgotten by gameplay and have no owner left to stop them.
		AudioEngine::StopAll();
	}

	void AudioSystem::OnUpdate(Timestep ts)
	{
		(void)ts;

		UpdateListener();

		// Poll and push rather than track changes: three floats and a bool per voice is
		// cheaper than the bookkeeping a ChangeView would need, and it means a Lua setter
		// writing the component needs no MarkChanged to be heard.
		for (auto [entity, source, worldTransform] : View<EmitterView>())
		{
			auto it = m_Voices.find(entity);
			if (it == m_Voices.end())
				continue;

			const VoiceId voice = it->second;
			AudioEngine::SetVolume(voice, source.Volume);
			AudioEngine::SetPitch(voice, source.Pitch);
			AudioEngine::SetLooping(voice, source.Loop);

			if (source.Spatialize)
				AudioEngine::SetPosition(voice, glm::vec3(worldTransform.World[3]));
		}

		// Note what is deliberately absent: nothing frees a non-looping voice that has reached
		// its end. AudioEngine::Stop is pause-with-cursor and Play rewinds, so a finished voice
		// costs one paused ma_sound and makes the next play instant. Freeing it would trade
		// that for a re-decode on every replay of, say, a gunshot.
	}

	void AudioSystem::PlaySound(entt::entity entity)
	{
		// try_get rather than a view: this is called from outside the update loop's
		// iteration (a script, before this system runs) and asks about one entity, which
		// is what the immediate API is for. It is a read, so it cannot invalidate anything.
		const AudioSourceComponent* source = m_Scene.Reg().try_get<AudioSourceComponent>(entity);
		if (!source)
			return;

		// Creating on demand is what makes a source that never auto-played usable: the
		// first PlaySound builds the voice, every later one finds it. AudioEngine::Play is
		// itself idempotent and rewinds a finished voice, so a script may call this every
		// frame - which is the natural idiom, and the one PlayAnimation learned the hard
		// way. Restart-from-the-top is StopSound() then PlaySound().
		const VoiceId voice = EnsureVoice(entity, *source);
		if (voice == InvalidVoiceId)
			return;

		AudioEngine::SetVolume(voice, source->Volume);
		AudioEngine::SetPitch(voice, source->Pitch);
		AudioEngine::Play(voice);
	}

	void AudioSystem::StopSound(entt::entity entity)
	{
		// Not DestroyVoice: Stop keeps the playback cursor, and keeping the voice makes the
		// next PlaySound instant instead of a re-decode.
		auto it = m_Voices.find(entity);
		if (it != m_Voices.end())
			AudioEngine::Stop(it->second);
	}

	bool AudioSystem::IsSoundPlaying(entt::entity entity) const
	{
		auto it = m_Voices.find(entity);
		return it != m_Voices.end() && AudioEngine::IsPlaying(it->second);
	}

	VoiceId AudioSystem::EnsureVoice(entt::entity entity, const AudioSourceComponent& source)
	{
		auto existing = m_Voices.find(entity);
		if (existing != m_Voices.end())
			return existing->second;

		if (!IsAssetHandleValid(source.Clip))
		{
			if (m_WarnedSources.insert(entity).second)
				GE_CORE_WARN("AudioSourceComponent has no clip assigned - nothing to play");
			return InvalidVoiceId;
		}

		// Handle -> path, the Script precedent: there is no GetAsset<AudioClip>, because
		// miniaudio's resource manager is the cache (docs/engine/audio.md).
		const AssetMetadata* metadata = AssetManager::GetMetadata(source.Clip);
		if (!metadata)
		{
			// The path is what the reader needs, and here there isn't one - which is itself
			// the diagnosis. This is where a fresh clone with a missing or stale registry
			// shows up first.
			if (m_WarnedSources.insert(entity).second)
				GE_CORE_WARN("Audio clip handle {0} is not in the asset registry - the source "
					"is silent. A missing or stale assets/AssetRegistry.gr is the usual cause.",
					static_cast<uint64_t>(source.Clip));
			return InvalidVoiceId;
		}

		const std::filesystem::path fullPath = GetAssetRoot() / metadata->FilePath;

		const VoiceId voice = AudioEngine::CreateVoice(fullPath, source.Group,
			source.Spatialize, source.Stream, source.Loop);

		if (voice == InvalidVoiceId)
		{
			// CreateVoice already logged the full path and miniaudio's reason; adding a second
			// line here would say less, twice.
			m_WarnedSources.insert(entity);
			return InvalidVoiceId;
		}

		m_Voices[entity] = voice;
		return voice;
	}

	void AudioSystem::UpdateListener()
	{
		glm::mat4 listenerTransform{ 1.0f };
		bool found = false;
		int primaryCount = 0;

		for (auto [entity, listener, worldTransform] : View<ListenerView>())
		{
			(void)entity;
			if (!listener.Primary)
				continue;

			primaryCount++;
			if (!found)
			{
				listenerTransform = worldTransform.World;
				found = true;   // first primary wins, matching CameraSystem
			}
		}

		if (primaryCount > 1 && !m_WarnedMultipleListeners)
		{
			m_WarnedMultipleListeners = true;
			GE_CORE_WARN("{0} primary AudioListenerComponents in the scene - using the first. "
				"There is exactly one pair of ears.", primaryCount);
		}

		if (!found)
		{
			// Fallback: hear from the primary camera. CameraSystem resolved it earlier in this
			// same update, so the pose is this frame's, not last frame's - which is the reason
			// AudioSystem is registered after it.
			ECS::SingletonAccessView<RenderContext> renderView{ m_Scene };
			const RenderContext& context = *renderView.Get();

			if (!context.MainCamera)
				return;   // no listener and no camera: leave the listener wherever it was

			listenerTransform = context.CameraTransform;
			found = true;

			if (!m_LoggedCameraFallback)
			{
				m_LoggedCameraFallback = true;
				GE_CORE_INFO("No AudioListenerComponent in the scene - listening from the "
					"primary camera");
			}
		}

		// Right-handed, -Z forward: the same convention as a camera, and miniaudio's default,
		// so these go straight through untouched (docs/engine/audio.md).
		const glm::vec3 position{ listenerTransform[3] };
		const glm::vec3 forward = SafeDirection(-glm::vec3(listenerTransform[2]), { 0.0f, 0.0f, -1.0f });
		const glm::vec3 up = SafeDirection(glm::vec3(listenerTransform[1]), { 0.0f, 1.0f, 0.0f });

		AudioEngine::SetListener(position, forward, up);
	}
}

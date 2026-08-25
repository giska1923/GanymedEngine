#include "gepch.h"
#include "GanymedE/Audio/AudioEngine.h"

// Declarations only - the implementation lives in miniaudio_impl.cpp.
#if defined(_MSC_VER)
	#pragma warning(push, 0)
#endif

#include <miniaudio.h>

#if defined(_MSC_VER)
	#pragma warning(pop)
#endif

#include <list>

namespace GanymedE {

	namespace {

		struct AudioEngineData
		{
			ma_engine Engine{};

			// Master is the engine endpoint, so only two real groups exist.
			ma_sound_group MusicGroup{};
			ma_sound_group SfxGroup{};

			// A ma_sound is a node in miniaudio's graph and its neighbours point at it
			// by address, so it must never be relocated after init. Both containers are
			// node-based for that reason - a vector of ma_sound would reallocate and
			// leave the graph pointing at freed memory.
			std::unordered_map<VoiceId, ma_sound> Voices;
			std::list<ma_sound> OneShots;

			// Never reset, not even by a re-Init: ids stay unique for the life of the
			// process, so a VoiceId held across a Shutdown/Init pair cannot alias a
			// different sound afterwards. It only has to no-op, and it does.
			VoiceId NextVoiceId = 1;
			bool Initialized = false;
		};

		AudioEngineData s_Data;

		// Master returns null: miniaudio attaches a null-group sound straight to the
		// engine endpoint, which is what "master bus" means here.
		ma_sound_group* GetGroup(AudioGroup group)
		{
			switch (group)
			{
				case AudioGroup::Music: return &s_Data.MusicGroup;
				case AudioGroup::SFX:   return &s_Data.SfxGroup;
				default:                return nullptr;
			}
		}

		ma_sound* FindVoice(VoiceId voice)
		{
			if (!s_Data.Initialized || voice == InvalidVoiceId)
				return nullptr;

			auto it = s_Data.Voices.find(voice);
			return it != s_Data.Voices.end() ? &it->second : nullptr;
		}

		// Sounds load synchronously (no MA_SOUND_FLAG_ASYNC). Two reasons: a missing or
		// undecodable file then fails HERE, at the call site that knows which entity and
		// which path is at fault, instead of on a job thread some frames later; and it
		// matches the rest of the asset layer, which is synchronous throughout. The cost
		// is a decode hitch the first time a clip is used - which is what the authored
		// stream flag is for on the long files where it would be noticeable, and which
		// miniaudio's resource manager charges only once per path.
		//
		// The upgrade path if that stops being good enough is MA_SOUND_FLAG_ASYNC plus a
		// ma_fence to report load results; it is a bigger change than it looks, because
		// every caller's error handling moves off the call site.
		ma_uint32 MakeSoundFlags(bool spatial, bool stream)
		{
			// DECODE trades memory for CPU: the whole file becomes PCM once, shared by
			// every voice on that path, so replaying an SFX costs nothing. STREAM is the
			// opposite trade and excludes it.
			ma_uint32 flags = stream ? MA_SOUND_FLAG_STREAM : MA_SOUND_FLAG_DECODE;

			// Skips allocating a spatializer rather than merely bypassing it.
			if (!spatial)
				flags |= MA_SOUND_FLAG_NO_SPATIALIZATION;

			return flags;
		}

	}

	void AudioEngine::Init()
	{
		if (s_Data.Initialized)
			return;

		ma_engine_config config = ma_engine_config_init();

		ma_result result = ma_engine_init(&config, &s_Data.Engine);
		if (result != MA_SUCCESS)
		{
			// Not fatal, by design. A headless build machine, a VM with no sound card
			// or a user with every output device disabled must still be able to run the
			// game; Initialized stays false and every call below becomes a no-op.
			GE_CORE_ERROR("AudioEngine: no audio device ({0}) - continuing without sound",
				ma_result_description(result));
			return;
		}

		if (ma_sound_group_init(&s_Data.Engine, 0, nullptr, &s_Data.MusicGroup) != MA_SUCCESS ||
			ma_sound_group_init(&s_Data.Engine, 0, nullptr, &s_Data.SfxGroup) != MA_SUCCESS)
		{
			GE_CORE_ERROR("AudioEngine: failed to create sound groups - continuing without sound");
			ma_engine_uninit(&s_Data.Engine);
			return;
		}

		s_Data.Initialized = true;

		ma_device* device = ma_engine_get_device(&s_Data.Engine);

		char deviceName[MA_MAX_DEVICE_NAME_LENGTH + 1] = {};
		ma_device_get_name(device, ma_device_type_playback, deviceName, sizeof(deviceName), nullptr);

		GE_CORE_INFO("AudioEngine initialized ({0}, '{1}', {2} Hz, {3} channels)",
			ma_get_backend_name(ma_device_get_context(device)->backend),
			deviceName,
			ma_engine_get_sample_rate(&s_Data.Engine),
			ma_engine_get_channels(&s_Data.Engine));
	}

	void AudioEngine::Shutdown()
	{
		if (!s_Data.Initialized)
			return;

		// Strict order, the same shape as the Application dtor: leaves first. Sounds are
		// graph nodes attached to the groups, and the groups to the engine; uninitialising
		// the engine out from under a live sound tears down the node it is attached to.
		for (auto& [id, sound] : s_Data.Voices)
			ma_sound_uninit(&sound);
		s_Data.Voices.clear();

		for (ma_sound& sound : s_Data.OneShots)
			ma_sound_uninit(&sound);
		s_Data.OneShots.clear();

		ma_sound_group_uninit(&s_Data.MusicGroup);
		ma_sound_group_uninit(&s_Data.SfxGroup);
		ma_engine_uninit(&s_Data.Engine);

		s_Data.Initialized = false;
		GE_CORE_INFO("AudioEngine shut down");
	}

	bool AudioEngine::IsInitialized()
	{
		return s_Data.Initialized;
	}

	void AudioEngine::OnUpdate()
	{
		if (!s_Data.Initialized)
			return;

		// Reap finished one-shots. Nobody owns them, so nothing else would ever free
		// them, and each one holds a reference into the resource manager's decoded data
		// - a game firing footsteps for ten minutes would otherwise pin every clip it
		// ever played. at_end is miniaudio's own recycling signal for inlined sounds.
		for (auto it = s_Data.OneShots.begin(); it != s_Data.OneShots.end(); )
		{
			if (ma_sound_at_end(&(*it)))
			{
				ma_sound_uninit(&(*it));
				it = s_Data.OneShots.erase(it);
			}
			else
			{
				++it;
			}
		}
	}

	VoiceId AudioEngine::CreateVoice(const std::filesystem::path& fullPath, AudioGroup group,
		bool spatial, bool stream, bool loop)
	{
		if (!s_Data.Initialized)
			return InvalidVoiceId;

		VoiceId voice = s_Data.NextVoiceId++;

		// Insert first, then initialise in place: ma_sound_init_from_file records the
		// node's own address in the graph, so it has to be built where it will live.
		ma_sound& sound = s_Data.Voices[voice];

		ma_result result = ma_sound_init_from_file(&s_Data.Engine, fullPath.string().c_str(),
			MakeSoundFlags(spatial, stream), GetGroup(group), nullptr, &sound);

		if (result != MA_SUCCESS)
		{
			s_Data.Voices.erase(voice);
			GE_CORE_WARN("AudioEngine: could not load '{0}' ({1})",
				fullPath.generic_string(), ma_result_description(result));
			return InvalidVoiceId;
		}

		ma_sound_set_looping(&sound, loop ? MA_TRUE : MA_FALSE);
		return voice;
	}

	void AudioEngine::DestroyVoice(VoiceId voice)
	{
		ma_sound* sound = FindVoice(voice);
		if (!sound)
			return;

		ma_sound_uninit(sound);
		s_Data.Voices.erase(voice);
	}

	void AudioEngine::Play(VoiceId voice)
	{
		// ma_sound_start is already idempotent and already rewinds a sound that has
		// reached its end, which is exactly the contract the header advertises - so
		// there is nothing to add here. Do not "improve" this into an unconditional
		// seek: that would restart the voice on every frame a script calls PlaySound.
		if (ma_sound* sound = FindVoice(voice))
			ma_sound_start(sound);
	}

	void AudioEngine::Stop(VoiceId voice)
	{
		if (ma_sound* sound = FindVoice(voice))
			ma_sound_stop(sound);
	}

	bool AudioEngine::IsPlaying(VoiceId voice)
	{
		ma_sound* sound = FindVoice(voice);
		return sound && ma_sound_is_playing(sound);
	}

	void AudioEngine::SetVolume(VoiceId voice, float volume)
	{
		if (ma_sound* sound = FindVoice(voice))
			ma_sound_set_volume(sound, volume);
	}

	void AudioEngine::SetPitch(VoiceId voice, float pitch)
	{
		// miniaudio ignores a pitch <= 0 rather than dividing by it, so no guard here.
		if (ma_sound* sound = FindVoice(voice))
			ma_sound_set_pitch(sound, pitch);
	}

	void AudioEngine::SetLooping(VoiceId voice, bool loop)
	{
		if (ma_sound* sound = FindVoice(voice))
			ma_sound_set_looping(sound, loop ? MA_TRUE : MA_FALSE);
	}

	void AudioEngine::SetPosition(VoiceId voice, const glm::vec3& position)
	{
		if (ma_sound* sound = FindVoice(voice))
			ma_sound_set_position(sound, position.x, position.y, position.z);
	}

	void AudioEngine::SetListener(const glm::vec3& position, const glm::vec3& forward, const glm::vec3& up)
	{
		if (!s_Data.Initialized)
			return;

		// One listener (index 0). Split-screen would want more; that is not v1.
		ma_engine_listener_set_position(&s_Data.Engine, 0, position.x, position.y, position.z);
		ma_engine_listener_set_direction(&s_Data.Engine, 0, forward.x, forward.y, forward.z);
		ma_engine_listener_set_world_up(&s_Data.Engine, 0, up.x, up.y, up.z);
	}

	void AudioEngine::PlayOneShot(const std::filesystem::path& fullPath, AudioGroup group,
		const glm::vec3* position, float volume)
	{
		if (!s_Data.Initialized)
			return;

		// Same reason as CreateVoice: emplace, then initialise in place.
		ma_sound& sound = s_Data.OneShots.emplace_back();

		ma_result result = ma_sound_init_from_file(&s_Data.Engine, fullPath.string().c_str(),
			MakeSoundFlags(position != nullptr, false), GetGroup(group), nullptr, &sound);

		if (result != MA_SUCCESS)
		{
			s_Data.OneShots.pop_back();
			GE_CORE_WARN("AudioEngine: could not load one-shot '{0}' ({1})",
				fullPath.generic_string(), ma_result_description(result));
			return;
		}

		ma_sound_set_volume(&sound, volume);
		if (position)
			ma_sound_set_position(&sound, position->x, position->y, position->z);

		ma_sound_start(&sound);
	}

	void AudioEngine::SetGroupVolume(AudioGroup group, float volume)
	{
		if (!s_Data.Initialized)
			return;

		if (ma_sound_group* target = GetGroup(group))
			ma_sound_group_set_volume(target, volume);
		else
			ma_engine_set_volume(&s_Data.Engine, volume); // Master is the endpoint
	}

	void AudioEngine::StopAll()
	{
		if (!s_Data.Initialized)
			return;

		for (auto& [id, sound] : s_Data.Voices)
			ma_sound_stop(&sound);

		// One-shots are dropped rather than paused: nothing owns them, so a paused
		// one-shot would sit in the list until Shutdown.
		for (ma_sound& sound : s_Data.OneShots)
			ma_sound_uninit(&sound);
		s_Data.OneShots.clear();
	}

	size_t AudioEngine::GetVoiceCount()
	{
		return s_Data.Voices.size();
	}

	size_t AudioEngine::GetOneShotCount()
	{
		return s_Data.OneShots.size();
	}

}

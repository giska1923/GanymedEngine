#pragma once

#include "GanymedE/Audio/AudioTypes.h"
#include "GanymedE/Core/Core.h"

#include <glm/glm.hpp>

#include <filesystem>

namespace GanymedE {

	// Audio playback, the counterpart to Renderer on the sound side: a global owning
	// the one miniaudio engine, its two groups, and every live voice.
	//
	// No miniaudio types in this header. Everything the engine owns - the device, the
	// two groups, the voice table - is a file-static in the .cpp, which is the same
	// firewall PhysicsScene gets from its pimpl; a static class needs no pimpl
	// pointer to get it.
	//
	// ---- The alive guard, and why every call has one ----
	//
	// Shutdown() runs in the Application DESTRUCTOR BODY. The LayerStack - and every
	// scene in it, and whatever stops that scene's sounds - is destroyed afterwards, when
	// members unwind. So a layer's OnDetach stopping its sounds runs against an
	// already-uninitialised ma_engine. Every public function here therefore checks an
	// initialised flag first and does nothing when it is false. This is exactly
	// Renderer::IsGpuAlive applied to audio: same ordering problem, same solution.
	// Do not "simplify" it away.
	//
	// The same flag covers the other failure this class must survive: a machine with
	// no output device (a headless CI box, a VM with audio disabled). Init() logs the
	// failure and leaves the flag false; the game then runs silent instead of dying.
	//
	// ---- Paths ----
	//
	// Voices take a path to a real file on disk, not an AssetHandle: AssetType::Audio
	// is path-resolved by design and there is deliberately no GetAsset<AudioClip> -
	// miniaudio's resource manager already ref-counts and caches decoded data by path,
	// and a second cache over the same resource is a lifetime argument waiting to
	// happen (docs/engine/audio.md). Callers resolve handle -> path through
	// AssetManager::GetMetadata, the same way scripts do.
	//
	// Paths are handed to miniaudio as path::string(), i.e. the platform's native
	// narrow encoding. On Windows that is the ANSI code page (miniaudio's default VFS
	// calls CreateFileA), so a path outside it will not open. Asset paths are ASCII in
	// practice; wiring up miniaudio's wide-char entry points is the fix if that ever
	// stops being true.
	class AudioEngine
	{
	public:
		// No ordering dependency on the renderer or the VM - audio touches neither.
		// Called from the Application ctor after Renderer::Init purely so the boot log
		// reads in a stable order, and from the dtor body between ScriptEngine::Shutdown
		// and Renderer::Shutdown for the same reason. Init on an already-initialised
		// engine is a no-op; Init after Shutdown re-opens the device.
		static void Init();
		static void Shutdown();

		static bool IsInitialized();

		// Frees one-shots that have finished playing. Called once per frame from
		// Application::Run, outside the minimised gate - a window that stopped
		// rendering has not stopped making noise.
		static void OnUpdate();

		// ---- Voices ----
		//
		// A voice is a persistent, addressable sound: created once, played and stopped
		// many times, destroyed when its owner goes away. It is a live resource, not a
		// value - exactly one owner should hold a given id, and that owner is
		// responsible for destroying it. (The ECS layer that will own one per audio
		// source is Phase 4 of docs/history/RUNTIME_AUDIO_ROADMAP.md.)
		//
		// stream = decode on the fly from disk (music, long ambience) instead of
		// decoding the whole file into memory once (everything else). It is an authored
		// choice, not a size heuristic - see docs/engine/audio.md.
		static VoiceId CreateVoice(const std::filesystem::path& fullPath, AudioGroup group,
			bool spatial, bool stream, bool loop);
		static void DestroyVoice(VoiceId voice);

		// Idempotent: playing an already-playing voice does nothing, so a script may
		// call this every frame without stuttering. A voice that has reached the end
		// rewinds. Restart-from-the-top is Stop() then Play().
		static void Play(VoiceId voice);

		// Pause semantics: the playback cursor is kept, so Play() resumes where this
		// left off. There is no separate Pause().
		static void Stop(VoiceId voice);

		static bool IsPlaying(VoiceId voice);

		static void SetVolume(VoiceId voice, float volume);   // linear, 1.0 = unattenuated
		static void SetPitch(VoiceId voice, float pitch);     // 1.0 = unmodified; also scales speed
		static void SetLooping(VoiceId voice, bool loop);
		static void SetPosition(VoiceId voice, const glm::vec3& position); // world space; ignored on non-spatial voices

		// Right-handed, -Z forward - miniaudio's default handedness, which is glm's and
		// the engine's, so world vectors go straight through with no conversion.
		static void SetListener(const glm::vec3& position, const glm::vec3& forward, const glm::vec3& up);

		// Fire and forget: an engine-owned voice that plays once and is freed by
		// OnUpdate when it finishes. For footsteps and impacts, which should not need
		// an entity each. Pass nullptr for position to play unspatialised (UI, 2D).
		//
		// Not ma_engine_play_sound(): that one hard-disables spatialisation and pitch
		// and takes no volume, so it cannot serve a positioned SFX.
		static void PlayOneShot(const std::filesystem::path& fullPath, AudioGroup group,
			const glm::vec3* position, float volume);

		static void SetGroupVolume(AudioGroup group, float volume);

		// Stops every voice (keeping their cursors, as Stop does) and discards every
		// in-flight one-shot, which has no owner left to resume it. This is what a
		// scene-scoped caller uses when play mode ends: stopping play silences
		// everything, the PhysicsScene lifetime rule applied to sound.
		static void StopAll();

		// Diagnostics. Voice count is what callers created and have not destroyed;
		// one-shot count is what OnUpdate is waiting to reap.
		static size_t GetVoiceCount();
		static size_t GetOneShotCount();
	};

}

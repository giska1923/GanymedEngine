#pragma once

#include <unordered_map>
#include <unordered_set>

#include "GanymedE/Audio/AudioTypes.h"
#include "GanymedE/ECS/System.h"
#include "GanymedE/ECS/Views.h"
#include "GanymedE/Scene/Components.h"

namespace GanymedE {

	// Drives the AudioEngine from the scene: creates a voice per AudioSourceComponent that
	// asks for one, pushes its authored state and world position every frame, and resolves
	// which entity's pose is the listener.
	//
	// Voices live HERE, in a map keyed by entity, not on the component. That is the opposite
	// of the AnimatorComponent palette decision and the difference is the point: a palette is
	// pure data produced by one system and read by another through declared access, while a
	// voice is a live foreign resource with a lifecycle - which is what a Jolt body is, and
	// the engine already decided those belong to the system (PhysicsScene). The map lives and
	// dies with the run, so a stop leaves nothing behind and Scene::Copy needs no audio fixup.
	//
	// Edit mode is SILENT: there is no OnUpdateEditor override. This follows the engine's
	// "systems simulate only in play mode" norm - stated explicitly because AnimationSystem
	// deliberately diverges from it (it samples poses in edit mode so the inspector's scrub
	// slider can work). Two decisions, not an accident; an inspector preview-play button is
	// deferred, see docs/engine/audio.md.
	class AudioSystem : public ECS::System<AudioSystem>
	{
	public:
		// Both read the cached world transform, which TransformSystem refreshed earlier in
		// the same update. No reactive views, so no editor drain obligation.
		using EmitterView = ECS::IterView<ECS::EntityId,
			ECS::RO<AudioSourceComponent>, ECS::RO<WorldTransformComponent>>;
		using ListenerView = ECS::IterView<ECS::EntityId,
			ECS::RO<AudioListenerComponent>, ECS::RO<WorldTransformComponent>>;

		using Views = TypeList<EmitterView, ListenerView>;

		using ECS::System<AudioSystem>::System;

		void OnRuntimeStart() override;
		void OnRuntimeStop() override;
		void OnUpdate(Timestep ts) override;
		const char* Name() const override { return "AudioSystem"; }

		// ---- Gameplay control, for scripts ----
		//
		// This is the route a script takes to start or stop a sound, and it exists so the
		// voice map stays the single owner - the PhysicsScene arrangement, where scripts
		// reach the live Jolt body through the system rather than holding one. Volume,
		// pitch and looping are NOT here: those are authored fields the update loop pushes
		// every frame, so a script writes the component directly and the next update
		// carries it (the AnimatorComponent arrangement). Which half a setter belongs to
		// is decided by where the value lives, not by taste.
		//
		// All no-op on an entity with no AudioSourceComponent, on an unloadable clip, and
		// outside play - a script poking at the wrong entity should not take the game down.
		void PlaySound(entt::entity entity);
		void StopSound(entt::entity entity);
		bool IsSoundPlaying(entt::entity entity) const;

	private:
		// Idempotent: returns the entity's existing voice, or builds one from the component's
		// authored flags. InvalidVoiceId when the clip cannot be resolved or loaded, having
		// warned at most once for that entity.
		VoiceId EnsureVoice(entt::entity entity, const AudioSourceComponent& source);

		void UpdateListener();

	private:
		std::unordered_map<entt::entity, VoiceId> m_Voices;

		// Entities already reported as having an unusable clip. A source with a broken handle
		// is broken for the whole run, and EnsureVoice may be called repeatedly for it.
		std::unordered_set<entt::entity> m_WarnedSources;

		bool m_WarnedMultipleListeners = false;
		bool m_LoggedCameraFallback = false;
	};
}

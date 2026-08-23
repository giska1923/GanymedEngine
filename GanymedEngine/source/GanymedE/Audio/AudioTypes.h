#pragma once

#include <cstdint>
#include <string>

namespace GanymedE {

	// The vocabulary of the audio subsystem, split out of AudioEngine.h for the same reason
	// AssetTypes.h is split out of AssetManager.h: Components.h needs AudioGroup and nothing
	// else, and should not pull in <filesystem> and a facade full of static methods to get it.

	// Handle to a live sound owned by the AudioEngine. 0 is never issued, so an unset
	// VoiceId is safely invalid and every AudioEngine call no-ops on it.
	using VoiceId = uint32_t;

	inline constexpr VoiceId InvalidVoiceId = 0;

	// The whole mixer. Master is the engine endpoint; Music and SFX are the two buses
	// hanging off it. Arbitrary bus graphs, sends and ducking are out of scope - see
	// docs/engine/audio.md.
	//
	// Serialized by NAME, not by ordinal, so this may be reordered freely - unlike
	// AssetType, which is persisted by ordinal in the registry.
	enum class AudioGroup : uint8_t
	{
		Master = 0,
		Music,
		SFX
	};

	const char* AudioGroupToString(AudioGroup group);

	// Unknown names warn and return the fallback rather than throwing: a scene with one
	// mistyped group should still load.
	AudioGroup AudioGroupFromString(const std::string& name, AudioGroup fallback = AudioGroup::SFX);

}

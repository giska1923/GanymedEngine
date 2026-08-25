#include "gepch.h"
#include "GanymedE/Audio/AudioTypes.h"

namespace GanymedE {

	const char* AudioGroupToString(AudioGroup group)
	{
		switch (group)
		{
			case AudioGroup::Master: return "Master";
			case AudioGroup::Music:  return "Music";
			case AudioGroup::SFX:    return "SFX";
			default:                 return "Unknown";
		}
	}

	AudioGroup AudioGroupFromString(const std::string& name, AudioGroup fallback)
	{
		if (name == "Master") return AudioGroup::Master;
		if (name == "Music")  return AudioGroup::Music;
		if (name == "SFX")    return AudioGroup::SFX;

		GE_CORE_WARN("Unknown audio group '{0}' - falling back to {1}",
			name, AudioGroupToString(fallback));
		return fallback;
	}

}

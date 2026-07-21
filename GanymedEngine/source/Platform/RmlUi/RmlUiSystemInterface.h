#pragma once

#include <RmlUi/Core/SystemInterface.h>

namespace GanymedE {

	// Minimal SystemInterface: the clock RmlUi animates from, and log routing.
	//
	// Deliberately NOT the reference RmlUi_Platform_GLFW from extern/RmlUi/Backends:
	// the bulk of that file is input translation and cursor/clipboard glue, none of
	// which has a caller until UI input lands. Its key-mapping table is worth
	// copying then - the engine's KeyCodes.h uses GLFW values, so it maps 1:1.
	//
	// Every SystemInterface method has a base implementation, so only the two that
	// actually need engine behaviour are overridden.
	class RmlUiSystemInterface : public Rml::SystemInterface
	{
	public:
		double GetElapsedTime() override;
		bool LogMessage(Rml::Log::Type type, const Rml::String& message) override;
	};
}

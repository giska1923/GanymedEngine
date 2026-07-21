#pragma once

#include "GanymedE/Core/KeyCodes.h"

#include <RmlUi/Core/Input.h>

namespace GanymedE {

	// Translation between the engine's input codes and RmlUi's.
	//
	// The engine's KeyCodes.h/MouseButtonCodes.h are GLFW values verbatim, and so is
	// what RmlUi's reference GLFW backend switches on - so the mapping below is
	// copied from extern/RmlUi/Backends/RmlUi_Platform_GLFW.cpp and still keyed on
	// the GLFW_KEY_* constants. Keeping their names means it can be re-extracted
	// verbatim when RmlUi is upgraded, instead of being hand-translated into
	// Key::* names and quietly drifting.
	namespace RmlUiInput {

		Rml::Input::KeyIdentifier ConvertKey(int engineKeyCode);

		// Built by polling Input rather than from a callback's mods bitfield, which
		// the engine's events do not carry. Consequence: the lock keys
		// (KM_CAPSLOCK / KM_NUMLOCK / KM_SCROLLLOCK) are never reported. Nothing in
		// RCSS or the default widgets depends on them.
		int GetModifierState();

		// RmlUi button indices match GLFW's (0 left, 1 right, 2 middle).
		inline int ConvertMouseButton(int engineMouseCode) { return engineMouseCode; }
	}
}

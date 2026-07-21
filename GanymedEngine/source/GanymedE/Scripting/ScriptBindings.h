#pragma once

// PRIVATE to Scripting/. This is the one header in the engine that includes sol2, and it must stay
// that way: sol2's templates are heavy, and Components.h / the system headers are pulled into most
// translation units. Include this only from Scripting/*.cpp.

#include <sol/sol.hpp>

namespace GanymedE {

	// Populates the shared VM's globals: Vec3, Entity, Input, Key, Mouse, Log, Scene, UI.
	// Defined in ScriptBindings.cpp; kept out of ScriptEngine.cpp so the binding surface is one
	// file to read and the TS declarations in scripts-src/types/ganymed.d.ts have one file to
	// mirror.
	void RegisterScriptBindings(sol::state& lua);

	// Just the plain global tables, without re-registering the usertypes.
	//
	// Exists because RmlUi's Lua plugin installs globals of its own into the same state, and one
	// of them is called `Log` - so whichever runs last wins. See ScriptEngine::ReinstallGlobals.
	void RegisterScriptGlobals(sol::state& lua);
}

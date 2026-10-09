#pragma once

// PRIVATE to Scripting/. This is the one header in the engine that includes sol2, and it must stay
// that way: sol2's templates are heavy, and Components.h / the system headers are pulled into most
// translation units. Include this only from Scripting/*.cpp.

#include <sol/sol.hpp>

#include "GanymedE/Online/Json.h"
#include "GanymedE/Online/Online.h"

#include <functional>

namespace GanymedE {

	// Populates the shared VM's globals: Vec3, Entity, Input, Key, Mouse, Log, Scene, UI, Backend.
	// Defined in ScriptBindings.cpp; kept out of ScriptEngine.cpp so the binding surface is one
	// file to read and the TS declarations in scripts-src/types/ganymed.d.ts have one file to
	// mirror.
	void RegisterScriptBindings(sol::state& lua);

	// Just the plain global tables, without re-registering the usertypes.
	//
	// Exists because RmlUi's Lua plugin installs globals of its own into the same state, and one
	// of them is called `Log` - so whichever runs last wins. See ScriptEngine::ReinstallGlobals.
	void RegisterScriptGlobals(sol::state& lua);

	// ---- Backend requests owned by a script (docs/engine/online.md) ----------------------------

	// Turns a 2xx response into the value a script's callback receives second. Throw a
	// std::exception for a body without the expected shape: the script then gets
	// `ok == false, "malformed response: <what>"`, not a half-read table.
	using ScriptResponseReader = std::function<sol::object(sol::state_view lua, const OnlineResponse& response)>;

	// Sends `request` for the script instance on `owner` in the current scene, and calls
	// `callback(ok, value)` inside that scene's script update a frame or more later - or never, if
	// the instance or its scene is gone by then (the request is cancelled when they go).
	//
	// `owner` and `callback` are sol::objects so that a missing or wrong argument gets this
	// function's error, naming `binding`, rather than sol2's generic "expected userdata". Throws
	// sol::error - which fails the calling script, and disables it like any script error - when
	// `owner` is not an entity with a live script instance in the current scene, or `callback` is
	// not a function. Nothing is sent then.
	void SendScriptRequest(const char* binding, const sol::object& owner, OnlineRequest request,
		const sol::object& callback, ScriptResponseReader read);

	// JSON <-> Lua. JSON null becomes nil (so it is absent from a table), integers stay Lua
	// integers, arrays become 1-based sequences. LuaToJson throws sol::error on a value JSON
	// cannot carry (a function, a userdata, a table with mixed keys).
	sol::object JsonToLua(sol::state_view lua, const JsonValue& value);
	JsonValue LuaToJson(const sol::object& value);
}

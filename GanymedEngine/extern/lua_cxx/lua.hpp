// C++ wrapper for the Lua C headers.
//
// This file lives outside the lua submodule on purpose. The official lua/lua git
// mirror is the raw source tree and does NOT ship lua.hpp - that header only exists
// in the packaged release tarballs. sol2 includes <lua.hpp> unconditionally, so the
// shim has to come from somewhere; putting it inside the submodule would leave the
// submodule permanently dirty in git status, exactly like writing bin/ in there does.
//
// Contents are verbatim from the upstream 5.4 distribution.

extern "C" {
#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"
}

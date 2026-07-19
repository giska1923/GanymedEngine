# Scripting & UI Integration Guide

**Lua 5.4 + sol2 (scripting) · TypeScriptToLua (TS authoring) · RmlUi (HTML/CSS UI)**

This document describes how to integrate a complete gameplay scripting and UI stack
into GanymedEngine, following the engine's existing conventions (premake5 workspace,
vendored source dependencies as static libs, EnTT ECS, Hazel-style scene lifecycle).

---

## 1. Architecture Overview

```
┌─────────────────────────────────────────────────────────────────┐
│  Authoring (tooling only, never shipped)                        │
│                                                                 │
│  TypeScript sources (scripts-src/)                              │
│      │  tstl --watch  (TypeScriptToLua)                         │
│      ▼                                                          │
│  Lua 5.4 files  →  GanymedEditor/assets/scripts/*.lua           │
└──────────────────────────────┬──────────────────────────────────┘
                               │ loaded as assets
┌──────────────────────────────▼──────────────────────────────────┐
│  Engine (C++)                                                   │
│                                                                 │
│  ScriptEngine (sol2 → Lua 5.4 VM)                               │
│    • one sol::state for the whole runtime                       │
│    • one instance table per scripted entity                     │
│    • lifecycle: OnCreate / OnUpdate / OnCollisionEnter/Exit /   │
│      OnDestroy — mirrors ScriptableEntity                       │
│                                                                 │
│  UIEngine (RmlUi)                                               │
│    • RML/RCSS documents rendered via the GL3 backend            │
│    • data bindings for C++ → UI values                          │
│    • RmlUi Lua plugin runs on the SAME lua_State, so UI logic   │
│      can also be written in TS → Lua                            │
└─────────────────────────────────────────────────────────────────┘
```

Key design decisions:

1. **One Lua VM** (`sol::state`) shared by gameplay scripts and RmlUi's Lua plugin.
   Scripts and UI can call each other; only one set of bindings to maintain.
2. **TypeScript is a pure tooling layer.** The engine only ever sees `.lua` files.
   No JS engine, no Node at runtime.
3. **`ScriptComponent` stays a POD** (just a path string). All `sol::` objects live
   inside `ScriptEngine`, keyed by entity UUID — `Components.h` stays sol2-free and
   `Scene::Copy` keeps working without changes to its copy semantics.
4. The existing `NativeScriptComponent` stays untouched — Lua scripting is added
   alongside it.

---

## 2. Part 1 — Embedding Lua 5.4 + sol2

### 2.1 Add submodules

```bash
git submodule add https://github.com/lua/lua GanymedEngine/extern/lua
cd GanymedEngine/extern/lua && git checkout v5.4.8 && cd -   # latest 5.4.x tag

git submodule add https://github.com/ThePhD/sol2 GanymedEngine/extern/sol2
cd GanymedEngine/extern/sol2 && git checkout v3.3.0 && cd -  # or newest release tag
```

- `lua/lua` is the official read-only source mirror — plain C files, no build system
  needed, which is perfect for the engine's "build everything as a premake static lib"
  approach.
- sol2 is header-only; only an include dir is needed.

### 2.2 Premake build script for Lua

Following the `Jolt.lua` / `GLFW.lua` pattern (build script lives *outside* the
submodule), create `GanymedEngine/extern/Lua.lua`:

```lua
-- Build script for the Lua submodule. Lives outside the submodule because
-- git cannot track files inside a submodule's working tree.
project "Lua"
	kind "StaticLib"
	language "C"
	staticruntime "off"
	warnings "Off"

	targetdir ("lua/bin/" .. outputdir .. "/%{prj.name}")
	objdir ("lua/bin-int/" .. outputdir .. "/%{prj.name}")

	files
	{
		"lua/*.c",
		"lua/*.h"
	}

	-- lua.c = standalone interpreter main(), onelua.c = amalgamated build,
	-- ltests.c = internal test hooks. None belong in the embedded library.
	removefiles
	{
		"lua/lua.c",
		"lua/onelua.c",
		"lua/ltests.c"
	}

	filter "system:windows"
		systemversion "latest"

	filter "system:linux"
		pic "On"
		systemversion "latest"

	filter "configurations:Debug"
		runtime "Debug"
		symbols "on"

	filter "configurations:Release"
		runtime "Release"
		optimize "on"

	filter "configurations:Dist"
		runtime "Release"
		optimize "on"
		symbols "off"
```

### 2.3 Wire into the workspace

Root `premake5.lua`:

```lua
IncludeDir["lua"]  = "%{wks.location}/GanymedEngine/extern/lua"
IncludeDir["sol2"] = "%{wks.location}/GanymedEngine/extern/sol2/include"

group "Dependencies"
	-- ...existing includes...
	include "GanymedEngine/extern/Lua.lua"
group ""
```

`GanymedEngine/premake5.lua`:

```lua
includedirs
{
	-- ...existing...
	"%{IncludeDir.lua}",
	"%{IncludeDir.sol2}"
}

links
{
	-- ...existing...
	"Lua"
}

defines
{
	-- ...existing...
	-- Bounds/type checks on every sol2 call. Costs a little perf,
	-- turns script bugs into readable errors instead of crashes.
	"SOL_ALL_SAFETIES_ON=1"
}
```

> **MSVC note:** sol2 needs exceptions (on by default) and C++17 (already set).
> `staticruntime "off"` matches the rest of the workspace — keep it consistent or
> you'll get LNK4098/2038 runtime-mismatch errors.

### 2.4 `ScriptComponent` and the Script asset type

Add to `Components.h` (POD only — no sol2 include here):

```cpp
struct ScriptComponent
{
	// Path relative to assets/, e.g. "scripts/Player.lua"
	std::string ScriptPath;

	ScriptComponent() = default;
	ScriptComponent(const ScriptComponent&) = default;
};
```

Extend the asset system (`AssetTypes.h` / `.cpp`):

```cpp
enum class AssetType : uint16_t
{
	None = 0,
	StaticMesh,
	Environment,
	Texture,
	Material,
	Scene,
	Script          // NEW
};

// In AssetTypeFromExtension():
if (extension == ".lua") return AssetType::Script;
```

This makes `.lua` files show up in the ContentBrowser with proper typing, and later
lets you drag-drop them onto a `ScriptComponent` field.

### 2.5 The `ScriptEngine`

New files: `GanymedEngine/source/GanymedE/Scripting/ScriptEngine.h/.cpp`.

**Header:**

```cpp
#pragma once

#include "GanymedE/Core/Timestep.h"
#include "GanymedE/Core/UUID.h"

namespace GanymedE {

	class Scene;
	class Entity;

	class ScriptEngine
	{
	public:
		static void Init();       // create sol::state, open libs, register bindings
		static void Shutdown();

		// Scene lifecycle (mirrors the NativeScriptComponent flow in Scene.cpp)
		static void OnRuntimeStart(Scene* scene);
		static void OnRuntimeStop();
		static void OnUpdate(Timestep ts);

		static void OnCollisionEnter(Entity entity, Entity other);
		static void OnCollisionExit(Entity entity, Entity other);

		// Called when an entity with a ScriptComponent is created during play
		static void InstantiateEntity(Entity entity);
		static void DestroyEntityInstance(UUID entityID);

		// Returns the raw lua_State, needed to initialise the RmlUi Lua plugin
		static void* GetLuaState();
	};
}
```

**Implementation** (the important parts):

```cpp
#include "gepch.h"
#include "ScriptEngine.h"

#include "GanymedE/Scene/Scene.h"
#include "GanymedE/Scene/Entity.h"
#include "GanymedE/Scene/Components.h"
#include "GanymedE/Assets/AssetPaths.h"

#include <sol/sol.hpp>

namespace GanymedE {

	struct ScriptEngineData
	{
		sol::state Lua;
		Scene* SceneContext = nullptr;

		// Loaded script "classes" keyed by script path (chunk executed once)
		std::unordered_map<std::string, sol::table> ScriptClasses;
		// Per-entity instance tables keyed by entity UUID
		std::unordered_map<UUID, sol::table> EntityInstances;
	};

	static ScriptEngineData* s_Data = nullptr;

	static void RegisterBindings(sol::state& lua);   // section 2.6

	void ScriptEngine::Init()
	{
		s_Data = new ScriptEngineData();
		s_Data->Lua.open_libraries(
			sol::lib::base, sol::lib::math, sol::lib::string,
			sol::lib::table, sol::lib::package);
		RegisterBindings(s_Data->Lua);
	}

	void ScriptEngine::Shutdown()
	{
		delete s_Data;
		s_Data = nullptr;
	}

	static sol::table LoadScriptClass(const std::string& relativePath)
	{
		auto it = s_Data->ScriptClasses.find(relativePath);
		if (it != s_Data->ScriptClasses.end())
			return it->second;

		std::filesystem::path fullPath = AssetPaths::GetAssetDirectory() / relativePath;
		sol::protected_function_result result =
			s_Data->Lua.safe_script_file(fullPath.string(), sol::script_pass_on_error);

		if (!result.valid())
		{
			sol::error err = result;
			GE_CORE_ERROR("[ScriptEngine] Failed to load '{}': {}", relativePath, err.what());
			return sol::table{};
		}

		sol::table scriptClass = result;

		// TypeScriptToLua emits `export default X` as ____exports.default = X.
		// Accept both plain `return X` (hand-written Lua) and TSTL default exports.
		if (scriptClass["default"].valid())
			scriptClass = scriptClass["default"];

		s_Data->ScriptClasses[relativePath] = scriptClass;
		return scriptClass;
	}

	void ScriptEngine::InstantiateEntity(Entity entity)
	{
		auto& sc = entity.GetComponent<ScriptComponent>();
		if (sc.ScriptPath.empty())
			return;

		sol::table scriptClass = LoadScriptClass(sc.ScriptPath);
		if (!scriptClass.valid())
			return;

		// Instance = fresh table delegating method lookup to the class table.
		// TSTL classes keep methods on .prototype; object literals are the table itself.
		sol::table indexTarget = scriptClass;
		if (scriptClass["prototype"].valid())
			indexTarget = scriptClass["prototype"];

		sol::state& lua = s_Data->Lua;
		sol::table instance = lua.create_table();
		sol::table mt = lua.create_table();
		mt["__index"] = indexTarget;
		instance[sol::metatable_key] = mt;
		instance["entity"] = entity;

		s_Data->EntityInstances[entity.GetUUID()] = instance;

		sol::protected_function onCreate = instance["OnCreate"];
		if (onCreate.valid())
		{
			auto r = onCreate(instance);
			if (!r.valid())
			{
				sol::error err = r;
				GE_CORE_ERROR("[ScriptEngine] OnCreate ({}): {}", sc.ScriptPath, err.what());
			}
		}
	}

	void ScriptEngine::OnRuntimeStart(Scene* scene)
	{
		s_Data->SceneContext = scene;

		auto view = scene->Reg().view<ScriptComponent>();
		for (auto e : view)
			InstantiateEntity(Entity{ e, scene });
	}

	void ScriptEngine::OnUpdate(Timestep ts)
	{
		for (auto& [uuid, instance] : s_Data->EntityInstances)
		{
			sol::protected_function onUpdate = instance["OnUpdate"];
			if (!onUpdate.valid())
				continue;

			auto r = onUpdate(instance, (float)ts);
			if (!r.valid())
			{
				sol::error err = r;
				GE_CORE_ERROR("[ScriptEngine] OnUpdate: {}", err.what());
			}
		}
	}

	void ScriptEngine::OnRuntimeStop()
	{
		for (auto& [uuid, instance] : s_Data->EntityInstances)
		{
			sol::protected_function onDestroy = instance["OnDestroy"];
			if (onDestroy.valid())
				onDestroy(instance);
		}
		s_Data->EntityInstances.clear();
		s_Data->ScriptClasses.clear();   // forces a fresh load on next play (free hot reload)
		s_Data->SceneContext = nullptr;
		s_Data->Lua.collect_garbage();
	}

	// OnCollisionEnter/Exit follow the same instance-lookup + protected-call pattern.
}
```

Notes:

- `safe_script_file` + `protected_function` everywhere: **a script error must log,
  never crash or throw across the C++ boundary.**
- Clearing `ScriptClasses` in `OnRuntimeStop` means every editor Play press re-reads
  scripts from disk — the simplest hot-reload that exists. (Section 2.9 covers
  live-reload during play.)
- `GetLuaState()` returns `s_Data->Lua.lua_state()` — needed later for RmlUi.

### 2.6 Bindings (sol2 usertypes)

Keep all bindings in one file, e.g. `Scripting/ScriptBindings.cpp`. The minimum
useful set:

```cpp
static void RegisterBindings(sol::state& lua)
{
	// ---- glm::vec3 ----
	lua.new_usertype<glm::vec3>("Vec3",
		sol::call_constructor, sol::constructors<glm::vec3(), glm::vec3(float), glm::vec3(float, float, float)>(),
		"x", &glm::vec3::x,
		"y", &glm::vec3::y,
		"z", &glm::vec3::z,
		sol::meta_function::addition,       [](const glm::vec3& a, const glm::vec3& b) { return a + b; },
		sol::meta_function::subtraction,    [](const glm::vec3& a, const glm::vec3& b) { return a - b; },
		sol::meta_function::multiplication, sol::overload(
			[](const glm::vec3& v, float s) { return v * s; },
			[](float s, const glm::vec3& v) { return s * v; }),
		"Length",    [](const glm::vec3& v) { return glm::length(v); },
		"Normalized",[](const glm::vec3& v) { return glm::normalize(v); }
	);

	// ---- TransformComponent ----
	lua.new_usertype<TransformComponent>("TransformComponent",
		"Translation", &TransformComponent::Translation,
		"Rotation",    &TransformComponent::Rotation,
		"Scale",       &TransformComponent::Scale
	);

	// ---- Entity ----
	lua.new_usertype<Entity>("Entity",
		"GetName",      &Entity::GetName,
		"GetUUID",      [](Entity& e) { return (uint64_t)e.GetUUID(); },
		"GetTransform", [](Entity& e) -> TransformComponent& {
			return e.GetComponent<TransformComponent>();
		},
		"HasRigidBody", [](Entity& e) { return e.HasComponent<RigidBodyComponent>(); },
		sol::meta_function::equal_to, [](const Entity& a, const Entity& b) { return a == b; }
	);

	// ---- Input (static) ----
	auto input = lua.create_named_table("Input");
	input["IsKeyPressed"]         = [](int key)    { return Input::IsKeyPressed((KeyCode)key); };
	input["IsMouseButtonPressed"] = [](int button) { return Input::IsMouseButtonPressed((MouseCode)button); };
	input["GetMousePosition"]     = []() { auto p = Input::GetMousePosition(); return std::make_tuple(p.x, p.y); };

	// ---- Key codes (mirror KeyCodes.h; generate the full table once) ----
	auto key = lua.create_named_table("Key");
	key["W"] = (int)Key::W;  key["A"] = (int)Key::A;
	key["S"] = (int)Key::S;  key["D"] = (int)Key::D;
	key["Space"] = (int)Key::Space;
	// ... etc.

	// ---- Logging (routed to the CLIENT logger, not core) ----
	auto log = lua.create_named_table("Log");
	log["Trace"] = [](const std::string& m) { GE_TRACE("{}", m); };
	log["Info"]  = [](const std::string& m) { GE_INFO("{}", m); };
	log["Warn"]  = [](const std::string& m) { GE_WARN("{}", m); };
	log["Error"] = [](const std::string& m) { GE_ERROR("{}", m); };

	// ---- Scene access ----
	auto scene = lua.create_named_table("Scene");
	scene["FindEntityByName"] = [](const std::string& name) -> sol::object { /* iterate TagComponent view */ };
}
```

**Important sol2/EnTT gotcha:** `GetTransform` returns a reference into EnTT's
component storage. sol2 hands class-type members and reference returns to Lua as
references (writes go through — `transform.Translation.x = 5` works), **but the
reference can dangle if the registry reallocates that component pool** (i.e., if any
entity gets a `TransformComponent` added while the script holds the reference).
Rules for script authors, enforce by convention:

- Fetch components fresh each frame — never store them in `self` across frames.
- Don't create entities/components while holding a component reference.

Later, physics-facing bindings (`SetLinearVelocity`, `ApplyImpulse`, raycasts) should
go through `PhysicsScene` rather than writing transforms directly, so kinematic and
dynamic bodies behave correctly.

### 2.7 Scene lifecycle wiring

`Application` (or `EntryPoint`): call `ScriptEngine::Init()` after logging init, and
`ScriptEngine::Shutdown()` on exit.

`Scene.cpp` — mirror the existing `NativeScriptComponent` flow:

```cpp
void Scene::OnRuntimeStart()
{
	InstantiateScripts();               // native scripts (existing)
	ScriptEngine::OnRuntimeStart(this); // NEW: lua scripts

	m_PhysicsScene = CreateScope<PhysicsScene>();
	m_PhysicsScene->Start(this);
	m_PhysicsAccumulator = 0.0f;
}

void Scene::OnRuntimeStop()
{
	// ...existing physics + native script teardown...
	ScriptEngine::OnRuntimeStop();      // NEW
}

void Scene::OnUpdateRuntime(Timestep ts, EditorCamera* fallbackCamera)
{
	// ...existing fixed-step physics loop...

	// Update scripts
	{
		// ...existing native script update...
		ScriptEngine::OnUpdate(ts);     // NEW
	}
	// ...
}

void Scene::DispatchCollisionEvents()
{
	// inside the existing notify lambda, alongside the NativeScriptComponent path:
	if (self.HasComponent<ScriptComponent>())
	{
		if (event.Entered) ScriptEngine::OnCollisionEnter(self, other);
		else               ScriptEngine::OnCollisionExit(self, other);
	}
}
```

Also add `CopyComponent<ScriptComponent>(...)` to `Scene::Copy` — the editor's
play-mode scene copy must carry script paths across (instances are per-run state and
live in `ScriptEngine`, so nothing else to reset).

### 2.8 Serialization + editor UI

**SceneSerializer** — same YAML pattern as every other component:

```cpp
// Serialize
if (entity.HasComponent<ScriptComponent>())
{
	auto& sc = entity.GetComponent<ScriptComponent>();
	out << YAML::Key << "ScriptComponent" << YAML::BeginMap;
	out << YAML::Key << "ScriptPath" << YAML::Value << sc.ScriptPath;
	out << YAML::EndMap;
}

// Deserialize
if (auto scriptComponent = entity["ScriptComponent"])
{
	auto& sc = deserializedEntity.AddComponent<ScriptComponent>();
	sc.ScriptPath = scriptComponent["ScriptPath"].as<std::string>();
}
```

**SceneHierarchyPanel** — add `ScriptComponent` to the Add Component menu and a
`DrawComponent<ScriptComponent>` block: a read-only text field showing the path plus
a drag-drop target accepting ContentBrowser payloads whose extension is `.lua`
(the ContentBrowser already emits path payloads for scene/mesh drops — reuse that
payload type and filter by extension, like the `.glb` handling in `EditorLayer`).

### 2.9 Script conventions + example

A gameplay script is a Lua module returning a table of lifecycle methods:

```lua
-- assets/scripts/Player.lua
local Player = {}

function Player:OnCreate()
	self.speed = 5.0
	Log.Info("Player created: " .. self.entity:GetName())
end

function Player:OnUpdate(ts)
	local transform = self.entity:GetTransform()

	if Input.IsKeyPressed(Key.W) then
		transform.Translation.z = transform.Translation.z - self.speed * ts
	end
	if Input.IsKeyPressed(Key.S) then
		transform.Translation.z = transform.Translation.z + self.speed * ts
	end
end

function Player:OnCollisionEnter(other)
	Log.Warn("Hit " .. other:GetName())
end

function Player:OnDestroy()
end

return Player
```

**Hot reload during play** (optional, later): store each script's
`filesystem::last_write_time` at load; once per second, re-check; on change, re-run
the chunk, replace the class table, and re-point each live instance's metatable
`__index` at the new table. Instance state (fields on `self`) survives; only methods
are swapped. Log and keep the old table if the new chunk has a syntax error.

---

## 3. Part 2 — TypeScriptToLua (TS authoring layer)

Nothing in this section touches C++ — it's tooling that emits `.lua` files into the
assets folder. (One exception was already handled: the `default` export unwrap in
`LoadScriptClass`.)

### 3.1 Project layout

Keep the TS project *outside* the assets folder so only compiled output ships:

```
GanymedEditor/
├── assets/
│   └── scripts/            ← tstl output (.lua) — what the engine loads
└── scripts-src/            ← TypeScript project (not scanned by AssetManager)
    ├── package.json
    ├── tsconfig.json
    ├── types/
    │   └── ganymed.d.ts    ← engine API declarations
    └── Player.ts
```

```bash
cd GanymedEditor/scripts-src
npm init -y
npm install --save-dev typescript typescript-to-lua lua-types
```

### 3.2 `tsconfig.json`

```jsonc
{
	"compilerOptions": {
		"target": "ESNext",
		"lib": ["ESNext"],
		"moduleResolution": "node",
		"strict": true,
		"types": ["lua-types/5.4"],
		"outDir": "../assets/scripts"
	},
	"tstl": {
		"luaTarget": "5.4",
		// Inline the TS helper lib into each file — every emitted .lua is
		// self-contained, so the C++ loader needs no require() path setup.
		"luaLibImport": "inline",
		"noImplicitSelf": true
	},
	"include": ["**/*.ts", "types/**/*.d.ts"]
}
```

`package.json` scripts:

```json
{
	"scripts": {
		"build": "tstl",
		"watch": "tstl --watch"
	}
}
```

Run `npm run watch` while working — every save recompiles to
`assets/scripts/*.lua`. (Later, the editor can spawn this process itself when a
project opens.)

### 3.3 Engine API declarations — `types/ganymed.d.ts`

This file is the TS mirror of `ScriptBindings.cpp`. Keep them in sync — it's the
contract that gives you IntelliSense and compile errors against the real engine API.

```typescript
/** @noSelfInFile */

declare interface Vec3 {
	x: number;
	y: number;
	z: number;
	Length(): number;
	Normalized(): Vec3;
}

declare interface TransformComponent {
	Translation: Vec3;
	Rotation: Vec3;
	Scale: Vec3;
}

declare interface Entity {
	GetName(): string;
	GetUUID(): number;
	GetTransform(): TransformComponent;
	HasRigidBody(): boolean;
}

/** Base shape every gameplay script implements. */
declare interface Script {
	entity: Entity;
	OnCreate?(): void;
	OnUpdate?(ts: number): void;
	OnCollisionEnter?(other: Entity): void;
	OnCollisionExit?(other: Entity): void;
	OnDestroy?(): void;
}

declare namespace Input {
	function IsKeyPressed(key: number): boolean;
	function IsMouseButtonPressed(button: number): boolean;
	/** Returns [x, y] as a LuaMultiReturn. */
	function GetMousePosition(): LuaMultiReturn<[number, number]>;
}

declare namespace Key {
	const W: number; const A: number; const S: number; const D: number;
	const Space: number;
	// ...mirror the bound Key table
}

declare namespace Log {
	function Trace(msg: string): void;
	function Info(msg: string): void;
	function Warn(msg: string): void;
	function Error(msg: string): void;
}
```

`/** @noSelfInFile */` and `noImplicitSelf` stop TSTL from inserting `self`
parameters on these *static* API calls (`Input.IsKeyPressed(...)` must compile to a
`.` call, not a `:` call).

### 3.4 Script pattern in TS

**Use object literals, not `class`.** The C++ loader instantiates via
`setmetatable({}, {__index = ...})`, so an object literal (methods on the table
itself) is the zero-surprise path. TSTL classes work through the `prototype`
fallback in `LoadScriptClass`, but their constructors never run — a foot-gun better
avoided by convention.

```typescript
// scripts-src/Player.ts
const Player: Script = {
	entity: undefined!,   // injected by ScriptEngine before OnCreate
	speed: 5.0,

	OnCreate() {
		Log.Info(`Player created: ${this.entity.GetName()}`);
	},

	OnUpdate(ts: number) {
		const transform = this.entity.GetTransform();
		if (Input.IsKeyPressed(Key.W)) transform.Translation.z -= this.speed * ts;
		if (Input.IsKeyPressed(Key.S)) transform.Translation.z += this.speed * ts;
	},

	OnCollisionEnter(other: Entity) {
		Log.Warn(`Hit ${other.GetName()}`);
	},
} as Script & { speed: number };

export default Player;
```

Compiles to a self-contained `assets/scripts/Player.lua` whose exports table has a
`default` field — exactly what `LoadScriptClass` unwraps.

### 3.5 Limitations to know

- **No npm runtime packages** unless they're TSTL-compatible (pure TS compiled by
  tstl, or Lua packages with declaration files). No Node APIs, no DOM.
- Debugging happens at the Lua level. tstl can emit inline source maps
  (`"sourceMapTraceback": true`) so Lua error tracebacks point at `.ts` lines —
  worth enabling from day one.
- `LuaMultiReturn`, `$range`, and other TSTL language extensions are documented at
  typescripttolua.github.io — needed occasionally when a binding returns multiple
  values.

---

## 4. Part 3 — RmlUi (HTML/CSS UI)

### 4.1 Add submodules

```bash
git submodule add https://github.com/mikke89/RmlUi GanymedEngine/extern/RmlUi
cd GanymedEngine/extern/RmlUi && git checkout 6.1 && cd -    # latest release tag

git submodule add https://gitlab.freedesktop.org/freetype/freetype GanymedEngine/extern/freetype
cd GanymedEngine/extern/freetype && git checkout VER-2-13-3 && cd -
```

FreeType is RmlUi's one hard dependency (its default font engine). It builds cleanly
as a static lib from a known minimal file list (below).

### 4.2 Premake build scripts

`GanymedEngine/extern/FreeType.lua`:

```lua
project "FreeType"
	kind "StaticLib"
	language "C"
	staticruntime "off"
	warnings "Off"

	targetdir ("freetype/bin/" .. outputdir .. "/%{prj.name}")
	objdir ("freetype/bin-int/" .. outputdir .. "/%{prj.name}")

	-- Canonical minimal FreeType build (one .c per module; each includes the rest)
	files
	{
		"freetype/src/autofit/autofit.c",
		"freetype/src/base/ftbase.c",
		"freetype/src/base/ftbbox.c",
		"freetype/src/base/ftbdf.c",
		"freetype/src/base/ftbitmap.c",
		"freetype/src/base/ftcid.c",
		"freetype/src/base/ftdebug.c",
		"freetype/src/base/ftfstype.c",
		"freetype/src/base/ftgasp.c",
		"freetype/src/base/ftglyph.c",
		"freetype/src/base/ftgxval.c",
		"freetype/src/base/ftinit.c",
		"freetype/src/base/ftmm.c",
		"freetype/src/base/ftotval.c",
		"freetype/src/base/ftpatent.c",
		"freetype/src/base/ftpfr.c",
		"freetype/src/base/ftstroke.c",
		"freetype/src/base/ftsynth.c",
		"freetype/src/base/ftsystem.c",
		"freetype/src/base/fttype1.c",
		"freetype/src/base/ftwinfnt.c",
		"freetype/src/bdf/bdf.c",
		"freetype/src/cache/ftcache.c",
		"freetype/src/cff/cff.c",
		"freetype/src/cid/type1cid.c",
		"freetype/src/gzip/ftgzip.c",
		"freetype/src/lzw/ftlzw.c",
		"freetype/src/pcf/pcf.c",
		"freetype/src/pfr/pfr.c",
		"freetype/src/psaux/psaux.c",
		"freetype/src/pshinter/pshinter.c",
		"freetype/src/psnames/psnames.c",
		"freetype/src/raster/raster.c",
		"freetype/src/sdf/sdf.c",
		"freetype/src/sfnt/sfnt.c",
		"freetype/src/smooth/smooth.c",
		"freetype/src/svg/svg.c",
		"freetype/src/truetype/truetype.c",
		"freetype/src/type1/type1.c",
		"freetype/src/type42/type42.c",
		"freetype/src/winfonts/winfnt.c"
	}

	includedirs { "freetype/include" }
	defines { "FT2_BUILD_LIBRARY" }

	filter "system:windows"
		systemversion "latest"

	filter "system:linux"
		pic "On"
		systemversion "latest"

	filter "configurations:Debug"
		runtime "Debug"
		symbols "on"

	filter "configurations:Release or configurations:Dist"
		runtime "Release"
		optimize "on"
```

`GanymedEngine/extern/RmlUi.lua`:

```lua
project "RmlUi"
	kind "StaticLib"
	language "C++"
	cppdialect "C++17"
	staticruntime "off"
	warnings "Off"

	targetdir ("RmlUi/bin/" .. outputdir .. "/%{prj.name}")
	objdir ("RmlUi/bin-int/" .. outputdir .. "/%{prj.name}")

	files
	{
		"RmlUi/Source/Core/**.cpp",
		"RmlUi/Source/Core/**.h",
		-- Visual document inspector (Ctrl+Shift toggle); worth having in Debug
		"RmlUi/Source/Debugger/**.cpp",
		-- Lua plugin — runs UI logic on the shared Lua state
		"RmlUi/Source/Lua/**.cpp",
		"RmlUi/Include/RmlUi/**.h"
	}

	includedirs
	{
		"RmlUi/Include",
		"freetype/include",
		"lua"                       -- the Lua submodule, for the Lua plugin
	}

	defines
	{
		"RMLUI_STATIC_LIB"
	}

	links { "FreeType", "Lua" }

	filter "system:windows"
		systemversion "latest"
		buildoptions { "/utf-8" }

	filter "system:linux"
		pic "On"
		systemversion "latest"

	filter "configurations:Debug"
		runtime "Debug"
		symbols "on"

	filter "configurations:Release or configurations:Dist"
		runtime "Release"
		optimize "on"
		defines { "NDEBUG" }
```

Wire both into the root `premake5.lua` (`IncludeDir["RmlUi"]`, dependency group
includes) and add `"RmlUi"`, `"FreeType"` to GanymedEngine's `links`, plus
`RMLUI_STATIC_LIB` to its `defines` (consumers need it too, like the Jolt defines).

> If the exact file list drifts on a newer RmlUi/FreeType tag, trust the compiler:
> missing-symbol link errors name the module to add. Check each tag's CMakeLists for
> the authoritative list.

### 4.3 Backend files (renderer + platform glue)

RmlUi deliberately doesn't render or read input itself — you provide a
`RenderInterface` and `SystemInterface`. The repo ships MIT-licensed reference
backends **designed to be copied into your project**:

Copy from `GanymedEngine/extern/RmlUi/Backends/` into
`GanymedEngine/source/Platform/RmlUi/`:

- `RmlUi_Renderer_GL3.h/.cpp` — OpenGL 3.3+ renderer (your GL 4.1 core context is fine)
- `RmlUi_Platform_GLFW.h/.cpp` — GLFW input translation + system interface

The GL3 backend bundles its own GL loader by default, which would collide with your
Glad. It supports an override — add to GanymedEngine's `defines`:

```lua
'RMLUI_GL3_CUSTOM_LOADER=<glad/glad.h>'
```

(Verify the macro spelling in the `RmlUi_Renderer_GL3.h` you copy — it's documented
at the top of the file.)

### 4.4 `UIEngine`

New files: `GanymedEngine/source/GanymedE/UI/UIEngine.h/.cpp`. Same singleton shape
as `ScriptEngine`:

```cpp
#pragma once

#include "GanymedE/Core/Timestep.h"
#include "GanymedE/events/Event.h"

#include <filesystem>

namespace Rml { class Context; class ElementDocument; }

namespace GanymedE {

	class UIEngine
	{
	public:
		static void Init(uint32_t width, uint32_t height); // after renderer init
		static void Shutdown();

		static Rml::ElementDocument* LoadDocument(const std::filesystem::path& rmlPath);
		static void CloseAllDocuments();

		static void OnUpdate(Timestep ts);
		static void OnRender();                     // call with target framebuffer bound
		static void OnEvent(Event& e);              // engine events → Rml input
		static void SetViewport(uint32_t width, uint32_t height);

		static Rml::Context* GetContext();
	};
}
```

Implementation sketch:

```cpp
#include <RmlUi/Core.h>
#include <RmlUi/Debugger.h>
#include <RmlUi/Lua.h>

#include "Platform/RmlUi/RmlUi_Renderer_GL3.h"
#include "Platform/RmlUi/RmlUi_Platform_GLFW.h"
#include "GanymedE/Scripting/ScriptEngine.h"

namespace GanymedE {

	struct UIEngineData
	{
		SystemInterface_GLFW SystemInterface;
		RenderInterface_GL3  RenderInterface;
		Rml::Context* Context = nullptr;
	};
	static UIEngineData* s_Data = nullptr;

	void UIEngine::Init(uint32_t width, uint32_t height)
	{
		s_Data = new UIEngineData();

		Rml::SetSystemInterface(&s_Data->SystemInterface);
		Rml::SetRenderInterface(&s_Data->RenderInterface);
		Rml::Initialise();

		// Share the gameplay VM — UI scripts and gameplay scripts see the same globals
		Rml::Lua::Initialise((lua_State*)ScriptEngine::GetLuaState());

		s_Data->Context = Rml::CreateContext("main", Rml::Vector2i(width, height));

	#ifdef GE_DEBUG
		Rml::Debugger::Initialise(s_Data->Context);   // toggle with Rml::Debugger::SetVisible
	#endif

		// The editor assets dir already has Montserrat — perfect UI font
		Rml::LoadFontFace("assets/fonts/montserrat/Montserrat-Regular.ttf");
		Rml::LoadFontFace("assets/fonts/montserrat/Montserrat-Bold.ttf");
	}

	void UIEngine::OnUpdate(Timestep) { s_Data->Context->Update(); }

	void UIEngine::OnRender()
	{
		s_Data->RenderInterface.BeginFrame();
		s_Data->Context->Render();
		s_Data->RenderInterface.EndFrame();
		// NOTE: GL3 backend changes blend/scissor/program state.
		// Re-assert engine GL state here if the next pass assumes it.
	}

	void UIEngine::Shutdown()
	{
		Rml::Shutdown();          // must run BEFORE ScriptEngine::Shutdown()
		delete s_Data; s_Data = nullptr;
	}
}
```

**Init/shutdown order matters:**
`ScriptEngine::Init` → `UIEngine::Init` … `UIEngine::Shutdown` → `ScriptEngine::Shutdown`
(RmlUi's Lua plugin holds references into the shared `lua_State`).

### 4.5 Frame-loop and editor integration

Where things go each frame (Play state):

1. `Scene::OnUpdateRuntime` — gameplay scripts may show/hide documents, set data-model values
2. `UIEngine::OnUpdate` — RmlUi layouts/animates
3. Scene 3D render + post-processing (tonemap/FXAA) into the viewport framebuffer
4. `UIEngine::OnRender` — **after** tonemapping with the same framebuffer still
   bound, so UI is composited in final display space (not tonemapped as if it were
   scene HDR), and it appears inside the editor's viewport ImGui image automatically
5. ImGui (editor chrome) renders on top as before

In `EditorLayer`:

- `OnScenePlay` → load the scene's UI documents (start with a hard-coded
  `assets/ui/hud.rml`; later make it a scene property); `OnSceneStop` →
  `UIEngine::CloseAllDocuments()`.
- Viewport resize → `UIEngine::SetViewport(w, h)` alongside the existing
  `OnViewportResize`.
- `OnEvent` in Play state → forward to `UIEngine::OnEvent` **before** the camera/
  ImGui handlers, and only when the viewport is hovered/focused. Mouse coordinates
  must be translated to viewport space first:
  `mouse - m_ViewportBounds[0]` (the same math the pixel-picking / gizmo code uses).
- The event translation itself is a `switch` mapping your `MouseMovedEvent`,
  `MouseButtonPressed/ReleasedEvent`, `MouseScrolledEvent`, `KeyPressed/Released`,
  `KeyTyped` events to `Context::ProcessMouseMove / ProcessMouseButtonDown / ...`.
  Copy the key mapping table from `RmlUi_Platform_GLFW.cpp` (your `KeyCodes.h` uses
  GLFW keycodes, so it maps 1:1).
- If an element consumed the event (`Context::Process*` returns false when RmlUi
  wants it), mark the engine event `Handled` so gameplay input doesn't also fire.

In a shipped game (Sandbox app), the same calls run against the default framebuffer —
no coordinate translation needed.

### 4.6 Example HUD

`assets/ui/hud.rml`:

```html
<rml>
<head>
	<link type="text/rcss" href="hud.rcss"/>
</head>
<body data-model="hud">
	<div id="health-bar">
		<div id="health-fill" data-style-width="health + '%'"/>
		<span id="health-text">{{health}} / 100</span>
	</div>
	<div id="score">Score: {{score}}</div>
</body>
</rml>
```

`assets/ui/hud.rcss`:

```css
body {
	font-family: Montserrat;
	font-size: 18dp;
	color: white;
	width: 100%;
	height: 100%;
}

#health-bar {
	position: absolute;
	bottom: 30dp; left: 30dp;
	width: 300dp; height: 24dp;
	background-color: #00000080;
	border-radius: 4dp;
}

#health-fill {
	height: 100%;
	background-color: #c0392b;
	border-radius: 4dp;
	transition: width 0.2s cubic-out;
}

#score {
	position: absolute;
	top: 30dp; right: 30dp;
	font-weight: bold;
}
```

Data model registration (C++, once at document setup — or bind it from Lua):

```cpp
Rml::DataModelConstructor ctor = context->CreateDataModel("hud");
ctor.Bind("health", &s_HudData.Health);   // float
ctor.Bind("score",  &s_HudData.Score);    // int
// After changing values: s_HudModel.DirtyVariable("health");
```

Because the Lua plugin shares the gameplay VM, a gameplay script (written in TS,
compiled to Lua) can also drive the UI directly — RmlUi exposes `rmlui.contexts`,
document lookup, element manipulation, and event listeners to Lua, and `<script>`
blocks inside `.rml` documents execute on the shared state. Declare the small part
of that API you use in `ganymed.d.ts` and your UI logic gets type-checked too.

---

## 5. Suggested implementation order

Each milestone compiles, runs, and is independently verifiable:

1. **Lua static lib builds.** Submodules added, `Lua.lua` written, workspace links.
   Smoke test: `lua.script("print('hello')")` in `Application` startup, then delete.
2. **ScriptEngine + ScriptComponent, no editor UI.** Bindings for Vec3/Transform/
   Entity/Input/Log. Hand-add a `ScriptComponent` in `Scene` setup code, press Play
   in the editor, watch a cube move via `Player.lua`.
3. **Editor + serialization.** SceneHierarchyPanel add/inspect, SceneSerializer,
   ContentBrowser drag-drop, `Scene::Copy`. Script assignment survives save/load.
4. **TSTL toolchain.** `scripts-src/` project, `ganymed.d.ts`, port `Player.lua` to
   `Player.ts`, confirm the compiled output behaves identically.
5. **RmlUi builds + renders.** FreeType/RmlUi libs, backends copied, `UIEngine`,
   static `hud.rml` visible in the Play-state viewport. Debugger toggle works.
6. **UI input + data binding.** Event routing with viewport coordinate translation;
   health bar driven from a gameplay script.
7. **Polish.** Lua plugin UI scripting from TS, hot reload during play, collision
   callbacks, physics bindings.

Milestones 1–3 are the core value; 4–7 can each ship independently.

---

## 6. Pitfalls & gotchas

| # | Pitfall | Mitigation |
|---|---------|------------|
| 1 | **EnTT reference invalidation** — component refs held in Lua dangle if a pool reallocates | Convention: fetch components each frame; never cache in `self`. Long-term: bind by UUID + lookup |
| 2 | **sol2 compile times** — heavy templates | Keep ALL sol2 includes inside `Scripting/*.cpp`; never include `sol.hpp` in a header |
| 3 | **Errors crossing the C++ boundary** | `SOL_ALL_SAFETIES_ON=1`, `safe_script_file`, `protected_function` everywhere. A bad script logs and disables itself, never crashes the editor |
| 4 | **Runtime library mismatch** (LNK2038) | Every new premake project uses `staticruntime "off"` like the rest of the workspace |
| 5 | **GL state leakage** — RmlUi's GL3 backend changes blend/scissor/shader state | Re-assert engine GL state after `UIEngine::OnRender`; keep the call between well-defined passes |
| 6 | **Two GL loaders** — RmlUi GL3 backend bundles its own | `RMLUI_GL3_CUSTOM_LOADER=<glad/glad.h>` define; verify macro name in the copied header |
| 7 | **UI rendered in HDR space** — tonemap washing out UI colors | Render UI after tonemapping, into the final LDR target |
| 8 | **Editor mouse coordinates** — RmlUi expects viewport-local pixels | Translate with `m_ViewportBounds[0]` exactly like existing picking code |
| 9 | **Shutdown order** — Rml Lua plugin refs into shared `lua_State` | `Rml::Shutdown()` strictly before `ScriptEngine::Shutdown()` |
| 10 | **TSTL classes** — methods live on `.prototype`, constructors don't run | Prefer object-literal scripts; the loader's `prototype` fallback covers stragglers |
| 11 | **Scene::Copy** — play-mode copy must carry `ScriptComponent` | Add `CopyComponent<ScriptComponent>`; instances live in ScriptEngine and reset on stop |
| 12 | **RmlUi is not a browser** — ~XHTML + CSS2 with much of CSS3 (flexbox, animations, transforms, transitions) | Check the RCSS docs before assuming a CSS feature exists; the Debugger shows computed properties |

---

## 7. References

- Lua 5.4 manual — https://www.lua.org/manual/5.4/
- Lua source mirror — https://github.com/lua/lua
- sol2 docs — https://sol2.readthedocs.io/
- TypeScriptToLua — https://typescripttolua.github.io/ (see *The Basics*, *Advanced → Language Extensions*, *Writing Declarations*)
- ts-defold (reference for a TSTL + engine `.d.ts` setup) — https://ts-defold.dev/
- RmlUi docs — https://mikke89.github.io/RmlUiDoc/ (RML/RCSS reference, data bindings, Lua plugin, C++ integration)
- RmlUi repo & backends — https://github.com/mikke89/RmlUi
- FreeType — https://freetype.org/

# Scripting & UI Integration Guide

**Lua 5.4 + sol2 (scripting) · TypeScriptToLua (TS authoring) · RmlUi (HTML/CSS UI)**

This document describes how to integrate a complete gameplay scripting and UI stack into
GanymedEngine, following the engine's conventions: premake5 workspace, vendored source
dependencies as static libs, the view/access-wrapper ECS, and the bgfx renderer.

> **Revised 2026-07-19.** The original version of this plan predated two engine-wide
> refactors and was invalidated by both:
>
> - the **ECS views refactor** ([`ECS_VIEWS_IMPLEMENTATION_GUIDE.md`](ECS_VIEWS_IMPLEMENTATION_GUIDE.md))
>   deleted every `Scene::OnUpdateRuntime`-era insertion point this doc used to name, moved
>   collision dispatch into `PhysicsSystem`, made `TransformComponent` change-tracked (a script
>   writing it directly would *silently not move anything on screen*), and made immediate
>   structural changes illegal during system updates;
> - the **bgfx migration** ([`BGFX_MIGRATION.md`](BGFX_MIGRATION.md)) removed OpenGL, Glad and
>   framebuffer bind/unbind entirely, so RmlUi's reference GL3 backend cannot even initialise —
>   a custom bgfx render backend is required (the in-tree ImGui backend is the template).
>
> This revision folds all of that in. Where it deviates from the original plan, the reason is
> stated inline. The stack choice itself (Lua 5.4 + sol2, TSTL, RmlUi, one shared VM) was
> re-evaluated and stands.

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
                               │ registered as Script assets
┌──────────────────────────────▼──────────────────────────────────┐
│  Engine (C++)                                                   │
│                                                                 │
│  LuaScriptSystem (ECS::System, registered in Scene ctor)        │
│    • InitView  → scripts that appeared since last update        │
│    • FiniView  → scripts that went away (incl. entity death)    │
│    • IterView  → per-frame OnUpdate                             │
│    • mirrors NativeScriptSystem exactly                         │
│                                                                 │
│  ScriptEngine (sol2 → Lua 5.4 VM)                               │
│    • one sol::state for the whole runtime                       │
│    • class tables per script asset, instance tables per entity  │
│    • lifecycle: OnCreate / OnUpdate / OnCollisionEnter/Exit /   │
│      OnDestroy — same contract as ScriptableEntity              │
│                                                                 │
│  UIEngine (RmlUi)                                               │
│    • RML/RCSS documents rendered via a custom bgfx backend      │
│      (RenderPass::UI view into the composite framebuffer)       │
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
3. **`ScriptComponent` stays a POD** — an `AssetHandle`, nothing else. All `sol::` objects live
   inside `ScriptEngine`, keyed by entity UUID, so `Components.h` stays sol2-free and the
   play-mode `Scene::Copy` needs nothing special.
4. **Scripting is a system, not a Scene bolt-on.** A `LuaScriptSystem` registered in the `Scene`
   constructor drives the lifecycle through the same reactive views `NativeScriptSystem` uses.
   The original plan's hand-called `InstantiateEntity` hook ("called when an entity with a
   ScriptComponent is created during play") had no caller — `InitView` *is* that caller.
5. The existing `NativeScriptComponent` stays untouched — Lua scripting is added alongside it.

---

## 2. Part 1 — Embedding Lua 5.4 + sol2

### 2.1 Add submodules

```bash
git submodule add https://github.com/lua/lua GanymedEngine/extern/lua
cd GanymedEngine/extern/lua && git checkout v5.4.8 && cd -   # latest 5.4.x tag

git submodule add https://github.com/ThePhD/sol2 GanymedEngine/extern/sol2
cd GanymedEngine/extern/sol2 && git checkout v3.3.0 && cd -  # or newest release tag
```

- `lua/lua` is the official read-only source mirror — plain C files, no build system needed,
  which fits the engine's "build everything as a premake static lib" approach.
- sol2 is header-only; only an include dir is needed.
- If script throughput ever becomes a problem, LuaJIT is a drop-in *authoring-compatible* swap
  (TSTL can target it via `luaTarget: "JIT"`), so avoid Lua-5.4-only constructs in the binding
  layer. Not worth doing up front.

### 2.2 Premake build script for Lua

Following the `Jolt.lua` / `GLFW.lua` pattern (build script lives *outside* the submodule),
create `GanymedEngine/extern/Lua.lua`:

```lua
-- Build script for the Lua submodule. Lives outside the submodule because
-- git cannot track files inside a submodule's working tree.
project "Lua"
	kind "StaticLib"
	language "C"
	staticruntime "off"
	warnings "Off"

	-- Workspace-level output dirs, like every other extern project. Writing
	-- bin/ inside the submodule tree makes the submodule permanently dirty in
	-- git status - the parent repo's .gitignore does not apply inside it.
	targetdir ("%{wks.location}/bin/" .. outputdir .. "/%{prj.name}")
	objdir ("%{wks.location}/temp/" .. outputdir .. "/%{prj.name}")

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

> **MSVC note:** sol2 needs exceptions (on by default for the engine project; only the bgfx
> projects disable them locally) and C++17 (already set). `staticruntime "off"` matches the rest
> of the workspace — keep it consistent or you'll get LNK4098/2038 runtime-mismatch errors.

### 2.4 `ScriptComponent` and the Script asset type

Add to `Components.h` (POD only — no sol2 include here). **Deviation from the original plan:**
the component holds an `AssetHandle`, not a path string. The asset system exists now; every other
asset-referencing component (`StaticMeshComponent`, `SkyLightComponent`) uses handles, and the
serializer already has the handle + legacy-path pattern to copy:

```cpp
struct ScriptComponent
{
	AssetHandle Script = InvalidAssetHandle;   // a .lua asset

	ScriptComponent() = default;
	ScriptComponent(const ScriptComponent&) = default;
};
```

Register it with the ECS — this is what replaced the original plan's hand-edits to
`Scene::Copy`. In `ECS/ComponentTraits.h`:

```cpp
// Script lifecycle is reactive: InitView instantiates scripts that appear,
// FiniView tears down scripts that go away (including entity destruction).
template<> struct ComponentTraits<ScriptComponent>
{
	static constexpr bool TrackChanges = false;
	static constexpr bool EnableInit   = true;
	static constexpr bool EnableFini   = true;
};

using ComponentList = TypeList<
	/* ...existing... */
	ScriptComponent
>;
```

Adding it to `ComponentList` automatically covers `Scene::Copy`, the ViewDesc bitmask space and
the on_construct/on_destroy signal hookup. Serializer and editor UI remain manual (§2.8).

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

`.lua` files then show up typed in the ContentBrowser (give them an icon tint in
`GetAssetIconTint` and add `Script` to `IsImportableAsset`), and drag-drop onto a
`ScriptComponent` field goes through `AssetManager::ImportAsset` exactly like meshes do.
No `GetAsset<>` specialization is needed — `ScriptEngine` resolves handle → relative path via
`AssetManager::GetMetadata` and loads the file itself (hot reload wants the file path anyway).

### 2.5 `LuaScriptSystem` + `ScriptEngine`

Two pieces with a clean split:

- **`LuaScriptSystem`** (`GanymedE/Scene/Systems/LuaScriptSystem.h/.cpp`) — the per-scene driver.
  Owns *when* things happen, via views. Contains no sol2 types, so its header stays cheap.
- **`ScriptEngine`** (`GanymedE/Scripting/ScriptEngine.h/.cpp`) — the global VM owner. Owns *what*
  happens: the `sol::state`, class tables per script asset, instance tables per entity UUID.
  All `sol::` includes stay inside `Scripting/*.cpp`.

The system mirrors [`NativeScriptSystem`](../../GanymedEngine/source/GanymedE/Scene/Systems/NativeScriptSystem.cpp)
almost line for line:

```cpp
// LuaScriptSystem.h
class LuaScriptSystem : public ECS::System<LuaScriptSystem>
{
public:
	// FiniView note: ReactHas would not compile here - FiniView statically requires
	// ReadTypes == ReactTypes ("may only access its React types"), so the react
	// element must be ReactRO. Reading the buried component is useful for logging.
	using ScriptInitView = ECS::InitView<ECS::EntityId, ECS::ReactRO<ScriptComponent>>;
	using ScriptFiniView = ECS::FiniView<ECS::EntityId, ECS::ReactRO<ScriptComponent>>;
	using ScriptView     = ECS::IterView<ECS::EntityId, ECS::RO<ScriptComponent>>;

	// Never accessed through this view - declared so ValidateOrdering knows script
	// code writes transforms (via the bindings) and must run before TransformSystem.
	// Same trick PhysicsSystem uses for its out-of-view transform writeback.
	using TransformWrite = ECS::AccessView<ECS::RW<TransformComponent>>;

	using Views = TypeList<ScriptInitView, ScriptFiniView, ScriptView, TransformWrite>;

	using ECS::System<LuaScriptSystem>::System;

	void OnRuntimeStart() override;   // sweep all live scripts -> ScriptEngine::Instantiate
	void OnRuntimeStop() override;    // sweep -> ScriptEngine::DestroyAllInstances
	void OnUpdate(Timestep ts) override;
	void OnUpdateEditor(Timestep ts) override;
	const char* Name() const override { return "LuaScriptSystem"; }
};
```

```cpp
// LuaScriptSystem.cpp (shape)
void LuaScriptSystem::OnUpdate(Timestep ts)
{
	// Scripts that appeared since the last update. First-ever read reports every
	// existing script, which is why Instantiate is idempotent - OnRuntimeStart has
	// usually created them already.
	for (auto [entity, script] : View<ScriptInitView>())
		ScriptEngine::Instantiate(Entity{ entity, &m_Scene }, script.Script);

	// Scripts that went away - component removed or the whole entity destroyed.
	// The instance lives in ScriptEngine keyed by UUID, so only the id is needed;
	// the buried component is available for diagnostics.
	for (auto [entity, script] : View<ScriptFiniView>())
		ScriptEngine::DestroyInstance(entity);

	for (auto [entity, script] : View<ScriptView>())
		ScriptEngine::Update(entity, ts);
}

void LuaScriptSystem::OnUpdateEditor(Timestep ts)
{
	// Edit mode does not run scripts, but the reactive views MUST still be read
	// every update or the next runtime read asserts on the skipped epoch. Teardown
	// still runs, so removing a ScriptComponent in edit mode cleans up any instance
	// left from a previous play session. Exactly NativeScriptSystem's pattern.
	for (auto [entity, script] : View<ScriptInitView>()) { /* drain only */ }
	for (auto [entity, script] : View<ScriptFiniView>())
		ScriptEngine::DestroyInstance(entity);
	(void)ts;
}
```

Registration, in the `Scene` constructor — order is execution order, and
`ValidateOrdering` will hold you to it:

```cpp
m_Systems->Add<PhysicsSystem>(*this);
m_Systems->Add<NativeScriptSystem>(*this);
m_Systems->Add<LuaScriptSystem>(*this);   // NEW: scripts move things...
m_Systems->Add<TransformSystem>(*this);   // ...so the transform cache runs after
m_Systems->Add<CameraSystem>(*this);
m_Systems->Add<RenderSystem>(*this);
```

`ScriptEngine` keeps the original plan's shape, minus the scene-lifecycle passthrough
(the system owns that now):

```cpp
// ScriptEngine.h - no sol2 types leak out
class ScriptEngine
{
public:
	static void Init();       // create sol::state, open libs, register bindings
	static void Shutdown();

	static void SetSceneContext(Scene* scene);   // set by LuaScriptSystem::OnRuntimeStart

	static void Instantiate(Entity entity, AssetHandle script);   // idempotent
	static void DestroyInstance(entt::entity entity);
	static void DestroyAllInstances();
	static void Update(entt::entity entity, Timestep ts);

	static void OnCollisionEnter(Entity entity, Entity other);
	static void OnCollisionExit(Entity entity, Entity other);

	// Raw lua_State, needed to initialise the RmlUi Lua plugin
	static void* GetLuaState();
};
```

Implementation highlights (full sketch mirrors the original plan):

```cpp
struct ScriptEngineData
{
	sol::state Lua;
	Scene* SceneContext = nullptr;

	// Class tables keyed by asset handle (chunk executed once per load)
	std::unordered_map<AssetHandle, sol::table> ScriptClasses;
	// Per-entity instance tables keyed by entity UUID
	std::unordered_map<UUID, sol::table> EntityInstances;
};
```

- Loading: resolve `AssetHandle` → relative path via `AssetManager::GetMetadata`, then
  `GetAssetRoot() / relativePath` (the original plan's `AssetPaths::GetAssetDirectory()` never
  existed — the function is the free `GetAssetRoot()`).
- `safe_script_file` + `protected_function` everywhere: **a script error must log, never crash
  or throw across the C++ boundary.**
- **Disable an instance on its first error.** The original sketch logged the same `OnUpdate`
  error 60× per second forever. On a failed protected call, set a dead flag on the instance,
  log once with the script path, and skip it from then on — that is what "a bad script disables
  itself" actually means.
- TSTL interop, unchanged from the original: unwrap `default` exports
  (`if (scriptClass["default"].valid()) scriptClass = scriptClass["default"];`), and instance
  tables delegate method lookup via `setmetatable({}, { __index = classOrPrototype })`, with a
  `prototype` fallback for TSTL `class` output.
- Clearing `ScriptClasses` in `DestroyAllInstances` (runtime stop) means every editor Play press
  re-reads scripts from disk — the simplest hot reload that exists. §2.9 covers live reload
  during play.
- `GetLuaState()` returns `s_Data->Lua.lua_state()` — needed for RmlUi.

Instance keying by UUID works across the play-mode copy for free: `Scene::Copy` preserves UUIDs,
instances only exist while the runtime scene plays, and `SetSceneContext` points at it.

### 2.6 Bindings (sol2 usertypes)

Keep all bindings in one file, `Scripting/ScriptBindings.cpp`.

**Deviation from the original plan, and the most important change in this revision:**
**bind component data by value, with explicit setters — never hand Lua a reference into entt
storage.** The original plan's `GetTransform() -> TransformComponent&` had one known problem
(the reference dangles if the pool reallocates — its own pitfall #1, "enforced by convention")
and has since grown a second, worse one: `TransformComponent` is now **change-tracked**. A
direct write through a bound reference is invisible to the change log, `TransformSystem` never
refreshes the world-transform cache, and *the entity visibly does not move* even though the
component data changed. Value + setter kills both at once, and the cost — copying 9 floats —
is nothing at script scale.

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
		"Length",     [](const glm::vec3& v) { return glm::length(v); },
		"Normalized", [](const glm::vec3& v) { return glm::normalize(v); }
	);

	// ---- Entity: value-semantics transform access ----
	// Setters write the component AND report the write to change tracking. Without
	// MarkChanged the world-transform cache goes silently stale (see Components.h).
	lua.new_usertype<Entity>("Entity",
		"GetName", &Entity::GetName,
		"GetUUID", [](Entity& e) { return (uint64_t)e.GetUUID(); },

		"GetTranslation", [](Entity& e) { return e.GetComponent<TransformComponent>().Translation; },
		"SetTranslation", [](Entity& e, const glm::vec3& v) {
			e.GetComponent<TransformComponent>().Translation = v;
			ScriptEngine::GetSceneContext()->MarkChanged<TransformComponent>(e);
		},
		"GetRotation", [](Entity& e) { return e.GetComponent<TransformComponent>().Rotation; },   // Euler radians
		"SetRotation", [](Entity& e, const glm::vec3& v) {
			e.GetComponent<TransformComponent>().Rotation = v;
			ScriptEngine::GetSceneContext()->MarkChanged<TransformComponent>(e);
		},
		"GetScale", [](Entity& e) { return e.GetComponent<TransformComponent>().Scale; },
		"SetScale", [](Entity& e, const glm::vec3& v) {
			e.GetComponent<TransformComponent>().Scale = v;
			ScriptEngine::GetSceneContext()->MarkChanged<TransformComponent>(e);
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

Binding rules to hold the line on:

- **Structural changes route through the CommandQueue.** `Entity::AddComponent` /
  `Scene::DestroyEntity` *assert* while systems run. Any future binding that spawns or destroys
  (projectiles, pickups) must queue through `Scene::Commands()` — `CreateEntity` returns a
  `PendingEntity` that components can be queued onto in the same frame; the change becomes
  visible next frame. Expose that as the scripting contract from day one
  (`Scene.Spawn(name) -> queued`), don't bind the immediate API.
- **Writes to tracked components always pair with `MarkChanged`.** If a binding grows that
  writes `RelationshipComponent` (re-parenting), same rule.
- Physics-facing bindings (`SetLinearVelocity`, `ApplyImpulse`, raycasts) should go through
  `PhysicsScene` rather than writing transforms, so kinematic vs dynamic bodies behave
  correctly. `PhysicsSystem::GetPhysicsScene()` is reachable via
  `scene.Systems().Get<PhysicsSystem>()`.
- Script component access is inherently *undeclared* ECS access (identical to native scripts —
  `ScriptableEntity::GetComponent` bypasses views too). The `TransformWrite` declaration on the
  system keeps the dominant case visible to ordering validation; exotic script writes are on
  the author, same as today.

### 2.7 Collision events

`PhysicsSystem::DispatchCollisionEvents` already resolves each event's UUIDs and notifies both
sides' native script instances through an `AccessView`. Add the Lua path beside it, keeping
dispatch **per fixed step** (inside the accumulator loop) so native and Lua scripts see identical
timing:

```cpp
// PhysicsSystem.h: add to Views
using LuaScriptAccess = ECS::AccessView<ECS::RO<ScriptComponent>>;

// PhysicsSystem.cpp, inside the notify lambda:
if (luaScripts.Has(self))
{
	if (event.Entered) ScriptEngine::OnCollisionEnter(self, other);
	else               ScriptEngine::OnCollisionExit(self, other);
}
```

### 2.8 Serialization + editor UI

**SceneSerializer** — handle first, legacy path fallback, same as `StaticMeshComponent`:

```cpp
// Serialize
if (entity.HasComponent<ScriptComponent>())
{
	auto& sc = entity.GetComponent<ScriptComponent>();
	out << YAML::Key << "ScriptComponent" << YAML::BeginMap;
	if (IsAssetHandleValid(sc.Script))
		out << YAML::Key << "Script" << YAML::Value << static_cast<uint64_t>(sc.Script);
	out << YAML::EndMap;
}

// Deserialize
if (auto scriptComponent = entity["ScriptComponent"])
{
	auto& sc = deserializedEntity.AddComponent<ScriptComponent>();
	if (auto handle = scriptComponent["Script"])
		sc.Script = handle.as<uint64_t>();
	else if (auto path = scriptComponent["ScriptPath"])          // pre-handle scenes
		sc.Script = AssetManager::ImportAsset(path.as<std::string>());
}
```

**SceneHierarchyPanel** — add `ScriptComponent` to the Add Component popup and a
`DrawComponent<ScriptComponent>` block: show the asset's path (via `GetMetadata`), accept a
`CONTENT_BROWSER_ITEM` drop filtered to `.lua`, import via `AssetManager::ImportAsset` — the
`.glb` handling in `EditorLayer` and the mesh field in the panel are the pattern to copy. Panels
run outside the system update, so the immediate `Entity` API is fine here, as everywhere in the
editor.

A later, worthwhile step (not required for milestone 3): exposed script *properties* — a
`Properties` table on the class read by the panel and serialized per entity. Design it when the
first script needs tuning without recompiling… which is immediately, so keep it near the top of
the polish list.

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
	local pos = self.entity:GetTranslation()

	if Input.IsKeyPressed(Key.W) then
		pos.z = pos.z - self.speed * ts
	end
	if Input.IsKeyPressed(Key.S) then
		pos.z = pos.z + self.speed * ts
	end

	self.entity:SetTranslation(pos)
end

function Player:OnCollisionEnter(other)
	Log.Warn("Hit " .. other:GetName())
end

function Player:OnDestroy()
end

return Player
```

(Get → mutate the copy → Set. The setter is what makes the movement visible — it routes the
write through change tracking.)

**Hot reload during play** (optional, later): store each script's
`filesystem::last_write_time` at load; once per second, re-check; on change, re-run the chunk,
replace the class table, and re-point each live instance's metatable `__index` at the new table.
Instance state (fields on `self`) survives; only methods are swapped. Log and keep the old table
if the new chunk has a syntax error. Unlike shaders, no offline compile step is involved — Lua
is data.

---

## 3. Part 2 — TypeScriptToLua (TS authoring layer)

Nothing in this section touches C++ — it's tooling that emits `.lua` files into the assets
folder, and it was untouched by both engine refactors. (One exception was already handled: the
`default` export unwrap in the class loader.)

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
		"noImplicitSelf": true,
		// Lua tracebacks point at .ts lines — enable from day one.
		"sourceMapTraceback": true
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

Run `npm run watch` while working — every save recompiles to `assets/scripts/*.lua`. (Later,
the editor can spawn this process itself when a project opens.)

### 3.3 Engine API declarations — `types/ganymed.d.ts`

This file is the TS mirror of `ScriptBindings.cpp`. Keep them in sync — it's the contract that
gives you IntelliSense and compile errors against the real engine API. Note the get/set shape
matching §2.6:

```typescript
/** @noSelfInFile */

declare interface Vec3 {
	x: number;
	y: number;
	z: number;
	Length(): number;
	Normalized(): Vec3;
}

declare interface Entity {
	GetName(): string;
	GetUUID(): number;

	/** Returns a copy. Mutate it, then call the setter — setters route the write
	 *  through the engine's change tracking; nothing moves without them. */
	GetTranslation(): Vec3;
	SetTranslation(v: Vec3): void;
	GetRotation(): Vec3;          // Euler radians
	SetRotation(v: Vec3): void;
	GetScale(): Vec3;
	SetScale(v: Vec3): void;

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

`/** @noSelfInFile */` and `noImplicitSelf` stop TSTL from inserting `self` parameters on these
*static* API calls (`Input.IsKeyPressed(...)` must compile to a `.` call, not a `:` call).

### 3.4 Script pattern in TS

**Use object literals, not `class`.** The C++ loader instantiates via
`setmetatable({}, {__index = ...})`, so an object literal (methods on the table itself) is the
zero-surprise path. TSTL classes work through the `prototype` fallback in the loader, but their
constructors never run — a foot-gun better avoided by convention.

```typescript
// scripts-src/Player.ts
const Player: Script & { speed: number } = {
	entity: undefined!,   // injected by ScriptEngine before OnCreate
	speed: 5.0,

	OnCreate() {
		Log.Info(`Player created: ${this.entity.GetName()}`);
	},

	OnUpdate(ts: number) {
		const pos = this.entity.GetTranslation();
		if (Input.IsKeyPressed(Key.W)) pos.z -= this.speed * ts;
		if (Input.IsKeyPressed(Key.S)) pos.z += this.speed * ts;
		this.entity.SetTranslation(pos);
	},

	OnCollisionEnter(other: Entity) {
		Log.Warn(`Hit ${other.GetName()}`);
	},
};

export default Player;
```

Compiles to a self-contained `assets/scripts/Player.lua` whose exports table has a `default`
field — exactly what the loader unwraps.

### 3.5 Limitations to know

- **No npm runtime packages** unless they're TSTL-compatible (pure TS compiled by tstl, or Lua
  packages with declaration files). No Node APIs, no DOM.
- Debugging happens at the Lua level; `sourceMapTraceback` (enabled above) maps tracebacks to
  `.ts` lines.
- `LuaMultiReturn`, `$range`, and other TSTL language extensions are documented at
  typescripttolua.github.io — needed occasionally when a binding returns multiple values.

---

## 4. Part 3 — RmlUi (HTML/CSS UI)

> **This part diverges most from the original plan.** The engine has no OpenGL, no Glad, and no
> framebuffer bind/unbind — the reference `RmlUi_Renderer_GL3` backend cannot initialise at all
> (the window is `GLFW_NO_API`; there is no GL context). A custom **bgfx render backend** is
> required. That sounds worse than it is: the in-tree
> [`ImGuiRendererBgfx`](../../GanymedEngine/source/Platform/Bgfx/ImGuiRendererBgfx.cpp) already
> solves ~90% of the same problem and is the template to copy. In exchange, two whole pitfall
> classes from the original plan (GL state leakage, duplicate GL loaders) vanish — bgfx state is
> per-submit, nothing leaks.

### 4.1 Add submodules

```bash
git submodule add https://github.com/mikke89/RmlUi GanymedEngine/extern/RmlUi
cd GanymedEngine/extern/RmlUi && git checkout 6.1 && cd -    # latest release tag

git submodule add https://gitlab.freedesktop.org/freetype/freetype GanymedEngine/extern/freetype
cd GanymedEngine/extern/freetype && git checkout VER-2-13-3 && cd -
```

FreeType is RmlUi's one hard dependency (its default font engine). It builds cleanly as a static
lib from a known minimal file list (below).

> **RmlUi 6.x note:** the `RenderInterface` was overhauled in 6.0 (compiled-geometry handles and
> spans replaced the old immediate-mode calls). Implement against the header of the tag you pin,
> not against pre-6.0 tutorials or backends.

### 4.2 Premake build scripts

Both scripts follow the workspace conventions: build script outside the submodule, **output
outside the submodule** (`%{wks.location}/bin|temp` — anything written inside the submodule tree
shows the submodule as permanently dirty in git).

`GanymedEngine/extern/FreeType.lua`:

```lua
project "FreeType"
	kind "StaticLib"
	language "C"
	staticruntime "off"
	warnings "Off"

	targetdir ("%{wks.location}/bin/" .. outputdir .. "/%{prj.name}")
	objdir ("%{wks.location}/temp/" .. outputdir .. "/%{prj.name}")

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

	targetdir ("%{wks.location}/bin/" .. outputdir .. "/%{prj.name}")
	objdir ("%{wks.location}/temp/" .. outputdir .. "/%{prj.name}")

	files
	{
		"RmlUi/Source/Core/**.cpp",
		"RmlUi/Source/Core/**.h",
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
		symbols "on"
		runtime "Debug"
		-- Visual document inspector (element tree, computed properties).
		-- Debug-only so Dist doesn't carry it.
		files { "RmlUi/Source/Debugger/**.cpp" }

	filter "configurations:Release or configurations:Dist"
		runtime "Release"
		optimize "on"
		defines { "NDEBUG" }
```

Wire both into the root `premake5.lua` (`IncludeDir["RmlUi"]`, dependency group includes) and
add `"RmlUi"`, `"FreeType"` to GanymedEngine's `links`, plus `RMLUI_STATIC_LIB` to its `defines`
(consumers need it too, like the Jolt defines).

> If the exact file list drifts on a newer RmlUi/FreeType tag, trust the compiler:
> missing-symbol link errors name the module to add. Check each tag's CMakeLists for the
> authoritative list.

### 4.3 Backend files (renderer + platform glue)

RmlUi deliberately doesn't render or read input itself — you provide a `RenderInterface` and
`SystemInterface`.

**SystemInterface**: copy `RmlUi_Platform_GLFW.h/.cpp` from `extern/RmlUi/Backends/` into
`GanymedEngine/source/Platform/RmlUi/` — it's clipboard/cursor/time glue with no graphics
dependency, and its key-mapping table is reusable for event translation (the engine's
`KeyCodes.h` uses GLFW values, so it maps 1:1).

**RenderInterface**: write `Platform/RmlUi/RmlUiRendererBgfx.h/.cpp`, modeled directly on
`ImGuiRendererBgfx`. The job, per RmlUi 6.x's interface:

| RmlUi callback | bgfx implementation |
|---|---|
| `CompileGeometry(vertices, indices)` | Create static VB/IB (RmlUi geometry is reused across frames — that's the point of compiling); `Rml::Vertex` is pos(vec2) + color(u8×4) + texcoord(vec2), one small `bgfx::VertexLayout` |
| `RenderGeometry(handle, translation, texture)` | Set buffers + texture (or a white fallback), translation via a transform matrix, submit to the UI view |
| `ReleaseGeometry` / `ReleaseTexture` | `bgfx::destroy` — guarded by `Renderer::IsGpuAlive()` like every other resource destructor |
| `EnableScissorRegion` / `SetScissorRegion` | `bgfx::setScissor` per submit (cache the current rect) |
| `LoadTexture` | Decode with stb_image (already vendored) → `bgfx::createTexture2D`. Don't rely on RmlUi decoding for you — the reference backends only handle TGA |
| `GenerateTexture` | `createTexture2D` from the raw RGBA bytes (font glyph atlases arrive here) |
| `SetTransform` | Optional (CSS `transform` support). Skip in the first pass; store and pre-multiply into the submit transform when needed |

Rendering details that carry over from the ImGui backend verbatim:

- One dedicated view in **`Sequential` mode** (UI paints back-to-front; bgfx must not reorder).
- The ortho projection is built from `bgfx::getCaps()->homogeneousDepth` by hand — the
  compile-time `GLM_FORCE_DEPTH_ZERO_TO_ONE` cannot adapt per backend.
- Blending on (`BGFX_STATE_BLEND_ALPHA`), depth test off.
- Submit with an **explicit view id parameter** on every `bgfx::submit` — do not go through
  `RenderCommand::SetViewId`, whose sticky current-view state would then need restoring
  (the class of bug that once sent the whole scene into a shadow cascade).
- One `.sc` shader pair (`vs_RmlUi`/`fs_RmlUi` + its own `varying.RmlUi.def.sc`, since the
  vertex layout differs from the engine's) — effectively the ImGui shader with RmlUi's layout.
  `compile_shaders` already supports per-shader varying files.

Add the pass to [`RenderPassIDs.h`](../../GanymedEngine/source/GanymedE/Renderer/RenderPassIDs.h):

```cpp
// Game UI (RmlUi), composited into the final LDR image after post-processing.
constexpr uint16_t UI = 28;   // after Composite (26) and Picking (27), before ImGui (200)
```

Because **bgfx executes views in ID order**, this one constant replaces the original plan's
"call OnRender after tonemapping with the framebuffer still bound" choreography: target the view
at the composite framebuffer (`Framebuffer::BindToView(RenderPass::UI)`), and the UI lands after
tonemap/FXAA — in LDR display space, never tonemapped — no matter where in the frame the submit
calls happen. It also appears inside the editor's viewport ImGui image automatically, since that
image *is* the composite attachment.

### 4.4 `UIEngine`

New files: `GanymedEngine/source/GanymedE/UI/UIEngine.h/.cpp`. Same singleton shape as
`ScriptEngine`:

```cpp
#pragma once

#include "GanymedE/Core/Timestep.h"
#include "GanymedE/events/Event.h"

#include <filesystem>

namespace Rml { class Context; class ElementDocument; }

namespace GanymedE {

	class Framebuffer;

	class UIEngine
	{
	public:
		// After Renderer::Init (needs bgfx alive + compiled shaders) and after
		// ScriptEngine::Init (shares its lua_State).
		static void Init(uint32_t width, uint32_t height);
		static void Shutdown();

		static Rml::ElementDocument* LoadDocument(const std::filesystem::path& rmlPath);
		static void CloseAllDocuments();

		// The framebuffer the UI view composites into (the editor passes the
		// SceneRenderer's composite target; a shipped game passes nullptr for the
		// backbuffer).
		static void SetTarget(const Ref<Framebuffer>& target);

		static void OnUpdate(Timestep ts);      // Context::Update — layout/animation
		static void OnRender();                 // submit draw lists to RenderPass::UI
		static void OnEvent(Event& e);          // engine events → Rml input
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

#include "Platform/RmlUi/RmlUiRendererBgfx.h"
#include "Platform/RmlUi/RmlUi_Platform_GLFW.h"
#include "GanymedE/Scripting/ScriptEngine.h"

void UIEngine::Init(uint32_t width, uint32_t height)
{
	s_Data = new UIEngineData();

	Rml::SetSystemInterface(&s_Data->SystemInterface);   // the copied GLFW one
	Rml::SetRenderInterface(&s_Data->RenderInterface);   // RmlUiRendererBgfx
	Rml::Initialise();

	// Share the gameplay VM — UI scripts and gameplay scripts see the same globals
	Rml::Lua::Initialise((lua_State*)ScriptEngine::GetLuaState());

	s_Data->Context = Rml::CreateContext("main", Rml::Vector2i(width, height));

#ifdef GE_DEBUG
	Rml::Debugger::Initialise(s_Data->Context);   // toggle with Rml::Debugger::SetVisible
#endif

	// Present in GanymedEditor/assets/fonts/montserrat/ (verified)
	Rml::LoadFontFace("assets/fonts/montserrat/Montserrat-Regular.ttf");
	Rml::LoadFontFace("assets/fonts/montserrat/Montserrat-Bold.ttf");
}

void UIEngine::OnUpdate(Timestep) { s_Data->Context->Update(); }

void UIEngine::OnRender()
{
	// Target + rect for the UI view, then Context::Render drives the backend's
	// RenderGeometry calls, each submitting to RenderPass::UI.
	s_Data->RenderInterface.BeginFrame(s_Data->Target);
	s_Data->Context->Render();
	s_Data->RenderInterface.EndFrame();
}

void UIEngine::Shutdown()
{
	// TWO ordering constraints:
	//  1. before ScriptEngine::Shutdown — the Lua plugin holds refs into the VM;
	//  2. while Renderer::IsGpuAlive() — Rml::Shutdown releases its textures, and
	//     after bgfx::shutdown those destroys would touch a dead context.
	// In practice: run this from the same teardown path as Renderer::Shutdown.
	Rml::Shutdown();
	delete s_Data; s_Data = nullptr;
}
```

**Init/shutdown order:**
`Renderer::Init` → `ScriptEngine::Init` → `UIEngine::Init` … `UIEngine::Shutdown` →
`ScriptEngine::Shutdown` (→ bgfx dies with the window afterwards).

### 4.5 Frame-loop and editor integration

Per frame in Play state (call order is now mostly irrelevant on the render side — view IDs are
the schedule — but Update ordering still matters):

1. `Scene::OnUpdateRuntime` — gameplay scripts show/hide documents, set data-model values
2. `UIEngine::OnUpdate` — RmlUi layouts/animates with this frame's values
3. `UIEngine::OnRender` — submits to `RenderPass::UI`; bgfx composites it after the post stack

In `EditorLayer`:

- `OnAttach` → `UIEngine::SetTarget(m_SceneRenderer->GetCompositeFramebuffer())` (expose the
  composite target from `SceneRenderer`; today only its texture ID is exposed).
- `OnScenePlay` → load the scene's UI documents (start with a hard-coded `assets/ui/hud.rml`;
  later make it a scene property); `OnSceneStop` → `UIEngine::CloseAllDocuments()`.
- Viewport resize → `UIEngine::SetViewport(w, h)` alongside the existing resize handling.
- `OnEvent` in Play state → forward to `UIEngine::OnEvent` **before** the camera handlers, only
  while the viewport is hovered/focused. Translate mouse coordinates to viewport space first:
  `mouse - m_ViewportBounds[0]` — the same math the picking code uses (and like picking, no
  extra Y-flip: RmlUi and the pick request both work in top-left viewport coordinates).
- The event translation itself is a `switch` mapping `MouseMovedEvent`,
  `MouseButtonPressed/ReleasedEvent`, `MouseScrolledEvent`, `KeyPressed/Released`, `KeyTyped` to
  `Context::ProcessMouseMove / ProcessMouseButtonDown / ...`. Reuse the key table from the
  copied `RmlUi_Platform_GLFW.cpp`.
- `Context::Process*` returns **true when the event should propagate** — on false, mark the
  engine event `Handled` so gameplay input doesn't also fire.

In a shipped game (Sandbox-style app), the same calls run with a null target (backbuffer) and
window-space mouse coordinates — no translation.

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

Because the Lua plugin shares the gameplay VM, a gameplay script (written in TS, compiled to
Lua) can also drive the UI directly — RmlUi exposes `rmlui.contexts`, document lookup, element
manipulation, and event listeners to Lua, and `<script>` blocks inside `.rml` documents execute
on the shared state. Declare the small part of that API you use in `ganymed.d.ts` and your UI
logic gets type-checked too.

---

## 5. Suggested implementation order

Each milestone compiles, runs, and is independently verifiable:

1. **Lua static lib builds.** Submodules added, `Lua.lua` written (workspace-level output
   dirs!), workspace links. Smoke test: `lua.script("print('hello')")` in `Application`
   startup, then delete.
2. **ScriptEngine + LuaScriptSystem + ScriptComponent, no editor UI.** `ComponentList` +
   traits registration, bindings for Vec3/Entity/Input/Log with the get/set-`MarkChanged`
   shape, system registered before `TransformSystem` (`ValidateOrdering` must stay quiet).
   Hand-add a `ScriptComponent` in scene-setup code, press Play, watch a cube move via
   `Player.lua` — *the cube moving at all proves the MarkChanged path works*. Verify the
   reactive-view discipline: add/remove a script component in edit mode, then play — no
   epoch asserts, no leaked instances.
3. **Editor + serialization.** SceneHierarchyPanel add/inspect + drag-drop, SceneSerializer
   (handle + legacy path), ContentBrowser `.lua` typing. Script assignment survives
   save/load and the play-mode copy.
4. **TSTL toolchain.** `scripts-src/` project, `ganymed.d.ts`, port `Player.lua` to
   `Player.ts`, confirm the compiled output behaves identically.
5. **RmlUi builds + renders.** FreeType/RmlUi libs, `RmlUiRendererBgfx` + shader pair +
   `RenderPass::UI`, `UIEngine`, static `hud.rml` visible in the Play-state viewport.
   Debugger toggle works.
6. **UI input + data binding.** Event routing with viewport coordinate translation; health
   bar driven from a gameplay script.
7. **Polish.** Script properties exposed to the editor, Lua-side UI scripting from TS, hot
   reload during play, collision callbacks, physics bindings (through `PhysicsScene`).

Milestones 1–3 are the core value; 4–7 can each ship independently. Documentation rule: each
milestone that lands engine code also lands its docs — propose `docs/engine/scripting.md` with
milestone 2 and `docs/engine/ui.md` with milestone 5, plus index entries in `docs/README.md`.

---

## 6. Pitfalls & gotchas

| # | Pitfall | Mitigation |
|---|---------|------------|
| 1 | **Invisible writes to tracked components** — a script write that skips `MarkChanged` leaves the world-transform cache stale; the entity *visibly does not move* | Value-semantics bindings whose setters call `Scene::MarkChanged<T>()`; never bind a mutable component reference |
| 2 | **EnTT reference invalidation** — component refs held in Lua dangle if a pool reallocates | Same fix as #1: bindings return copies, setters re-fetch. No references cross into Lua |
| 3 | **Structural changes during update assert** — `Entity::AddComponent` / `Scene::DestroyEntity` are illegal while systems run | Spawning/destroying bindings go through `Scene::Commands()` (`PendingEntity` for create-plus-components); visible next frame, and say so in the script API docs |
| 4 | **Reactive views must be drained every update** — a skipped `InitView`/`FiniView` read asserts on the epoch gap | `LuaScriptSystem::OnUpdateEditor` drains without instantiating, exactly like `NativeScriptSystem` |
| 5 | **Errors crossing the C++ boundary** | `SOL_ALL_SAFETIES_ON=1`, `safe_script_file`, `protected_function` everywhere; an instance is disabled after its first error (log once, not 60×/s) |
| 6 | **sol2 compile times** — heavy templates | Keep ALL sol2 includes inside `Scripting/*.cpp`; never include `sol.hpp` in a header (`LuaScriptSystem`'s header must stay sol2-free) |
| 7 | **Runtime library mismatch** (LNK2038) | Every new premake project uses `staticruntime "off"` like the rest of the workspace |
| 8 | **Submodule shows dirty forever** | All three new extern build scripts output to `%{wks.location}/bin` / `temp`, never inside the submodule tree |
| 9 | **Sticky view ID** — a backend that mutates `RenderCommand::SetViewId` re-routes every draw after it | `RmlUiRendererBgfx` passes its view id explicitly on each submit, like `ImGuiRendererBgfx` |
| 10 | **UI rendered in HDR space** — tonemap washing out UI colors | Solved structurally: `RenderPass::UI` (28) executes after Tonemap/FXAA/Composite because bgfx runs views in ID order |
| 11 | **RmlUi 6.x interface drift** — pre-6.0 tutorials/backends show a different `RenderInterface` | Implement against the pinned tag's header; compiled-geometry model, spans |
| 12 | **GPU teardown order** — RmlUi holds textures; releasing them after `bgfx::shutdown` faults | `UIEngine::Shutdown` runs while `Renderer::IsGpuAlive()`; backend destructors carry the same guard as every engine resource |
| 13 | **Lua/RmlUi shutdown order** — the Rml Lua plugin holds refs into the shared `lua_State` | `Rml::Shutdown()` strictly before `ScriptEngine::Shutdown()` |
| 14 | **Editor mouse coordinates** — RmlUi expects viewport-local pixels | Translate with `m_ViewportBounds[0]` exactly like the existing picking code |
| 15 | **TSTL classes** — methods live on `.prototype`, constructors don't run | Prefer object-literal scripts; the loader's `prototype` fallback covers stragglers |
| 16 | **RmlUi is not a browser** — ~XHTML + CSS2 with much of CSS3 (flexbox, animations, transforms, transitions) | Check the RCSS docs before assuming a CSS feature exists; the Debugger shows computed properties |

Retired from the original plan's table (no longer applicable): GL state leakage and the
duplicate-GL-loader collision — there is no GL state to leak and no loader to collide with; and
"`Scene::Copy` must be extended per component" — `ComponentList` registration covers it.

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
- In-tree templates: [`ImGuiRendererBgfx`](../../GanymedEngine/source/Platform/Bgfx/ImGuiRendererBgfx.cpp)
  (bgfx UI backend shape), [`NativeScriptSystem`](../../GanymedEngine/source/GanymedE/Scene/Systems/NativeScriptSystem.cpp)
  (script lifecycle via reactive views), [`docs/engine/ecs.md`](../engine/ecs.md) (the rules the
  bindings must respect)

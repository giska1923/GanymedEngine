# Scripting (Lua)

Gameplay scripting on **Lua 5.4** via **sol2**, alongside — not replacing — the existing
`NativeScriptComponent` path. Authoring in TypeScript (compiled to Lua by TypeScriptToLua) is a
tooling layer on top; the engine only ever loads `.lua`.

Plan of record: [`Scripting-And-UI-Integration.md`](../toDo&done/Scripting-And-UI-Integration.md).

## Layout

| Piece | File | Owns |
|---|---|---|
| `ScriptComponent` | [`Scene/Components.h`](../../GanymedEngine/source/GanymedE/Scene/Components.h) | An `AssetHandle`. Nothing else. |
| `LuaScriptSystem` | [`Scene/Systems/LuaScriptSystem.h`](../../GanymedEngine/source/GanymedE/Scene/Systems/LuaScriptSystem.h) | *When* things happen — the reactive views |
| `ScriptEngine` | [`Scripting/ScriptEngine.h`](../../GanymedEngine/source/GanymedE/Scripting/ScriptEngine.h) | *What* happens — the `sol::state`, class tables, instances |
| Bindings | [`Scripting/ScriptBindings.cpp`](../../GanymedEngine/source/GanymedE/Scripting/ScriptBindings.cpp) | The entire Lua-visible API surface |

Two structural rules hold this together:

- **`ScriptComponent` is a POD.** Every `sol::` object lives inside `ScriptEngine`, keyed by entity
  UUID. `Components.h` is included nearly everywhere and sol2's templates are expensive, so the
  component stays a bare handle — which also means the play-mode `Scene::Copy` needs nothing
  special for it.
- **sol2 appears in exactly one header**, [`ScriptBindings.h`](../../GanymedEngine/source/GanymedE/Scripting/ScriptBindings.h),
  which is private to `Scripting/` and included only from `.cpp` files there. `ScriptEngine.h` and
  `LuaScriptSystem.h` are sol2-free; `GetLuaState()` returns `void*` for that reason.

One VM for the whole runtime, not one per scene — RmlUi's Lua plugin will run on the same
`lua_State`, so UI and gameplay scripts share globals.

## Lifecycle

`LuaScriptSystem` mirrors [`NativeScriptSystem`](../../GanymedEngine/source/GanymedE/Scene/Systems/NativeScriptSystem.cpp)
almost line for line, and for the same reasons:

- `InitView<EntityId, ReactRO<ScriptComponent>>` — scripts that appeared since the last update.
- `FiniView<EntityId, ReactRO<ScriptComponent>>` — scripts that went away, *including* via entity
  destruction; the slot reads the graveyard copy. `ReactRO` rather than `ReactRW` because `FiniView`
  statically requires `ReadTypes == ReactTypes`.
- `IterView<EntityId, RO<ScriptComponent>>` — per-frame `OnUpdate`.
- `AccessView<RW<TransformComponent>>` — **declared but never used**. It exists so
  `ValidateOrdering` knows script code writes transforms through the bindings (outside any view)
  and that `TransformSystem` must run after this system. Same trick `PhysicsSystem` uses for
  `PhysicsScene::SyncTransforms`.

Registration order in the `Scene` constructor is
`PhysicsSystem` → `NativeScriptSystem` → **`LuaScriptSystem`** → `TransformSystem` → `CameraSystem`
→ `RenderSystem`. Scripts move things; the transform cache runs after.

`OnUpdateEditor` drains both reactive views without instantiating. This is not optional: a skipped
`InitView`/`FiniView` read asserts on the epoch gap the next time the runtime reads it. Teardown
still runs there, so removing a `ScriptComponent` in edit mode cleans up an instance left over from
a previous play session.

### Instance keying, and why there are two maps

Instances are keyed by **UUID**, with a side map from `entt::entity` → `UUID` captured at
instantiation. The side map is load-bearing, not redundant: teardown is driven by `FiniView`, which
fires for entity destruction as well as component removal, and by then the entity is gone from the
registry — its `IDComponent`, and therefore its UUID, is unreachable. The reverse map is the only
way to find the instance again.

`Instantiate` is idempotent, because it is genuinely called twice: `OnRuntimeStart` sweeps every
live script, then the `InitView`'s first-ever read reports those same entities.

`OnRuntimeStop` clears instances *and* the cached class tables, so every Play press re-reads
scripts from disk — the cheapest possible reload, and it covers the common loop.

### Hot reload during play

`ScriptEngine::PollHotReload` (driven once per `LuaScriptSystem::OnUpdate`, throttled internally to
once a second because it stats every loaded script) re-runs any chunk whose file changed and
re-points each live instance's metatable `__index` at the new method table.

**Only the lookup target changes**, so fields already on `self` survive: an entity keeps its
position, timers, and whatever else it accumulated. Verified by swapping a version string mid-play —
the output went `VERSION=one ticks=8` → `Hot reloaded (1 live instance)` → `VERSION=TWO ticks=9`.
New code, same state.

Two deliberate behaviours:

- **A chunk that fails to compile leaves the old table live.** A bad save mid-play logs and keeps
  the running game on the last good version rather than killing every instance. The new timestamp
  is taken anyway, or a file with a syntax error re-reports every second until it is fixed.
- **A reload revives a dead instance.** An edit is the author's fix for whatever disabled it, so
  the `Dead` flag clears.

## Script contract

A script is a Lua module returning a table of lifecycle methods. Instances get a fresh table whose
metatable `__index` points at the class table, so methods are shared and `self` is per-entity.
`self.entity` is injected before `OnCreate`.

```lua
local Player = {}

function Player:OnCreate()
	self.speed = 3.0
end

function Player:OnUpdate(ts)
	local pos = self.entity:GetTranslation()
	if Input.IsKeyPressed(Key.W) then pos.z = pos.z - self.speed * ts end
	self.entity:SetTranslation(pos)      -- the setter is what makes it move; see below
end

return Player
```

All hooks are optional: `OnCreate`, `OnUpdate(ts)`, `OnDestroy`, `OnCollisionEnter/Exit(other)`.

Collision hooks are dispatched by `PhysicsSystem::DispatchCollisionEvents`, per fixed step inside
the accumulator loop, so native and Lua scripts see identical timing. An entity may carry a native
script, a Lua script, both, or neither; both hear about the same collision.

**TypeScriptToLua interop** is handled at load: an `exports.default` table is unwrapped, and method
lookup falls back to `prototype` so TSTL `class` output resolves. Object literals remain the
documented pattern — a TSTL class constructor never runs through this path.

## Bindings, and the one rule that matters

Component data is bound **by value, with explicit setters**. Nothing hands Lua a reference into
entt storage. Two independent reasons, either sufficient:

1. `TransformComponent` is **change-tracked**. A write through a bound reference is invisible to the
   change log, so `TransformSystem` never refreshes the world-transform cache and **the entity
   visibly does not move** even though the component data changed. Every setter pairs its write with
   `Scene::MarkChanged<TransformComponent>()`.
2. A reference into a component pool dangles when that pool reallocates, and Lua cannot know when.

Copying nine floats beats both problems at script call rates.

Current surface: `Vec3` (arithmetic metamethods, `Length`, `Normalized`, `Dot`, `Cross`), `Entity`
(`GetName`, `GetUUID`, `Get/SetTranslation`, `Get/SetRotation` (Euler radians), `Get/SetScale`,
`HasRigidBody`), `Input`, `Key`, `Mouse`, `Log` (routed to the **client** logger — script output is
game output), `Scene.FindEntityByName`, `UI` (the HUD data model — see [ui.md](ui.md)).

> **The `Log` global collides with RmlUi's.** RmlUi's Lua plugin registers its own `Log` usertype,
> and it loads after `ScriptEngine::Init`, so it shadowed ours until `UIEngine` started calling
> `ScriptEngine::ReinstallGlobals()` afterwards. That is what `RegisterScriptGlobals` exists for —
> the tables are re-installable, the usertypes deliberately are not (re-registering one would
> rebuild metatables that live objects point at). One shared VM means one shared namespace; check
> for collisions when adding a global or bumping RmlUi.

Rules for anything added later:

- **Structural changes route through the CommandQueue.** `Entity::AddComponent` and
  `Scene::DestroyEntity` *assert* while systems run. A spawning binding must queue through
  `Scene::Commands()`; the change becomes visible next frame, and the script API should say so.
- **Writes to tracked components always pair with `MarkChanged`.** Same rule if a binding ever
  writes `RelationshipComponent`.
- Physics-facing bindings belong on `PhysicsScene`, not on transform writes, so kinematic and
  dynamic bodies behave correctly. `Entity:Get/SetLinearVelocity`, `AddImpulse` and `AddForce` do
  this — reached via `Scene::Systems().Get<PhysicsSystem>()->GetPhysicsScene()`, which is null
  outside play, so all four no-op rather than assert. **Writing a dynamic body's transform instead
  does nothing visible**: `PhysicsScene::SyncTransforms` overwrites it from the simulation every
  step. `AddImpulse` is a one-shot change in momentum; `AddForce` is consumed by the next step and
  wants calling every frame while the push lasts. Each wakes the body first, because Jolt silently
  discards a velocity set on a sleeping one.
- Script component access is inherently *undeclared* ECS access — identical to native scripts,
  where `ScriptableEntity::GetComponent` bypasses views too. The `TransformWrite` declaration keeps
  the dominant case visible to ordering validation; exotic writes are on the author.

## Errors

A script error must never cross the C++ boundary. Every call into Lua goes through a
`protected_function`, `SOL_ALL_SAFETIES_ON=1` is defined engine-wide, and chunks load via
`safe_script_file`.

On a failed call the instance is **disabled** — logged once with the script filename and method,
then skipped from then on. Without that, a throwing `OnUpdate` logs the identical error sixty times
a second and buries whatever actually went wrong.

## Sandboxing

The VM opens `base`, `math`, `string`, `table` and `coroutine` only. No `io`, `os` or `package`:
gameplay scripts have no business touching the filesystem or spawning processes, and leaving
`package` out stops a stray `require()` from silently resolving something.

## Init order

`ScriptEngine::Init` runs in the `Application` constructor after `Renderer::Init`, and
`Shutdown` in the destructor before `Renderer::Shutdown`. Application scope rather than the
editor's, because any app that constructs a `Scene` gets a `LuaScriptSystem`. It needs no
`AssetManager` at init time — script assets are resolved only when an instance is created.

`UIEngine` initialises *after* `ScriptEngine` — RmlUi's Lua plugin runs on this same `lua_State`,
so UI and gameplay scripts share globals — and shuts down *before* it, because the plugin holds
references into the state. See [ui.md](ui.md).

## Assets

`.lua` maps to `AssetType::Script`. Scripts are loaded by path rather than through
`AssetManager::GetAsset<>`: there is no runtime object to cache — the "asset" is a chunk executed
into the shared VM — and hot reload wants the path anyway. `ScriptEngine` resolves handle → relative
path via `AssetManager::GetMetadata`, then joins `GetAssetRoot()`.

## Editor & serialization

`ScriptComponent` appears in the Add Component popup and gets its own inspector section showing the
assigned asset with a Clear button; dropping a `.lua` from the Content Browser assigns it
(extension-filtered, imported through `AssetManager::ImportAsset`). `.lua` files are typed and
importable in the Content Browser. Details: [editor.md](../editor/editor.md).

Serialization follows the handle-first, legacy-path-fallback shape every other asset-referencing
component uses — `Script` (handle) with a `ScriptPath` fallback.

## TypeScript authoring (TypeScriptToLua)

Optional layer. The engine only ever loads `.lua`; hand-written Lua remains fully supported.

```
GanymedEditor/
├── assets/scripts/       ← emitted .lua — what the engine loads (tracked in git)
└── scripts-src/          ← the TS project (not scanned by AssetManager)
    ├── package.json / tsconfig.json / package-lock.json
    ├── types/ganymed.d.ts
    └── Player.ts
```

```
cd GanymedEditor/scripts-src
npm install        # once
npm run watch      # recompiles into ../assets/scripts on every save
```

[`types/ganymed.d.ts`](../../GanymedEditor/scripts-src/types/ganymed.d.ts) is the hand-written TS
mirror of `ScriptBindings.cpp`. **Nothing enforces that they agree** — it is the contract giving you
IntelliSense and compile errors against the real engine API, so a binding change edits both files or
the types quietly lie.

Config decisions worth knowing:

- **`luaLibImport: "inline"`** — every emitted `.lua` is self-contained. Required, not stylistic:
  `ScriptEngine` does not open the `package` library, so a non-inlined build could not resolve its
  helper imports at runtime.
- **`noImplicitSelf`**, paired with the `@noSelfInFile` annotation at the top of `ganymed.d.ts` —
  makes static API calls emit as `Input.IsKeyPressed(...)` (a `.` call) rather than a `:` call.
- **`moduleResolution: "bundler"`** — the `"node"` (node10) mode the older TSTL guides show is a
  hard error in TypeScript 6 and gone in 7. TSTL resolves modules through its own pass regardless.
- **`sourceMapTraceback: false`**, deviating from the integration plan. It emits a preamble calling
  `debug.getinfo`, which hard-requires the `debug` library — and `debug` defeats metatables, reads
  locals and upvalues, and can hook the VM, so it stays out of the sandbox. It would also buy
  nothing: sol2 builds tracebacks with `luaL_traceback` (the C API), which never reads the
  Lua-level `debug` table, so the rewritten `debug.traceback` is not on the path any engine-reported
  script error takes. Errors cite `.lua` lines; the emitted Lua tracks the TypeScript closely.

**Write scripts as object literals, not `class`.** `ScriptEngine` instantiates via
`setmetatable({}, { __index = ... })`, so a literal is the zero-surprise path. TSTL classes resolve
through the loader's `prototype` fallback, but their constructors never run.

`export default` is the expected shape — the loader unwraps `exports.default`.

> `assets/scripts/*.lua` is **generated output committed to git**, unlike compiled shader bytecode.
> Hand-written Lua is a supported authoring path, so the folder cannot be ignored wholesale. The
> consequence: if a file there has a `.ts` counterpart, editing the `.lua` directly is pointless —
> the next `tstl` run overwrites it silently.

## Exposed script properties — designed, not built

The one item from the plan's polish list still open, and the one worth doing next: a `Properties`
table on the class, shown in the inspector and serialized per entity, so a script can be tuned
without an edit-reload cycle.

It is **not** polish — it breaks the invariant at the top of this document. `ScriptComponent` is a
bare `AssetHandle`; per-entity overrides mean it grows a `map<string, variant>`, which touches
`Scene::Copy`, the serializer, and the panel. Sketch:

- `ScriptEngine` reads a `Properties` table off the *class* (name → default) without instantiating,
  so the panel can show fields before play.
- `ScriptComponent` carries overrides as a small string→variant map; the serializer round-trips it
  next to the handle.
- `Instantiate` applies overrides onto `self` after the metatable is set and before `OnCreate`.

Worth its own milestone rather than being folded into one.

## Not done yet

Script properties (above), and spawning/destroying bindings — which must go through
`Scene::Commands()`, per the binding rules.

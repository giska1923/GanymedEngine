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
scripts from disk. That is the entire hot-reload story for now — deliberately, since it costs
nothing and covers the common loop.

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
game output), `Scene.FindEntityByName`.

Rules for anything added later:

- **Structural changes route through the CommandQueue.** `Entity::AddComponent` and
  `Scene::DestroyEntity` *assert* while systems run. A spawning binding must queue through
  `Scene::Commands()`; the change becomes visible next frame, and the script API should say so.
- **Writes to tracked components always pair with `MarkChanged`.** Same rule if a binding ever
  writes `RelationshipComponent`.
- Physics-facing bindings belong on `PhysicsScene`, not on transform writes, so kinematic and
  dynamic bodies behave correctly.
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

When `UIEngine` lands, it initialises *after* `ScriptEngine` (it shares the `lua_State`) and shuts
down *before* it.

## Assets

`.lua` maps to `AssetType::Script`. Scripts are loaded by path rather than through
`AssetManager::GetAsset<>`: there is no runtime object to cache — the "asset" is a chunk executed
into the shared VM — and hot reload wants the path anyway. `ScriptEngine` resolves handle → relative
path via `AssetManager::GetMetadata`, then joins `GetAssetRoot()`.

## Not done yet

Editor UI and serialization for `ScriptComponent`, the TypeScript toolchain, exposed script
properties, collision-event dispatch from `PhysicsSystem`, and physics bindings. See the plan
document's implementation order.

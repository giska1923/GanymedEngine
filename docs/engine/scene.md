# Scene

`GanymedEngine/source/GanymedE/Scene/` — the world model: entities, components, hierarchy, the
built-in systems, scene-wide singletons, and YAML serialization. The ECS machinery it sits on is
documented in [ecs.md](ecs.md).

## Scene

[`Scene`](../../GanymedEngine/source/GanymedE/Scene/Scene.h) owns:

- the `entt::registry` (exposed as `Reg()` for the ECS layer and tooling),
- a `UUID → entt::entity` map making `FindEntityByUUID` O(1) (maintained by
  `CreateEntityWithUUID`, `DestroyEntity`, and therefore also by `Copy` and deserialization),
- the `SystemManager` and `CommandQueue` (held by `Scope` to keep `Scene.h` free of `System.h` —
  the systems include `Views.h`, which includes `Scene.h`),
- per-component-type `ChangeBuffer`s, graveyards, and init/fini buffers,
- singleton storage in `registry.ctx()` plus per-singleton epochs.

Key entry points:

| Method | Notes |
|---|---|
| `CreateEntity(name)` / `CreateEntityWithUUID(uuid, name)` | Every entity gets `IDComponent`, `TransformComponent`, `WorldTransformComponent`, `RelationshipComponent`, `TagComponent` |
| `DestroyEntity(entity)` | Detaches from parent, unparents children (they stay as roots), erases the UUID mapping, destroys. Asserts if called during a system update — use `Commands().DestroyEntity()` there |
| `OnRuntimeStart/Stop` | Forwarded to the systems (start runs in reverse registration order — see [ecs.md](ecs.md#systemmanager)) |
| `OnUpdateRuntime(ts, fallbackCamera)` / `OnUpdateEditor(ts, camera)` | FrameBegin → systems → FrameEnd; the editor camera is passed via the `RenderContext` singleton |
| `OnViewportResize(w, h)` | Updates all non-fixed-aspect `CameraComponent`s |
| `Copy(other)` | Play-mode snapshot: recreate entities by UUID, then copy every `ComponentList` component via `ForEachType`; script `Instance` pointers are nulled so runtime instances are recreated on play |
| `GetWorldSpaceTransform(entity)` | Walks the parent chain from locals — for **editor/tooling** (gizmos). Renderable code reads the cached `WorldTransformComponent` instead |
| `SetParent(child, parent)` / `Unparent(child)` | Maintains both sides of the relationship, rejects self/descendant parenting, and calls `MarkChanged<RelationshipComponent>` so the transform cache reacts |
| `MarkChanged<T>(entity)` | Report an out-of-view write of a tracked component (see [ecs.md](ecs.md#accessors-and-the-modify-invariant)) |

`Scene`'s constructor wires the entt signals for tracked/init/fini component types, creates the
`RenderContext` and `PhysicsSettings` singletons, registers the five built-in systems, and asserts
`ValidateOrdering()` passes.

## Entity

[`Entity`](../../GanymedEngine/source/GanymedE/Scene/Entity.h) is a value-type handle
(`entt::entity` + `Scene*`). `AddComponent`/`RemoveComponent` are **immediate** and assert when
called during a system update; `GetComponent`/`HasComponent` are always fine. `GetUUID()` and
`GetName()` read `IDComponent`/`TagComponent`. A default-constructed `Entity` is falsy.

## Component catalog

All in [`Components.h`](../../GanymedEngine/source/GanymedE/Scene/Components.h) — plain structs,
copyable, no behavior beyond small helpers.

### Identity (not in `ComponentList`, never copied generically)

- **`IDComponent`** — the entity's `UUID`; stable across runs and scene copies.
- **`TagComponent`** — display name.

### Spatial

- **`TransformComponent`** — local TRS (`vec3` each; rotation is Euler radians, applied X·Y·Z).
  `GetLocalTransform()` builds the matrix. **Change-tracked**: direct writes need `MarkChanged`.
- **`WorldTransformComponent`** — the cached world matrix, maintained by `TransformSystem`.
  Derived data: never authored, never serialized.
- **`RelationshipComponent`** — `Parent` UUID (0 = root) + `Children` UUID list.
  **Change-tracked** (re-parenting moves the subtree).

### Rendering

- **`SpriteRendererComponent`** — 2D quad color (drawn by Renderer2D).
- **`StaticMeshComponent`** — `AssetHandle` of a mesh (see [assets.md](assets.md)).
- **`CameraComponent`** — a `SceneCamera` (perspective or orthographic) + `Primary` +
  `FixedAspectRatio`. The first primary camera wins (resolved once per update by `CameraSystem`).
- **`DirectionalLightComponent`** — color/intensity/`CastShadows`; direction is the entity's
  world −Z. The first shadow-casting one drives the cascaded shadow maps.
- **`PointLightComponent`** — color, intensity, radius, falloff.
- **`SpotLightComponent`** — color, intensity, range, inner/outer cone half-angles (radians),
  falloff.
- **`SkyLightComponent`** — environment `AssetHandle` (HDR IBL when valid) or procedural
  hemispheric sky/ground colors; intensity; `DrawSkybox`. First one wins.

### Scripting

- **`NativeScriptComponent`** — function pointers to instantiate/destroy a
  [`ScriptableEntity`](../../GanymedEngine/source/GanymedE/Scene/ScriptableEntity.h) subclass plus
  the live `Instance`. Bind with `entity.AddComponent<NativeScriptComponent>().Bind<MyScript>();`.
  Flagged `EnableInit`+`EnableFini` so `NativeScriptSystem` reacts declaratively to scripts
  appearing/disappearing. Script hooks: `OnCreate`, `OnUpdate(ts)`, `OnDestroy`,
  `OnCollisionEnter/Exit(Entity other)`.
- **`ScriptComponent`** — an `AssetHandle` to a `.lua` asset, and nothing else: every sol2 object
  lives in `ScriptEngine`, keyed by UUID. Same `EnableInit`+`EnableFini` flags and the same hook
  names as the native path, driven by `LuaScriptSystem`. Details: [scripting.md](scripting.md).

### Physics (pure data — Jolt never appears here)

- **`RigidBodyComponent`** — `Static | Dynamic | Kinematic`, mass, linear/angular damping,
  `UseGravity`.
- **`BoxColliderComponent`** (half extents), **`SphereColliderComponent`** (radius),
  **`CapsuleColliderComponent`** (radius + half height) — each with a local `Offset` and a
  `PhysicsMaterial { Friction, Restitution }`.

## Built-in systems

Registered in this order (order = execution order; validated against declared access):

### PhysicsSystem — [`Systems/PhysicsSystem.h`](../../GanymedEngine/source/GanymedE/Scene/Systems/PhysicsSystem.h)
Owns the `PhysicsScene` (created on play, destroyed on stop) and the fixed-timestep accumulator.
Per update: step the world at `PhysicsSettings.FixedTimestep` (capped at `MaxStepsPerFrame` —
spiral-of-death guard, dropping surplus time), dispatch collision events to script instances via an
`AccessView`, then `SyncTransforms(alpha)` writes interpolated poses back into
`TransformComponent`s. Declares `AccessView<RW<TransformComponent>>` purely so ordering validation
knows this system writes transforms (the actual writeback is inside `PhysicsScene`).
Details: [physics.md](physics.md).

### NativeScriptSystem — [`Systems/NativeScriptSystem.h`](../../GanymedEngine/source/GanymedE/Scene/Systems/NativeScriptSystem.h)
Script lifecycle, declaratively: an `InitView` instantiates scripts that appeared, a `FiniView`
calls `OnDestroy` + deletes instances that went away (including via entity destruction — the slot
reads the graveyard copy), a plain `IterView` runs `OnUpdate`. The editor path drains the reactive
views without instantiating (they must be read every update), which also cleans up instances if a
script component is removed in edit mode. `OnRuntimeStop` sweeps all live scripts (stopping play is
not a component removal, so FiniView never sees it).

### LuaScriptSystem — [`Systems/LuaScriptSystem.h`](../../GanymedEngine/source/GanymedE/Scene/Systems/LuaScriptSystem.h)
The same three-view lifecycle as `NativeScriptSystem`, for `ScriptComponent`, delegating to the
global `ScriptEngine` VM. Additionally declares an unused `AccessView<RW<TransformComponent>>` so
`ValidateOrdering` knows script bindings write transforms outside any view — which is why it is
registered before `TransformSystem`. Details: [scripting.md](scripting.md).

### TransformSystem — [`Systems/TransformSystem.h`](../../GanymedEngine/source/GanymedE/Scene/Systems/TransformSystem.h)
Maintains the `WorldTransformComponent` cache. A `ChangeView` reacting to `TransformComponent` and
`RelationshipComponent` yields only entities that actually moved/re-parented; for each, the world
matrix is recomputed from locals up the parent chain (never from a possibly-stale parent cache) and
pushed down the subtree, with a visited set making overlapping dirty entries idempotent. Runs in
both edit and play mode. An idle scene recomputes **zero** matrices.

### CameraSystem — [`Systems/CameraSystem.h`](../../GanymedEngine/source/GanymedE/Scene/Systems/CameraSystem.h)
Resolves "which camera renders this frame" once into the `RenderContext` singleton: the first
`CameraComponent` with `Primary` wins, paired with its cached world transform. In edit mode it
clears `MainCamera` (the editor camera renders instead). `MainCamera` points into a live component
— it is rewritten every update and must never be held across frames.

### RenderSystem — [`Systems/RenderSystem.h`](../../GanymedEngine/source/GanymedE/Scene/Systems/RenderSystem.h)
Pure submission — everything that used to be inlined in `Scene::OnUpdate*`. Reads `RenderContext`,
begins Renderer3D with the main camera (or the editor fallback), submits lights, sky/environment,
meshes, collider gizmos (or Jolt debug draw when enabled during play), ends the scene, then does the
2D pass (sprites) in its own render view. The editor path additionally draws the grid. Its nine view
declarations are live documentation of exactly what rendering reads.

## Singletons

Scene-wide state that is genuinely singular lives in `registry.ctx()`
([`SceneSingletons.h`](../../GanymedEngine/source/GanymedE/Scene/SceneSingletons.h)), accessed via
singleton views (systems) or `Scene::GetSingleton/FindSingleton/SetSingleton` (tooling):

- **`RenderContext`** — `MainCamera` + `CameraTransform` (resolved per update by CameraSystem) and
  `EditorViewCamera` (the editor's camera: the view camera in edit mode, the fallback in play
  mode). Change-tracked (`SingletonTraits<RenderContext>::TrackChanges`).
- **`PhysicsSettings`** — `DebugDraw` toggles, `FixedTimestep` (1/60), `MaxStepsPerFrame` (5).

## Serialization

[`SceneSerializer`](../../GanymedEngine/source/GanymedE/Scene/SceneSerializer.h) writes/reads YAML
`.ganymede` files: a `Scene` name plus an `Entities` sequence, each entity a map of component
blocks keyed by component name. Notes:

- Entity identity is the real UUID; deserialization mints a fresh UUID on `0` or collision
  (legacy scenes serialized one hardcoded ID for every entity).
- Asset references serialize as **handles** (`uint64_t`); `MeshPath`/`EnvironmentPath`/`ScriptPath`
  string fallbacks are still read for backward compatibility and imported into the registry on load.
  Unlike meshes, a deserialized `ScriptComponent` handle is *not* warmed through `GetAsset<>` —
  scripts have no runtime object to cache, and `ScriptEngine` loads the chunk on instantiation.
- `WorldTransformComponent` is intentionally not serialized (derived).
- Adding a component type means extending both `SerializeEntity` and `Deserialize` — this is one
  of the two remaining hand-maintained per-component lists (the other is the editor UI).
- `SerializeRuntime`/`DeserializeRuntime` (binary) are unimplemented stubs.

## Play mode

The editor's play button (see [editor.md](../editor/editor.md)) runs:

```
OnScenePlay: m_ActiveScene = Scene::Copy(m_EditorScene);  ActiveScene->OnRuntimeStart();
OnSceneStop: ActiveScene->OnRuntimeStop();  m_ActiveScene = m_EditorScene;
```

The runtime scene is a disposable deep copy keyed by UUID — physics can knock everything over and
Stop simply discards the copy. This is why stable UUIDs and the generic `ComponentList` copy exist.

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
| `DestroyEntity(entity)` | Detaches from parent, unparents children (they stay as roots — each orphan gets `MarkChanged<RelationshipComponent>`, or its cached world transform would keep the destroyed parent's contribution), erases the UUID mapping, destroys. Asserts if called during a system update — use `Commands().DestroyEntity()` there |
| `OnRuntimeStart/Stop` | Forwarded to the systems (start runs in reverse registration order — see [ecs.md](ecs.md#systemmanager)) |
| `OnUpdateRuntime(ts, fallbackCamera)` / `OnUpdateEditor(ts, camera)` | FrameBegin → systems → FrameEnd; the editor camera is passed via the `RenderContext` singleton |
| `OnViewportResize(w, h)` | Updates all non-fixed-aspect `CameraComponent`s |
| `Copy(other)` | Play-mode snapshot: recreate entities by UUID, then copy every `ComponentList` component via `ForEachType`; script `Instance` pointers are nulled so runtime instances are recreated on play |
| `GetWorldSpaceTransform(entity)` | Walks the parent chain from locals — for **editor/tooling** (gizmos). Renderable code reads the cached `WorldTransformComponent` instead |
| `SetParent(child, parent)` / `Unparent(child)` | Maintains both sides of the relationship, rejects self/descendant parenting, and calls `MarkChanged<RelationshipComponent>` so the transform cache reacts |
| `MarkChanged<T>(entity)` | Report an out-of-view write of a tracked component (see [ecs.md](ecs.md#accessors-and-the-modify-invariant)) |

`Scene`'s constructor wires the entt signals for tracked/init/fini component types, creates the
`RenderContext` and `PhysicsSettings` singletons, registers the eight built-in systems, and asserts
`ValidateOrdering()` passes.

## Entity

[`Entity`](../../GanymedEngine/source/GanymedE/Scene/Entity.h) is a value-type handle
(`entt::entity` + `Scene*`). `AddComponent`/`RemoveComponent` are **immediate** and assert when
called during a system update; `GetComponent`/`HasComponent` are always fine. `GetUUID()` and
`GetName()` read `IDComponent`/`TagComponent`. A default-constructed `Entity` is falsy.

`AddComponent` calls `Scene::OnComponentAdded<T>`, whose primary template does nothing; only
components needing post-add fixup are specialized (currently just `CameraComponent`, to seed the
viewport size). That hook takes `const Entity&` rather than `Entity` by value on purpose:
`Entity.h` includes `Scene.h`, so `Entity` is only forward-declared where the primary template is
*defined*, and a function definition requires complete parameter types. A reference to an
incomplete type is legal, by value is not — MSVC accepts the by-value form as an extension, gcc
and clang reject it outright.

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
- **`StaticMeshComponent`** — `AssetHandle` of a mesh (see [assets.md](assets.md)). Also carries
  skinned meshes: there is no separate `SkinnedMeshComponent`, because the asset already knows
  whether it has a skeleton and a second component would duplicate the drag-drop, serialization,
  inspector and `RenderSystem` plumbing to say nothing new.
- **`AnimatorComponent`** — `Clip` (by name), `Speed`, `Playing`, `Loop`, `Time`, and a runtime
  `Palette` of joint matrices. An entity is skinned iff its mesh `HasSkeleton()` *and* it has an
  animator. Clips are named rather than indexed because indices shift whenever a DCC reorders or
  adds a clip on re-export; the cost is that a rename detaches the reference silently, which
  `AnimationSystem` compensates for by warning once and holding the bind pose. `Time` and `Palette`
  are not serialized — a scene loads at the head of its clip, and the palette is rebuilt per frame.
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
- **`ScriptComponent`** — an `AssetHandle` to a `.lua` asset plus `Fields`, a map of per-entity
  property overrides (`bool`/`double`/`string`/`vec3`); every sol2 object still lives in
  `ScriptEngine`, keyed by scene and UUID. Only overridden values are stored, so editing a default
  in the script reaches entities that never changed it. Same `EnableInit`+`EnableFini` flags and
  the same hook names as the native path, driven by `LuaScriptSystem`.
  Details: [scripting.md](scripting.md).

### Audio (pure data — miniaudio never appears here, and neither does the live voice)

- **`AudioSourceComponent`** — an `AssetHandle` to a `.wav/.mp3/.flac` asset, plus `Volume`,
  `Pitch`, `Loop`, `PlayOnStart`, `Spatialize`, `Stream` and an `AudioGroup`
  (`Master | Music | SFX`). **No `VoiceId`, no "is playing" flag**: the live voice belongs to
  `AudioSystem`'s map, keyed by entity, exactly as a Jolt body belongs to `PhysicsScene`. That is
  the deliberate opposite of `AnimatorComponent::Palette`, and the difference is *what the field
  is* — a palette is pure data one system produces and another reads, a voice is a foreign
  resource with a lifecycle. The payoff: `Scene::Copy` needs no audio fixup at all.
  `Clip`, `Spatialize` and `Stream` are read when the voice is built and ignored afterwards;
  the rest are pushed every frame.
- **`AudioListenerComponent`** — one `Primary` bool. Absent from the scene entirely, the primary
  camera's pose becomes the listener. Details: [audio.md](audio.md).

Both untracked: `AudioSystem` polls them every frame, so change tracking would buy nothing, and
untracked means a script setter needs no `MarkChanged` to be heard — the `AnimatorComponent`
precedent.

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

### AnimationSystem — [`Systems/AnimationSystem.h`](../../GanymedEngine/source/GanymedE/Scene/Systems/AnimationSystem.h)
Samples each `AnimatorComponent`'s clip and leaves a joint palette on the component. Per animator:
advance `Time` (wrapped or clamped by clip duration), binary-search each channel's key pair and
interpolate — lerp for translation/scale, **slerp** for rotation, `Step` holding the left key —
over a copy of the skeleton's rest pose, so joints and paths the clip does not drive keep their
authored transform. Globals are then composed in a single forward pass (the importer sorts joints
parents-before-children so no recursion is needed), seeded from `Skeleton::RootTransform` rather
than identity, giving `Palette[i] = Global[i] * InverseBind[i]`.

An unresolvable clip name warns once per distinct name and holds the bind pose; a missing skeleton
clears the palette, which is also the signal to the renderer to use the static path. Scratch pose
and global arrays are system members reused across entities and frames.

**Runs in edit mode, but samples without advancing.** Evaluating poses is what makes the
inspector's Time scrub move the model; running the clock as well would leave every rig in the scene
permanently in motion while placing things, and `Time` is not serialized so it would drift with no
record. `OnRuntimeStart` resets every animator's `Time` to zero, so play begins at the head of the
clip whatever the editor was scrubbed to — the editor scene is a separate copy and keeps its scrub
position for when play stops.

Its registration slot (after the script systems, before `TransformSystem`) is a documented intention
that `ValidateOrdering` cannot enforce; the part it does enforce is running ahead of `RenderSystem`,
which reads the palette. See [ecs.md](ecs.md#systemmanager).

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

### AudioSystem — [`Systems/AudioSystem.h`](../../GanymedEngine/source/GanymedE/Scene/Systems/AudioSystem.h)
Drives the [AudioEngine](audio.md) from the scene. `OnRuntimeStart` builds a voice for every
`PlayOnStart` source (handle → path through `AssetManager::GetMetadata`, the Script precedent) and
starts it; `OnUpdate` pushes `Volume`/`Pitch`/`Loop` and, for spatial sources, the entity's world
position, then resolves the listener; `OnRuntimeStop` destroys every voice and calls `StopAll`.
Voices live in an `unordered_map<entt::entity, VoiceId>` that exists only between play and stop.

Listener resolution: the first `Primary` `AudioListenerComponent` wins, with a one-time warning if
there is more than one (the unknown-clip-name posture — loud, not broken). With none in the scene it
falls back to `RenderContext::CameraTransform`, logged once. **That fallback is why the system is
registered after `CameraSystem`** even though the two share no component: running after means the
fallback ears are this frame's pose, not last frame's. `ValidateOrdering` cannot see that
dependency, so the registration site carries the reason in a comment.

Nothing frees a non-looping voice that reaches its end. `AudioEngine::Stop` is pause-with-cursor and
`Play` rewinds, so a finished voice costs one paused sound and makes the next play instant — the
alternative trades that for a re-decode on every replay.

**Edit mode is silent**: there is no `OnUpdateEditor` override at all. This *follows* the engine's
"systems simulate only in play mode" norm; state it next to `AnimationSystem`, which deliberately
diverges from that norm so the inspector can scrub a pose. Two decisions, not an accident.

### RenderSystem — [`Systems/RenderSystem.h`](../../GanymedEngine/source/GanymedE/Scene/Systems/RenderSystem.h)
Pure submission — everything that used to be inlined in `Scene::OnUpdate*`. Reads `RenderContext`,
begins Renderer3D with the main camera (or the editor fallback), submits lights, sky/environment,
meshes, collider gizmos (or Jolt debug draw when enabled during play), ends the scene, then does the
2D pass (sprites) in its own render view. The editor path additionally draws the grid. Its nine view
declarations are live documentation of exactly what rendering reads.

Two play-mode policies live in this system:

- **Collider gizmos are opt-in.** With Jolt debug draw off, the authored-collider wireframes are
  drawn only when `PhysicsSettings::ShowColliderGizmos` is set. It defaults **false**, so a shipped
  game never draws them; the editor sets it true. (This used to fall through unconditionally, which
  meant any non-editor front-end drew collider wireframes over the game.) Edit mode calls
  `DrawColliderGizmos()` directly and is unaffected.
- **No camera is loud, not silent.** With no primary camera *and* no fallback, the frame is the
  scene target's clear colour and the system logs an error at most once every 5 s. Throttled rather
  than per-frame: a 60 Hz error would bury everything else in the log to say the same thing.

The mesh view carries `OptRO<AnimatorComponent>`, so one iteration covers both draw paths: an
entity with an animator, a mesh that `HasSkeleton()`, and a non-empty palette goes to
`Renderer3D::SubmitSkinnedMesh`, everything else to `SubmitMesh`. A rigged mesh with no animator
therefore draws as static geometry in its bind pose, which is the sane result of dropping a
character into a scene before authoring anything. Declaring that optional read is also what made
the `AnimationSystem`-before-`RenderSystem` ordering checkable at last — see
[ecs.md](ecs.md#systemmanager).

## Singletons

Scene-wide state that is genuinely singular lives in `registry.ctx()`
([`SceneSingletons.h`](../../GanymedEngine/source/GanymedE/Scene/SceneSingletons.h)), accessed via
singleton views (systems) or `Scene::GetSingleton/FindSingleton/SetSingleton` (tooling):

- **`RenderContext`** — `MainCamera` + `CameraTransform` (resolved per update by CameraSystem) and
  `EditorViewCamera` (the editor's camera: the view camera in edit mode, the fallback in play
  mode). Change-tracked (`SingletonTraits<RenderContext>::TrackChanges`). *Known misnomer:* now that
  a non-editor host exists, this field is really "fallback view camera" and is simply null there.
  Flagged as debt rather than renamed — the rename ripples through docs and editor for zero behaviour
  change.
- **`PhysicsSettings`** — `DebugDraw` toggles, `ShowColliderGizmos`, `FixedTimestep` (1/60),
  `MaxStepsPerFrame` (5).

**Singletons are not carried by `Scene::Copy`.** The copy constructs a fresh `Scene`, whose
constructor default-constructs its own `ctx()` entries, and then copies entities and components only.
Anything a host needs true on the play-mode scene must be (re)written after the copy — which is why
`EditorLayer` pushes `DebugDraw` *and* `ShowColliderGizmos` onto the active scene every play frame
rather than once on play.

## Serialization

[`SceneSerializer`](../../GanymedEngine/source/GanymedE/Scene/SceneSerializer.h) writes/reads YAML
`.ganymede` files: a `Scene` name plus an `Entities` sequence, each entity a map of component
blocks keyed by component name. Notes:

- **Save order is canonical: roots sorted by UUID, then depth-first through each root's `Children`
  in authored sibling order.** Saving the same scene twice produces byte-identical files, and a
  play/stop cycle does not change them. Before this the order was `view<IDComponent>` — entt's
  packed order, which entt 3.16 iterates backwards and `Scene::Copy` reshuffles wholesale, so
  re-saving an untouched scene produced a large meaningless diff and "did this edit change
  anything?" was unanswerable.

  A flat UUID sort would also be deterministic and would give tighter diffs (entities never move
  in the file, so a reparent touches only Relationship fields). DFS was chosen anyway: a subtree
  comes out as a *contiguous block*, which is the layout `.gprefab` needs, so both formats share
  one canonical order instead of having two; and sibling order is authored, user-visible state, so
  letting it order the file makes the layout content rather than an artifact. The cost, accepted:
  reparenting moves a block in the diff. Godot orders scene files by node path for the same
  reasons; Unity instead leans on stable fileIDs and keeps insertion order.

  The walk carries a visited set, so a corrupted hierarchy (a cycle, a child listed under two
  parents) terminates. Any entity reachable from no root is appended in UUID order **with a
  warning** — that combination is deliberate: corruption becomes visible instead of becoming
  silent data loss.
- Entity identity is the real UUID; deserialization mints a fresh UUID on `0` or collision
  (legacy scenes serialized one hardcoded ID for every entity), and **`ResolveHierarchy` then
  re-points every `Parent`/`Children` reference at the UUIDs the entities were actually created
  with**. `DeserializeEntity` writes the *file's* UUIDs into `RelationshipComponent` verbatim and
  the batch owner resolves them once all the entities exist. Without that pass a remapped entity
  kept a hierarchy naming its old UUID, and those references silently resolved to whichever entity
  won the original — a severed or mis-attached subtree with no diagnostic. The pass runs on every
  load, not only after a collision: with no remapping it is the identity, and a repair path that
  only runs on rare input is a path that rots. `Children` is authoritative (a child claimed by a
  parent's list takes that parent); an entity naming a parent that does not list it keeps a
  translated reference and warns.
- Asset references serialize as **handles** (`uint64_t`); `MeshPath`/`EnvironmentPath`/`ScriptPath`
  string fallbacks are still read for backward compatibility and imported into the registry on load.
  Unlike meshes, a deserialized `ScriptComponent` handle is *not* warmed through `GetAsset<>` —
  scripts have no runtime object to cache, and `ScriptEngine` loads the chunk on instantiation.
  Its property overrides serialize as a `Fields` sequence of `{Name, Type, Value}`, sorted by name
  so a scene file does not churn when a hash map reorders. Each carries its own type because the
  declaring script may not be loadable when the scene is read back.
- **`AudioGroup` serializes as a name, not an ordinal** (`Group: Music`). It is not persisted in the
  asset registry the way `AssetType` is, so nothing forces stable numbering on it, and an unknown
  name warns and falls back rather than throwing. Both audio components read every field guarded
  (`if (node["Volume"])`) rather than bare `as<T>()` — hand-authored scenes are a normal way to
  make one, and an absent key in a bare read throws out through `Deserialize` and loses the whole
  file.
- `WorldTransformComponent` is intentionally not serialized (derived).
- Adding a component type means extending both `SerializeEntity` and `DeserializeEntity` — this is
  one of the two remaining hand-maintained per-component lists (the other is the editor UI).
- **The per-entity halves are public statics**: `SerializeEntity(YAML::Emitter&, Entity)` and
  `DeserializeEntity(const YAML::Node&, Scene&, UUID)`. `Deserialize` is a loop over the second
  plus `ResolveHierarchy`. They are static and take their scene explicitly because a `.gprefab` is
  the same entity blocks under a different root, read into a scene the serializer does not own.
  `DeserializeEntity` does not read the UUID from the node: who owns that decision differs per
  container — a scene keeps the file's UUID and remaps collisions, a prefab instance always mints
  a fresh one.
- `SerializeRuntime`/`DeserializeRuntime` (binary) are unimplemented stubs.
- **`Deserialize` never throws.** It checks the file exists, then wraps the parse in one try/catch
  and returns `false` on any `YAML::Exception`, logging the file and the reason. This matters more
  than it sounds: yaml-cpp throws not only on malformed documents but on *every* `as<T>()` whose
  node is missing, mistyped, or out of range — a hand-authored 20-digit UUID overflowing `uint64_t`
  is how this was found, and unhandled it terminated the process before a single frame. For a
  shipped game that is the worst available failure mode: no window, no message, just an exit code.
  The scene is left partially populated rather than rolled back, so the caller chooses whether to
  discard it; a half-loaded scene is still inspectable in the editor. The split into a private
  `DeserializeUnchecked` exists only so the try block does not re-indent every component branch.

## Play mode

The editor's play button (see [editor.md](../editor/editor.md)) runs:

```
OnScenePlay: m_ActiveScene = Scene::Copy(m_EditorScene);  ActiveScene->OnRuntimeStart();
OnSceneStop: ActiveScene->OnRuntimeStop();  m_ActiveScene = m_EditorScene;
```

The runtime scene is a disposable deep copy keyed by UUID — physics can knock everything over and
Stop simply discards the copy. This is why stable UUIDs and the generic `ComponentList` copy exist.

The generic copy is a shallow value copy of every component, so anything that is runtime-only needs
an explicit fixup sweep after it. There are two: `NativeScriptComponent::Instance` is nulled so
instances are recreated on play, and `AnimatorComponent::Palette` is cleared because carrying a
per-joint matrix array per entity into the new scene buys one frame of stale data. Adding a
component with runtime-only state means adding a third sweep — nothing enforces this.

The audio components are the worked example of *not* needing one. Putting the live `VoiceId` on
`AudioSourceComponent` would have made a third sweep mandatory and would have let a copied scene
double-drive one sound; keeping the voice in `AudioSystem`'s map means the copy is correct with no
audio code involved at all. Where a component's runtime state goes is a `Scene::Copy` decision as
much as an ownership one.

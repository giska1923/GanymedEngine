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
| `DuplicateEntity(source)` | Deep-copies an entity and its descendants with **fresh** UUIDs, attaching the copy as a *sibling* of the source. `IDComponent` is minted; `RelationshipComponent` and `BoneAttachmentComponent::Target` are remapped when they name entities inside the copy — see below |
| `CollectSubtree(root, out, visited)` | `root` plus its descendants, depth-first through each `Children` in authored order: the canonical order the scene and prefab formats both save in. The visited set keeps a corrupted hierarchy from becoming an infinite walk |
| `Copy(other)` | Play-mode snapshot: recreate entities by UUID, then copy every `ComponentList` component via `ForEachType`; script `Instance` pointers are nulled so runtime instances are recreated on play |
| `GetWorldSpaceTransform(entity)` | Walks the parent chain from locals — for **editor/tooling** (gizmos). Renderable code reads the cached `WorldTransformComponent` instead. A resolved socket is the exception: local TR is ignored, so the socket gizmo reads `WorldTransformComponent::World` that `BoneAttachmentSystem` wrote this frame |
| `SetParent(child, parent)` / `Unparent(child)` | Maintains both sides of the relationship, rejects self/descendant parenting, and calls `MarkChanged<RelationshipComponent>` so the transform cache reacts |
| `MarkChanged<T>(entity)` | Report an out-of-view write of a tracked component (see [ecs.md](ecs.md#accessors-and-the-modify-invariant)) |

**The `DuplicateEntity` whitelist is the point, not an optimization.** `AssetHandle` *is* `UUID` —
the same C++ type — so a "rewrite every UUID-typed field" pass would happily renumber
`Script.Script`, `AudioSource.Clip` and `PrefabInstance.Source` into handles no registry knows, and
the entity would render nothing with no diagnostic. The mesh, material and environment fields are
`AssetRef<T>` now and are no longer even the same type, which narrows the hazard without removing
it — the three path-resolved references above are still bare handles by design. Here the whitelist is
structural rather than a list to maintain: `IDComponent` comes from `CreateEntityWithUUID`,
`RelationshipComponent` is rewritten explicitly afterwards, `BoneAttachmentComponent::Target` is
looked up in the **entity** remap (asset handles will not be keys of that map), and every other
component is copied verbatim by the `ForEachType(ComponentList)` loop. The copy is made in two passes because a
parent's `Children` names entities created later in the walk.

`Scene`'s constructor wires the entt signals for tracked/init/fini component types, creates the
`RenderContext`, `PhysicsSettings` and `EditorViewFilter` singletons, registers the ten built-in systems, and asserts
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
- **`StaticMeshComponent`** — `AssetRef<Mesh>` plus `MaterialOverrides`, a
  `std::vector<AssetRef<Material>>` parallel to the mesh's own material list (see
  [assets.md](assets.md#assetreft)). Also carries
  skinned meshes: there is no separate `SkinnedMeshComponent`, because the asset already knows
  whether it has a skeleton and a second component would duplicate the drag-drop, serialization,
  inspector and `RenderSystem` plumbing to say nothing new.
- **`AnimatorComponent`** — `Clip` (by name), `Speed`, `Playing`, `Loop`, `Time`, and a runtime
  `Palette` of joint matrices. The animator is what *plays* a clip. Drawing a rigged mesh does not
  require one: `RenderSystem` skins any mesh that `HasSkeleton()`, using `Mesh::GetRestPalette()`
  when no clip palette exists. `SubmitMesh` is not a bind-pose equivalent once `LocalTransform` is
  a unit conversion (Meshy: vertices in metres, joints in centimetres, mesh node 0.01) — the rest
  palette is ~scale 100 and is what cancels that scale. Clips are named rather than indexed because
  indices shift whenever a DCC reorders or adds a clip on re-export; the cost is that a rename
  detaches the reference silently, which `AnimationSystem` compensates for by warning once and
  holding the bind pose. `Time` and `Palette` are not serialized — a scene loads at the head of
  its clip, and the palette is rebuilt per frame.
- **`BoneAttachmentComponent`** — pins this entity to a named joint of another entity's skinned
  mesh. `Target` is an entity UUID (zero = hierarchy parent); `Joint` is a name, for the same
  reason clips are; `Offset` / `Rotation` are the rest pose in joint space (Euler radians, X·Y·Z).
  `Resolved` is a runtime index, not serialized, reset by `Scene::Copy`. The joint frame is
  `TryGetJointFrame` ([`Mesh.h`](../../GanymedEngine/source/GanymedE/Renderer/Mesh.h)) — one
  function, so a visualizer cannot re-derive the formula and drift. Recovery rather than
  keeping AnimationSystem's scratch globals: attachments are counted in ones and twos, and a
  second per-joint array would add 2–8 KB per animated entity for `Scene::Copy` to shuffle on
  every play. Writes `WorldTransformComponent` directly: feeding a joint quaternion through
  `TransformComponent`'s Euler storage is lossy. Local translation and rotation are ignored while
  the socket resolves — `Offset`/`Rotation` are what replace them — but local **`Scale`** is kept:
  nothing on this component replaces it, and reading one field on both the attached and the
  restored path means a socket that fails to resolve shows a correctly *sized* prop rather than a
  compensated one. `Offset` is in the target mesh's own units — metres for a mesh authored that
  way — whatever unit the *rig* uses; the system folds in the skinned submesh's `LocalTransform`
  and divides the bind pose's basis scale back out (below). What the clip does to the joint chain
  still carries, scale included.
- **`CameraComponent`** — a `SceneCamera` (perspective or orthographic) + `Primary` +
  `FixedAspectRatio`. The first primary camera wins (resolved once per update by `CameraSystem`).
- **`DirectionalLightComponent`** — color/intensity/`CastShadows`; direction is the entity's
  world −Z. The first shadow-casting one drives the cascaded shadow maps.
- **`PointLightComponent`** — color, intensity, radius, falloff.
- **`SpotLightComponent`** — color, intensity, range, inner/outer cone half-angles (radians),
  falloff.
- **`SkyLightComponent`** — `AssetRef<Environment>` (HDR IBL when it resolves) or procedural
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

### Particles

- **`ParticleEmitterComponent`** — CPU emitter: rate, cap, looping/duration, cone (+Y, aimed by
  the entity transform), min/max lifetime/speed/size/rotation, gravity modifier, simulation space,
  seed, `FloatCurve` size multiplier, `ColorGradient` over lifetime, and billboard vs mesh
  render settings. `ParticleBlend` is component-local (`Alpha`/`Additive`) so `RenderState.h` does
  not leak bgfx defines into this header. Runtime state (`Playing`, `Time`, `EmitAccumulator`,
  `BurstPending`, `Rng`, `Pool`, `WorldBounds`) is not serialized. `Scene::Copy` calls `ResetRuntime()` — pool,
  accumulator, timer, Playing, burst queue, bounds, **and RNG together** — so play mode starts empty and
  emitters warm up (`LifetimeMax` seconds to steady state, Unity without prewarm). `Seed = 0`
  derives from the entity UUID when playback starts, so two prefab instances do not march in
  lockstep; a nonzero seed pins the sequence for the replay instrument. Untracked: the sim
  polls every tick. Inspector Play / Stop / Restart (`PlayPreview` / `StopPreview` /
  `RestartPreview`) and Lua `PlayParticles` / `StopParticles` / `EmitBurst` are runtime-only: not
  serialized, not undoable. `PlayPreview` is a no-op if
  already playing. RNG reseeds only from a fresh state (`Time == 0` and empty pool) — Stop then
  Play resumes the same stream; Restart calls `ResetRuntime` and seeds itself because
  `ParticleSystem` has already run that frame. `EmitBurst` accumulates into `BurstPending` and
  does not auto-play; consume is this tick while `Playing` and is not gated by `Duration`.

### Map authoring

- **`ScatterGroupComponent`** — folder entity for Map-panel scatter. `Source` is the prefab or
  mesh handle that was painted; `LastSeed` is the last paint stroke's seed so it can be typed
  back into the brush. Inert at runtime: identity for the eraser (direct children of this
  entity) and a seed note, not a foliage instance array. Each child is an ordinary entity. Not
  in the Add Component menu; the Map panel creates it. See
  [editor.md](../editor/editor.md#map-panel).
- **`MarkerComponent`** — spawn / patrol / trigger (or whatever string the game invents). `Kind`
  is a string because the branch policy forbids the game from touching engine headers; an enum
  would make "Patrol" an engine change. `Color`, `Size` and `DrawForward` are editor
  visualization. Inert at runtime except as the query target for `Scene.FindMarkers` / 
  `Entity:GetMarkerKind()`. Wait times and teams stay on `ScriptComponent`. In Add Component.
  See [editor.md](../editor/editor.md#map-panel) and [scripting.md](scripting.md).

### Physics (pure data — Jolt never appears here)

- **`CharacterControllerComponent`** — a walking character (Jolt `CharacterVirtual`): max slope,
  step height, stick-to-floor, mass. Takes its shape from a capsule collider, and replaces rather
  than accompanies a rigid body. It is still detectable — raycasts hit it and it raises
  `OnCollisionEnter` — through an inner body the engine always gives it, which is not authored
  here because there is nothing to choose. See [physics.md](physics.md#character-controllers).
- **`RigidBodyComponent`** — `Static | Dynamic | Kinematic`, mass, linear/angular damping,
  `UseGravity`, `LockRotation` (forbids rotation while keeping translation — what an upright
  walking capsule needs; see [physics.md](physics.md#locked-rotation)), and `IsSensor` (a trigger
  volume: reports contacts, causes none — see [physics.md](physics.md#sensors-trigger-volumes)).
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
advance `Time` (wrapped or clamped by clip duration), then `SampleClipGlobals` — binary-search each
channel's key pair and interpolate — lerp for translation/scale, **slerp** for rotation, `Step`
holding the left key — over a copy of the skeleton's rest pose, so joints and paths the clip does
not drive keep their authored transform. Globals are composed in a single forward pass (the importer
sorts joints parents-before-children so no recursion is needed), seeded from
`Skeleton::RootTransform` rather than identity. The palette is then `Palette[i] = Global[i] *
InverseBind[i]`. The clip inspector samples the same globals (head / hips / root at a given time)
so a visualizer cannot re-derive the sampler and drift.

An unresolvable clip name warns once per distinct name and holds the bind pose; a missing skeleton
clears the palette. `RenderSystem` then uses `Mesh::GetRestPalette()` if the mesh still has a
skeleton, or `SubmitMesh` if it does not. Scratch pose and global arrays are system members reused
across entities and frames.

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
both edit and play mode. An idle scene recomputes **zero** matrices. `OverrideWorld(entity, world)`
is a second pass used by `BoneAttachmentSystem`: it clears the visited set (the dirty pass has
already marked every touched entity) and pushes `world` down the subtree so children of a socketed
entity track the socket. The cache-stomp risk is accepted; that system is the one caller.

### BoneAttachmentSystem — [`Systems/BoneAttachmentSystem.h`](../../GanymedEngine/source/GanymedE/Scene/Systems/BoneAttachmentSystem.h)
Pins entities with `BoneAttachmentComponent` to a joint. Per socket, in hierarchy-depth order
(so a nested attachment sees its target's already-rewritten world; an explicit `Target` that is
itself socketed is treated as deeper still): resolve the target (zero = parent), re-resolve
`Joint` by name against the target's skeleton when `Resolved` is stale, then
`TryGetJointFrame(mesh, palette, joint, jointGlobal)` and
`OverrideWorld(entity, targetWorld * jointGlobal * offset * localScale)`.
The frame function is the one owner of
`jointGlobal = skinnedSubmesh.LocalTransform * Palette[i] * inverse(InverseBind[i])` plus
**dividing the bind pose's basis scale out of it**.
A missing target, a mesh with no
palette, a singular inverse bind, or a joint name the skeleton does not have warns once per
distinct failure and leaves the entity at its **parent** transform (parent cache × local), never
at the origin. An empty joint is quiet — authoring a socket before picking a name.

**Why `LocalTransform` is in there.** `Renderer3D` draws a skinned submesh as
`entityWorld * LocalTransform * Palette * v` — the importer deliberately keeps the mesh node's
transform on skinned submeshes because `RootTransform` carries its inverse, and the two cancel.
A socket has to ride that same chain or it is not in the same space as the mesh it is pinned to.
`Palette[i] * inverse(InverseBind[i])` recovers the joint global *alone*, which is in whatever
unit the **joints** were authored in — and that need not be the unit the **vertices** are in.
Measured on a Meshy rig: vertices 1.797 units tall (metres), `RightHand` at y = 141.4
(centimetres), `LocalTransform` = 0.01 bridging them. Omitting it put the socket at 141 *metres*
instead of 1.41 — a correctly sized prop, far enough away to look tiny, which is an easy thing to
misread as a scale bug and "fix" with a compensating scale.

**Why the basis is then divided back out.** `inverse(InverseBind[i])` *is*
`LocalTransform * bindGlobal`, so it is exactly this frame in the bind pose: translation in the
mesh's units, basis carrying `LocalTransform`'s scale. For vertices that scale is cancelled by the
matching factor inside the palette; for a socket nothing cancels it, so left in, an attached entity
renders at 1% with `Offset` silently in centimetres. Each basis column is therefore divided by the
length of the matching column of `inverse(InverseBind[i])`. Dividing by the *bind* scale rather
than normalising to unit length is what keeps animated scale: the palette's scale is relative to
bind, so only the bind part is the artifact, and a clip that scales a hand still scales what the
hand is holding. When the mesh node is identity every column is already 1 and the loop is a no-op.

**Runs in edit mode**, following the pose `AnimationSystem` already sampled. A weapon on a hand,
or a camera on a head, has to move when the inspector scrubs `Time`.

Its slot after `TransformSystem` and before `CameraSystem` is enforced against `CameraSystem`
(both declare `WorldTransformComponent`; the later one only reads it). The palette read against
`AnimationSystem` is also checked. Two writers of world (this and `TransformSystem`) are
invisible to `ValidateOrdering` by design.

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

### ParticleSystem — [`Systems/ParticleSystem.h`](../../GanymedEngine/source/GanymedE/Scene/Systems/ParticleSystem.h)
CPU simulation of `ParticleEmitterComponent`. Per emitter, in this fixed order (the order is the
determinism contract): PlayOnStart on a fresh emitter (`Playing == false`, `IsFresh()` —
`Time == 0` and empty pool) sets `Playing`; a Playing rising edge **and** a fresh state seeds
`Rng` from `Seed` or the entity UUID (`SeedRng`). A Stop→Play resume is a rising edge that is
*not* fresh, so the stream continues. Inspector Restart seeds itself after `ResetRuntime` —
the next tick already sees `Playing == true` and would miss the rising-edge seed. Then: age +
**stable** compaction
(`std::remove_if`, not swap-erase — pool order is the replay instrument); integrate velocity /
position / rotation, with world-down gravity rotated into emitter space in local mode (a tilted
fountain's gravity must not tilt with it); spawn from `EmitAccumulator` at `RateOverTime`,
capped at `MaxParticles`, drawing lifetime/speed/cone/size/rotation from `Rng` in a documented
order; rebuild a conservative world AABB (frustum-cull input for the billboard / mesh submit).

**Ticks in edit mode, the same tick as play.** Against both neighbours, deliberately:
`AnimationSystem` samples-without-advancing — impossible here, a spawn/age sim has no closed
form. `AudioSystem` is silent in edit — defensible for sound, wrong for visual authoring.
`Playing = false` is how authors get quiet. Registration is between Audio and Render; Render's
`RO<ParticleEmitterComponent>` is what makes `ValidateOrdering` enforce that slot.
Rate spawn is gated by `Looping || Time <= Duration`; **`BurstPending` is not** — a one-shot
spark (`RateOverTime=0`, `Looping=false`) still emits on later Lua `EmitBurst` calls while
`Playing`. Lua has already run this update, so `PlayParticles` + `EmitBurst` in the same
script tick is consumed this frame. Remainder past `MaxParticles` stays queued.

### RenderSystem — [`Systems/RenderSystem.h`](../../GanymedEngine/source/GanymedE/Scene/Systems/RenderSystem.h)
Pure submission — everything that used to be inlined in `Scene::OnUpdate*`. Reads `RenderContext`,
begins Renderer3D with the main camera (or the editor fallback), submits lights, sky/environment,
meshes, **particles** (billboards queued into `ParticleRenderer`, mesh particles as ordinary
`SubmitMesh` opaques), collider gizmos (or Jolt debug draw when enabled during play), marker
gizmos, skeleton gizmos, ends the scene, then does the 2D pass (sprites) in its own render view.
The editor path additionally draws the grid, and looks through `RenderContext::PreviewCamera` when the viewport
dropdown has selected a scene camera (otherwise `EditorViewCamera`). Its eleven view declarations
are live documentation of exactly what rendering reads.
`SkyView` includes `EntityId` so the editor hide filter can skip a hidden sky light.

The editor outliner eye is honoured only on the editor path: `EditorViewFilter::HiddenEntities`
(a pointer into panel-owned UUID sets) is expanded to a subtree set at the start of
`OnUpdateEditor`, and every submit loop skips those entities. `OnUpdate` clears the set so Play
and the runtime draw everything. Hidden entities therefore vanish from the entity-ID buffer and
cannot be picked. This is an editor filter, not a runtime visibility component.

These policies live in this system:

- **Collider gizmos are opt-in on both paths.** With Jolt debug draw off, the authored-collider
  wireframes are drawn only when `PhysicsSettings::ShowColliderGizmos` is set. It defaults
  **false**, so a shipped game never draws them; the editor pushes the Visualizers checkbox every
  frame (Edit and Play) because `Scene::Copy` does not carry singletons. (Play used to fall through
  unconditionally, which meant any non-editor front-end drew collider wireframes over the game.
  Edit used to call `DrawColliderGizmos()` with no flag at all.)
- **Marker gizmos are the same opt-in.** `DrawMarkerGizmos` reads `PhysicsSettings::ShowMarkers`
  (engine default false; editor Icons toggle, default on). A wire sphere (16 segments) plus an
  optional world −Z forward (`DrawForward`, the light convention). `DrawWireSphere` / `DrawLine`
  accumulate and flush as one debug-line batch in `EndScene` — 100 markers are not 100 draws.
  The flag lives on `PhysicsSettings` next to `ShowColliderGizmos` rather than a one-bool
  singleton; the name is debt.
- **Skeleton gizmos are the same opt-in.** `DrawSkeletonGizmos` reads `ShowSkeletons` (engine
  default false; editor Visualizers, default on). Joint frames come from `TryGetJointFrame`, so
  the overlay cannot drift from a socket. Per posed entity: a line to each parent, a 3-line cross
  at the joint (sized from bone length, not a wire sphere), a short +Y stub on leaves, and an
  axis triad only on the highlighted joint. `ShowAllSkeletons` draws every rig; otherwise only
  the current selection and its hierarchy (select the capsule, see the body's bones).
  `SkeletonXRay` (default true) submits those lines with depth testing off. Joint-name labels
  are editor-side ImGui, and only for the highlighted joint plus its parent and children.
- **No camera is loud, not silent.** With no primary camera *and* no fallback, the frame is the
  scene target's clear colour and the system logs an error at most once every 5 s. Throttled rather
  than per-frame: a 60 Hz error would bury everything else in the log to say the same thing.

The mesh view carries `OptRO<AnimatorComponent>`, so one iteration covers both palettes: a mesh
that `HasSkeleton()` always goes to `Renderer3D::SubmitSkinnedMesh`. The animator's palette is used
when it is present and sized to the rig; otherwise the mesh's rest palette
(`SampleClipGlobals(nullptr)` then `Global * InverseBind`, cached on the asset). `SubmitMesh` is
the path for meshes with no skeleton. Dropping a character therefore draws at rest without adding
an animator first — and it draws at the right size, which the old static fallback did not for a
file whose `LocalTransform` is a unit conversion. Declaring that optional read is also what made
the `AnimationSystem`-before-`RenderSystem` ordering checkable at last — see
[ecs.md](ecs.md#systemmanager). `ParticleSystem` is the same shape: `RenderSystem` declares
`RO<ParticleEmitterComponent>` so the Audio-then-Particle-then-Render slot is enforced rather
than conventional.

## Singletons

Scene-wide state that is genuinely singular lives in `registry.ctx()`
([`SceneSingletons.h`](../../GanymedEngine/source/GanymedE/Scene/SceneSingletons.h)), accessed via
singleton views (systems) or `Scene::GetSingleton/FindSingleton/SetSingleton` (tooling):

- **`RenderContext`** — `MainCamera` + `CameraTransform` (resolved per update by CameraSystem),
  `EditorViewCamera` (the editor's camera: the view camera in edit mode, the fallback in play
  mode), and `PreviewCamera` (UUID{0} = editor camera; any other value is a scene
  `CameraComponent` the viewport dropdown is looking through, consumed only by
  `RenderSystem::OnUpdateEditor`). Change-tracked (`SingletonTraits<RenderContext>::TrackChanges`).
  *Known misnomer:* now that a non-editor host exists, `EditorViewCamera` is really "fallback view
  camera" and is simply null there. Flagged as debt rather than renamed — the rename ripples
  through docs and editor for zero behaviour change.
- **`PhysicsSettings`** — `DebugDraw` toggles, `ShowColliderGizmos`, `ShowMarkers`,
  `ShowSkeletons` / `ShowAllSkeletons` / `SkeletonXRay`, `FixedTimestep` (1/60),
  `MaxStepsPerFrame` (5). The Show* flags are editor visualization, not physics; they sit here
  because this is already the bag those per-frame editor pushes go through.
- **`EditorViewFilter`** — editor-only. Pointers to the outliner's hidden-UUID set and the
  current selection, asserted each frame by `EditorLayer`. `HiddenEntities` null means draw
  everything. `RenderSystem::OnUpdateEditor` expands each hidden UUID to its subtree via
  `CollectSubtree` and skips those submits. Play/runtime ignore hidden, so a hidden entity still
  simulates and draws in Play. `SelectedEntities` and `HighlightSkeletonEntity` / `HighlightJoint`
  drive the skeleton overlay on both Edit and Play (the editor pushes them onto the play copy).
  Highlight comes from editor joint picking / the Joints panel, falling back to a selected
  `BoneAttachmentComponent`'s resolved joint. Not serialized; `Scene::Copy` does not carry it.
- **`EditorBoundsOverlay`** — editor-only extra wire geometry, drawn after collider gizmos in
  `OnUpdateEditor`. The Map panel's parity audit fills `Boxes` with the focused finding's mesh AABB
  (cyan) and box collider (orange). Scatter fills `Spheres` with the brush (cyan paint, red erase).
  Not serialized, not copied, ignored by play/runtime.

**Singletons are not carried by `Scene::Copy`.** The copy constructs a fresh `Scene`, whose
constructor default-constructs its own `ctx()` entries, and then copies entities and components only.
Anything a host needs true on the play-mode scene must be (re)written after the copy — which is why
`EditorLayer` pushes `DebugDraw`, `ShowColliderGizmos`, `ShowMarkers` and the skeleton flags onto the
active scene every Edit and Play frame rather than once on play.

## Member reflection

Ganymed reflects component **types** well — `ComponentList` and `ComponentTraits<T>` drive
`Scene::Copy`, signal hookup and undo snapshots ([ecs.md](ecs.md#component-registration)). What it did
not reflect is component **members**: the serializer, the inspector and the Lua bindings each
hand-listed every field, so a new `float` had to be added in four places and silently did nothing if
one was missed. [`Reflection/`](../../GanymedEngine/source/GanymedE/Reflection/Reflection.h) is the
member half, built on **entt's own `entt::meta`** rather than a second reflection library — see
[REFLECTION_ROADMAP.md](../history/REFLECTION_ROADMAP.md) for why (short version: "add component by
type name", copy/paste-a-component and prefab diffing all have to get from a reflected type *back* to
entt storage, and only entt's own meta can).

Two files:

| File | Contents |
|---|---|
| [`Reflection.h`](../../GanymedEngine/source/GanymedE/Reflection/Reflection.h) | The `Trait` flag enum, the `Attr` payload struct, the query helpers, `GE_REFLECT_COMPONENT` / `GE_REFLECT_TYPE` |
| [`ComponentReflection.cpp`](../../GanymedEngine/source/GanymedE/Reflection/ComponentReflection.cpp) | Every registration block, `Reflection::Init()`, `Reflection::Validate()`, the size sentinels |

### The one discipline line

**No engine system may read a component through `entt::meta`.** `meta_data::get` returns `meta_any`
*by value*; entt's small-buffer optimization covers a `float` or a `bool`, but a `std::string`, a
`glm::mat4` or a `std::vector` allocates. For an inspector (a few dozen reads per frame on one
selected entity) that is irrelevant; for a save (thousands per file, not a frame-budget operation) it
is fine; for a system touching every entity every frame it is disqualifying. Systems use
`ComponentList`/`ForEachType` and direct member access, exactly as they do today. This is a rule about
*where* reflection is allowed, not a performance caveat to weigh case by case.

### The two-tier attribute vocabulary

Attributes split by cost, because entt stores them differently:

- **`Trait`** — a 16-bit flag word packed into the meta node itself. Free to read, no allocation, no
  lookup. entt reserves the low 16 bits of a node's traits word for its own flags (`is_class`,
  `is_enum`, …) and shifts user traits into the upper half, so a user enum gets exactly 16 bits and no
  more — a hard ceiling, and it also asserts that not all sixteen are set at once. Eleven are spent:
  `Hidden`, `ReadOnly`, `Color`, `Radians`, `NotSerialized`, `OmitIfDefault`, `Flatten`,
  `SerializeByName`, `Component`, `CustomDrawer`, `CustomWriter`, plus the composites
  `Runtime = Hidden | NotSerialized` and `Custom = CustomDrawer | CustomWriter`.
- **`Attr`** — one payload struct behind `.custom<>`: display label, section, note, min/max, drag
  speed, the key prefix a `Flatten`ed field gives its children, and the `AssetType` an `AssetHandle`
  field accepts. One struct rather than one per attribute
  kind because **entt holds a single `.custom<>` payload per meta object — a second call replaces the
  first, it does not append.** That single fact is also why registration is engine-side only: an
  editor-side second pass would silently overwrite everything the engine registered. Editor-only
  knowledge (drawer function pointers) belongs in the editor's own `meta_type`-keyed map.

Every flag exists because the current serializer or inspector measurably needs it, not because it
seemed generally useful:

- `Color` — 11 `ColorEdit3/4` call sites the type system cannot distinguish from a `DragFloat3`.
- `Radians` — `TransformComponent::Rotation`, the spot cone half-angles and the camera FOV are stored
  in radians and authored in degrees. When it is set, `Attr`'s min/max are in **display** units.
- `OmitIfDefault` — the key is written only when the field differs from the same field of a
  default-constructed instance. `ParticleEmitterComponent` puts it on all ~20 of its authored fields,
  and every asset handle carries it so an unset slot writes nothing. The comparison needs equality
  on the field type, which entt cannot synthesize — `meta_any::operator==` compares *addresses*
  unless a comparison function was registered — so each YAML codec carries an `Equal` alongside its
  read/write pair, and a type with no codec is never omitted (writing a redundant key costs a diff
  line; omitting a differing one is data loss).
- `Flatten` — the nested struct's fields are emitted as **siblings**, not as a sub-map. A collider's
  `PhysicsMaterial` writes `Friction` and `Restitution` beside `HalfExtents`; a particle's `RangeF`
  writes `LifetimeMin` and `LifetimeMax`, using `Attr::KeyPrefix` to prefix each child key. Without
  the flag a generic writer would nest them and invalidate every saved collider and emitter.
- `SerializeByName` — type-level, on `AudioGroup`, the one enum persisted by name rather than ordinal.
  The names come from the enumerators registered on the enum type, so the file and the inspector's
  combo read the same list.
- `CustomDrawer` / `CustomWriter` — "a bespoke implementation owns this field", asked **separately of
  each consumer**. These were one flag until the serializer conversion finished, and the conflation
  ran the wrong way: `AnimatorComponent::Clip` and the two particle curves need a bespoke *widget*
  (a combo over the mesh's clip names, a curve editor, a gradient editor) and nothing bespoke at all
  on disk — a string and two sequences — yet a fact about the inspector locked them out of the
  generic writer. `Custom` remains as the composite for the genuinely-both cases:
  `RelationshipComponent`'s two ends, `ScriptComponent::Fields`, and `StaticMeshComponent`'s
  per-slot override list.

There is deliberately no `AdvancedOnly` and no `EnumNames`: nothing in the panel has an advanced
section, and enum value names are registered on the **enum type**, so they live once beside the enum
instead of once per field that uses it (`meta_type::is_enum()` plus its `data()` range answers "what
are the options").

### Registered names are the on-disk contract

Every `YAML::Key` in `SceneSerializer.cpp` is currently character-identical to its C++ member
identifier, and the roadmap's R3 drives save/load from these registered names. A name here that does
not match today's key breaks every committed `.gscene` and `.gprefab`. Field names are therefore
**written out by hand**, never stringified from the token — a field rename would otherwise silently
drop one value from every scene. The human-facing label lives in `Attr::Display`, which is how
`SceneCamera` carries the key `PerspectiveFOV`, the accessor `GetPerspectiveVerticalFOV` and the label
"Vertical FOV" at once.

Type names *are* stringified by the macros. The asymmetry is deliberate: a component-type rename is
loud (the component vanishes from every entity in the editor immediately) where a field rename is
silent, and the type token already has a second binding in `ComponentList` that a rename must satisfy
anyway.

### What is registered

34 types, 129 members → wait I need the current text

35 types, 131 members (the boot log prints both — a count far below that is the cheapest signal that a
registration block was dropped by the linker):

- The **26 components** — all 24 `ComponentList` entries plus `IDComponent` and `TagComponent`, which
  `ComponentList` excludes as entity identity but which prefab diffing has to know exist in order to
  skip.
- **4 supporting types** — `PhysicsMaterial`; `SceneCamera`, whose seven private fields are registered
  through entt's setter/getter `.data` overload; and `FloatCurve` / `ColorGradient` with **zero
  members**. A reflected type with no members is the deliberate signal "opaque — a bespoke drawer and
  writer own this": both curve types keep a sorted-by-time invariant and never expose their key vector
  mutably, so a generic setter could not preserve the invariant even if one existed.
- **5 enums** with their value names.

glm's vector types are **not** registered. Reflecting `vec3::x/y/z` would invite a generic serializer
to emit a map where yaml-cpp's converter currently writes a flow sequence, silently changing the file
format; consumers identify them by `type_info` comparison instead, which needs no registration.

### Verification

`Reflection::Validate()` runs from `Init()` in Debug and logs every problem rather than stopping at the
first (a registration mistake is usually a repeated copy-paste). It checks that every `ComponentList`
entry is registered *and* went through `GE_REFLECT_COMPONENT`, that no field was left nameless, that
`SerializeByName` is only on an enum, that a `Flatten` field's type is itself reflected, and that
valued/flag attributes match the type they were put on — `Color` on a vec3/vec4, `Radians` on a
float/vec3, an asset slot on an `AssetHandle` or an `AssetRef<T>`. That last one is the load-bearing
check, and it is stronger on an `AssetRef<T>` than on a handle: a bare `AssetHandle` is a plain
`UUID` alias, the *same type* as `RelationshipComponent::Parent`, so it can only be checked for
being a handle at all, while an `AssetRef<T>` carries its asset type in the C++ type and the
declared slot has to **agree** with it. A copy-pasted `.Asset(AssetType::Texture)` on an
`AssetRef<Environment>` is caught; the same mistake on a bare handle still is not.

The "registered" test is `resolve<T>().name() != nullptr`, **not** `if (entt::resolve<T>())`. entt
synthesizes a node from a function-local static for any type it is asked about, so the truthiness test
passes for a type nobody registered and would report an entirely empty registration file as healthy.
`name` is only ever assigned by `.type(id, name)`, which makes it the honest signal.

What no test can check is whether a type's member list is **complete** — the true member set is
exactly the thing that is not reflected. The `static_assert(sizeof(T) == N)` sentinels at the bottom of
`ComponentReflection.cpp` are the only forcing function, and they have two honest limits. Padding: a
`bool` dropped into existing padding does not move `sizeof` (`AudioSourceComponent` has three spare
bytes right now). And they cover 17 of the 24 `ComponentList` entries — every one with no
standard-library container member. `sizeof(std::string)` is 40 with MSVC's STL and 32 with libstdc++,
and vector and unordered_map differ likewise, so a sentinel on `TagComponent`,
`RelationshipComponent`, `StaticMeshComponent`, `AnimatorComponent`, `BoneAttachmentComponent`,
`ScriptComponent` or
`ParticleEmitterComponent` would have to be a table of per-platform numbers — more cost than it
catches, on a codebase that builds for Windows, Linux and macOS. The rule is mechanical rather than a
judgement call per component: library container member ⇒ no sentinel.

### Current state

**Two consumers: the inspector and the serializer.** Sixteen of the editor's twenty component
sections are drawn from this registration rather than from a hand-written lambda
([editor.md](../editor/editor.md#the-generic-reflected-inspector)), and **every** component is
written and read generically by `SceneSerializer` (below). The Lua bindings still hand-list every
field and are deliberately out of scope.

The two consumers convert **independently**, which is worth seeing once: Static Mesh, Animator,
Bone Attachment and Script are serialized generically while their inspector sections stay
hand-written (Bone Attachment's Offset/Rotation rows are reflected; Target and Joint are not).
What blocks them from the panel is that their UI is driven by asset or Lua data — a mesh's
material slots, its clip names, a **different entity's** joint names, a Lua class's declared
fields — which has nothing to do with how the component is stored.
"Reflected" is per-consumer, not a property of the component.

That independence is what the `CustomDrawer` / `CustomWriter` split makes expressible per *field*
rather than only per component. `AnimatorComponent::Clip` is the smallest case: a combo over the
mesh's clip names in the panel, an ordinary omitted-when-empty string on disk.

Two facts that consumer established, both worth knowing before writing another one:

- **`meta_type::data()` iterates in registration order.** Verified rather than assumed — the
  inspector's field order is user-visible, and a `dense_map` that happened to iterate by hash would
  have scrambled every converted section. Registration order is therefore a real contract of
  `ComponentReflection.cpp`, not just tidiness.
- **`meta_data::get` may hand back a copy or a reference depending on entt's policy**, so every
  drawer reads into a concrete local, edits that, and writes back with `set`. Poking through the
  `meta_any` would work by accident and break on a policy change.

R2 also added `Attr::Reset` to the vocabulary: the value the X/Y/Z widget's coloured buttons restore.
It is not derivable and no other attribute can carry it, and without it the collider sections could
not go through the generic drawer without changing what their reset buttons do. One registered field
uses it today (`BoxColliderComponent::HalfExtents`), which is the same "forced by a specific measured
fact" bar every other attribute had to clear.

### Ranges

`RangeF` is a min/max pair authored as one thing, used by the particle emitter's five ranges
(lifetime, speed, start size, start rotation, rotation speed).

It exists for one reason, and it is not tidiness: **the clamp direction**. The panel pushes Max up
when Min passes it and pulls Min down when Max drops below, and a drawer seeing two unrelated floats
cannot know which half the author just moved. One drawer owning both halves does.

It is **layout-identical** to the two floats it replaced (`float Min, Max;` in that order), and two
things were deliberately kept byte-for-byte:

- **The YAML keys.** `SceneSerializer` still writes `LifetimeMin` / `LifetimeMax`, reading them from
  `Lifetime.Min` / `Lifetime.Max`. No scene or prefab on disk changed, and no migration was needed.
- **The Lua API.** `GetParticleLifetimeMin` still exists and still reads the same value; the
  bindings gained an overload taking a `RangeF` member pointer plus a `float RangeF::*` half. A C++
  refactor must not silently rewrite a scripting API that shipped.

### Prefab member links

Every entity `PrefabSerializer::Instantiate` creates carries a **`PrefabMemberComponent`** holding
its `CanonicalID` — the id it has *inside the prefab file*, which the writer assigns as 1..N in DFS
order. `PrefabInstanceComponent` still marks only the instance root, so "is this an instance root"
is the same question it always was.

This is the link per-property overrides key on, and nothing else could serve: an instance's entities
get fresh UUIDs every time, so without it there is no way to say which prefab object a given instance
entity corresponds to. Recording it is only possible inside `Instantiate`, which is the one place
that still holds the pairing.

Entities added by hand inside an instance carry no `PrefabMemberComponent` and take part in no diff,
which preserves the "structural freedom inside an instance is allowed and unmarked" rule.

It is written generically like everything else. It used to be hand-written because "a `UUID`
persists as a plain integer and the generic path has no codec for one" — there is a `UUID` codec now,
so the exception went with the reason for it.

## Serialization

### The reflected path

**Every component** is written and read by `WriteReflected` / `ReadReflected` in
[`SceneYaml.h`](../../GanymedEngine/source/GanymedE/Scene/SceneYaml.h) rather than by a hand-written
block. Three *fields* remain hand-written, and each is marked `Trait::Custom` so the generic path
skips exactly them while the rest of their component still goes through it:

| Field | Why |
|---|---|
| `RelationshipComponent::Parent` / `::Children` | The two ends of a link must agree; writing either half generically would corrupt the hierarchy. Reparenting goes through `Scene`'s API. |
| `StaticMeshComponent::MaterialOverrides` | A flow sequence whose **index is the meaning** — the slot count comes from the mesh asset, not from the vector. |
| `ScriptComponent::Fields` | A sequence of `{Name, Type, Value}` maps over a closed variant, because the declaring script may not be loadable when the scene is read. |

`TagComponent` is written generically but read by hand, because the tag is needed to *create* the
entity and so cannot go through a reader that needs an entity to read into. That is now the **only**
hand-written component read left — `PrefabSerializer::ReadRootTransform`, which peeked at the root
entity's three transform keys directly, goes through `ReadReflectedComponent` and gained its
tolerance for a missing key along the way.

Four things the generic writer had to learn to cover the rest:

- **`OmitIfDefault`**, against a default-constructed instance of the owning type. That instance is a
  parameter of `WriteReflected` rather than something it builds: getting a default-constructed `T`
  out of an `entt::meta_type` would need `.ctor<>()` registered on every component, where passing one
  down from `WriteReflectedComponent<T>` — which already knows `T` — needs nothing and cannot be
  forgotten for one type.
- **Codecs for `UUID`, `AssetRef<T>`, the curve types and the enums.** An `AssetRef<T>` writes the
  bare `uint64` handle a plain `AssetHandle` field writes, which is what kept every scene valid
  across the asset milestone. Its reader deliberately does **not** resolve: warming every reference
  at load would run a full IBL bake for an `AssetRef<Environment>`. The one field that wants warming,
  `StaticMeshComponent::Mesh`, asks for it at its own call site.
- **Prefixed `Flatten`**, so a `RangeF` keeps writing `LifetimeMin` / `LifetimeMax` while a
  `PhysicsMaterial` keeps writing `Friction` / `Restitution` bare. The prefix is an `Attr` value per
  field, not a rule derived from the field name — that would make an on-disk key a function of a C++
  member name, which decision 3 forbids.
- **Nested sub-maps** for a reflected struct with no codec, which is how `CameraComponent::Camera`
  keeps its `Camera:` block. `SceneCamera`'s seven fields are registered against accessors, so
  `RecalculateProjection` runs per field on load exactly as the hand-written setter calls made it,
  and the private projection matrix never reaches the file.

**The gate was byte-identical output, not a working round-trip.** Every committed `.ganymede` is a
file people diff; a serializer that reorders one key or reformats one float invalidates all of them
at once. Three properties make that achievable rather than hopeful:

1. `meta_type::data()` iterates in **registration order**, and `ComponentReflection.cpp` registers
   fields in the order the hand-written writer emitted them.
2. The registered field name **is** the YAML key — decision 3, which is why names are written out by
   hand rather than stringified from the C++ token.
3. Values go through the very same `operator<<` overloads, so a `float` written from a `meta_any`
   reaches yaml-cpp identically to one written from the member.

Verified by running all seven committed scenes and prefabs through both writers and diffing the
outputs: five are byte-identical, and the other two match block-for-block once the *pre-existing*
per-run UUID churn in `3DExample` and `Example` is accounted for — a control run of the unmodified
binary produces the same churn against itself. Note that the committed fixture files are themselves
stale: **both** writers reformat them, because the curve emitters changed style after they were last
saved. That is why the diff is taken between two runs rather than against what is in git.

The four components no fixture exercises (Animator, Point Light, Spot Light, and the sphere and
capsule colliders) are covered by a save → load → save fixed-point check over one entity carrying
every component at non-default values.

One deliberate difference from the code it replaced: **reading is more tolerant.** The hand-written
loader did `c.Field = node["Field"].as<T>()` unguarded, so a missing key threw. The generic reader
leaves the constructed value alone. That is required, not a nicety — a field omitted because it
equalled its default has to read back as that default.



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

  The walk itself is `Scene::CollectSubtree` — hierarchy traversal is a scene concern, and undo's
  subtree snapshots and `DuplicateEntity` need the same order. It carries a visited set, so a
  corrupted hierarchy (a cycle, a child listed under two parents) terminates. Any entity reachable from no root is appended in UUID order **with a
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
- `BoneAttachmentComponent::Target` is translated in the same pass: it is an entity UUID, looked
  up in the file-to-new map. Zero stays zero. A value that does not name an entity in the batch
  is left as-is, so a prefab instance can still name a scene entity that was not in the file.
- `StaticMeshComponent::MaterialOverrides` serializes as a flow sequence of handles, and **only
  when at least one slot is set**. Emitting an empty sequence for every mesh entity would rewrite
  every committed scene for no content change, which is what canonical saves exist to prevent.
  Trailing unset slots are kept rather than trimmed: the index *is* the slot.
- Asset references serialize as **handles** (`uint64_t`); `MeshPath`/`EnvironmentPath`/`ScriptPath`
  string fallbacks are still read for backward compatibility and imported into the asset index on load.
  Unlike meshes, a deserialized `ScriptComponent` handle is *not* warmed through `GetAsset<>` —
  scripts have no runtime object to cache, and `ScriptEngine` loads the chunk on instantiation.
  Its property overrides serialize as a `Fields` sequence of `{Name, Type, Value}`, sorted by name
  so a scene file does not churn when a hash map reorders. Each carries its own type because the
  declaring script may not be loadable when the scene is read back.
- `PrefabInstanceComponent` serializes its `Source` handle, omitted when unset. A scene whose
  prefab file has since been deleted still loads: the instances become plain entities carrying a
  handle that resolves to nothing, and the editor reports it when you try to Apply or Revert.
- `ScatterGroupComponent` serializes `Source` (omitted when unset) and `LastSeed`. It is a folder
  marker, not an instance array — the children are ordinary entities written as themselves.
- `MarkerComponent` serializes through the generic reflected writer (`Kind`, `Color`, `Size`,
  `DrawForward`). An unknown kind is still a string and round-trips; the Map-panel palette is not
  a whitelist.
- **`AudioGroup` serializes as a name, not an ordinal** (`Group: Music`). It is not persisted by
  ordinal the way `AssetType` was, so nothing forces stable numbering on it, and an unknown
  name warns and falls back rather than throwing. Both audio components read every field guarded
  (`if (node["Volume"])`) rather than bare `as<T>()` — hand-authored scenes are a normal way to
  make one, and an absent key in a bare read throws out through `Deserialize` and loses the whole
  file.
- **`ParticleEmitterComponent` serializes only when present**, and every authored field is omitted
  when equal to a default-constructed component (`IsDefault()` for the curves; invalid asset handles
  omitted). Runtime fields (`Playing`, `Time`, `EmitAccumulator`, `BurstPending`, `Rng`, `Pool`, `WorldBounds`) are
  never written. Decode is fully guarded. A default emitter therefore emits an empty map under the
  component key, not a wall of defaults — that is what keeps committed scenes without emitters
  byte-identical on load → save → load → save.
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
- **`FloatCurve` / `ColorGradient`** ([`Math/Curve.h`](../../GanymedEngine/source/GanymedE/Math/Curve.h))
  are linear keyframe types: never empty (default is the identity multiplier `{0, 1}` / white
  `{0, (1,1,1,1)}`), sampled by the same `upper_bound` → lerp shape as `AnimationClip::Channel`
  (`AnimationSystem::FindKeys`). Mutation goes through `AddKey` / `RemoveKey` / `SetKey` so the
  sorted-by-time invariant holds at the type boundary; `RemoveKey` refuses the last key.
  `IsDefault()` is the omit-guard for when a component starts serializing them. In YAML they are a
  Flow sequence of `[t, v]` / `[t, r, g, b, a]` keys. Decode is warn-and-default and **never
  returns false** — a scalar where a sequence was expected yields the identity curve rather than
  throwing out of `as<T>()`, which is a deliberate divergence from the glm conversions (those still
  return false and throw). Per-element `IsSequence` skips a malformed key and keeps the rest, the
  `MaterialOverrides` posture. Storage is array-of-structs, not Channel's parallel arrays: an
  authoring type has no glTF constraint.

## Prefabs (`.gprefab`)

[`PrefabSerializer`](../../GanymedEngine/source/GanymedE/Scene/PrefabSerializer.h) writes an
authored entity subtree. The file is the **scene format's entity list under a `Prefab:` root**:
same component blocks, written by `SceneSerializer::SerializeEntity`, read by
`DeserializeEntity`, ordered by the same hierarchy DFS. One schema, two containers — which is what
splitting the serializer into per-entity halves was for. `SceneYaml.h` holds the glm conversions
and the `FloatCurve`/`ColorGradient` key lists both share, so the encoding of a vec3 (or a
size curve) has one definition rather than two that can drift.

**UUIDs in the file are canonical: 1..N in DFS order, not the instance's own.** With preserved
UUIDs, applying identical content from two different instances produces two different files — a lie
in the diff. With canonical ones, identical content means identical bytes, and Apply is diff-stable
from any instance; the file also carries no trace of which scene birthed it. This is the fileID
stability Unity gets from its own local-ID scheme, reached the cheap way. Cost, accepted: an Apply
that only *reorders* children renumbers everything below the moved block — but sibling order is
content (it decides scene save order too), so that is a real change, not noise.

Only `IDComponent` and `RelationshipComponent` are renumbered. `AssetHandle` *is* `UUID`, so a
blanket remap would corrupt `StaticMesh.Mesh` and its `MaterialOverrides`, `SkyLight.Environment`,
`Script.Script` and `AudioSource.Clip` into handles no `.meta` sidecar knows.

**Prefab bookkeeping is stripped from the file.** `PrefabInstanceComponent` would make the result a
nested prefab, which v1 does not do. `PrefabMemberComponent` would bake *another* prefab's canonical
ids into this one — and that is not cosmetic: `Instantiate` gives every entity it creates a fresh
`PrefabMemberComponent` from the file's own 1..N numbering, and `Entity::AddComponent` asserts on an
entity that already has one. So "make a prefab out of part of a prefab instance" used to write a
file that asserted the next time anything instantiated it (and was undefined behaviour with asserts
compiled out). Instantiation also *assigns* rather than adds that component, so a file written
before this was fixed loads and has its stale id corrected instead of taking the editor down.

The renumbering happens in a **scratch `Scene`**, not in place. Renumbering the live scene and
putting it back would be faster; a throw or an early return in between would leave the real scene
carrying canonical UUIDs, which is unrecoverable corruption traded for one click's worth of
allocation.

**Instantiation reuses the scene loader wholesale.** Each block goes through `DeserializeEntity`
with a freshly minted UUID, then `ResolveHierarchy` translates the file's `Parent`/`Children` to
the created ones — the exact pass a scene load runs. Nothing prefab-specific was needed, which is
the payoff for making that pass uniform rather than a rare repair path. The canonical 1..N would
collide with the second instance in the same scene, so a fresh UUID per entity is mandatory, not an
optimization.

**Root transform ownership is symmetric.** The file stores the root transform captured at creation
(the spawn default) and *instantiate* applies it; thereafter the root's placement belongs to the
scene. So **Apply does not write the instance's root transform into the file**, and **Revert does
not overwrite the instance's root transform**. Everything below the root is wholly file-owned on
Revert and wholly instance-owned on Apply. This is the Unity norm: placement is per-instance.

**`PrefabInstanceComponent { AssetHandle Source }`** marks the instance *root only*. Descendants are
ordinary entities, which is what makes structural editing inside an instance free — add, remove and
re-parent children at will; Apply captures whatever the subtree is now, Revert discards it. There is
no divergence tracking, because per-field overrides are a serialization-diff engine and that is a
milestone rather than a feature. The component is inert at runtime: it exists so the editor can find
the source file again. An instance marker is stripped when a subtree is *saved* as a prefab —
nesting is not supported in v1, so a prefab file describes plain entities.

`AssetType::Prefab` is **appended as ordinal 8**, after `Audio`. Prefabs are **path-resolved with no
`GetAsset<Prefab>`** — the Script/Audio precedent: instantiation is a rare editor action reading a
small YAML, and a cached parsed form would add a staleness surface (Apply rewrites the file; the
next instantiate must see it) for no measurable win.

## Play mode

The editor's play button (see [editor.md](../editor/editor.md)) runs:

```
OnScenePlay: m_ActiveScene = Scene::Copy(m_EditorScene);  ActiveScene->OnRuntimeStart();
OnSceneStop: ActiveScene->OnRuntimeStop();  m_ActiveScene = m_EditorScene;
```

The runtime scene is a disposable deep copy keyed by UUID — physics can knock everything over and
Stop simply discards the copy. This is why stable UUIDs and the generic `ComponentList` copy exist.

The generic copy is a shallow value copy of every component, so anything that is runtime-only needs
an explicit fixup sweep after it. There are four: `NativeScriptComponent::Instance` is nulled so
instances are recreated on play; `AnimatorComponent::Palette` is cleared because carrying a
per-joint matrix array per entity into the new scene buys one frame of stale data;
`BoneAttachmentComponent::Resolved` is reset to −1 so a stale joint index cannot attach to
whatever now occupies that slot; and
`ParticleEmitterComponent::ResetRuntime()` clears pool, accumulator, timer, Playing, burst queue, bounds, and
RNG together — a copied-then-reset pool with a *not*-reset RNG would double-play the editor's
stream. Play-mode emitters therefore warm up from empty. Adding a component with runtime-only
state means adding another sweep — nothing enforces this.

The audio components are the worked example of *not* needing one. Putting the live `VoiceId` on
`AudioSourceComponent` would have made a fourth sweep mandatory and would have let a copied scene
double-drive one sound; keeping the voice in `AudioSystem`'s map means the copy is correct with no
audio code involved at all. Where a component's runtime state goes is a `Scene::Copy` decision as
much as an ownership one. Particles took the Palette side of that fork because the pool is pure
data, not a foreign resource.

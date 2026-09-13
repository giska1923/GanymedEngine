# Runtime Prefab Spawning — Scope Sketch

The engine has prefabs, a scripting layer and a physics system, and **a script cannot instantiate a
prefab at all**. There is no binding for it, and every `PrefabSerializer::Instantiate` call site is
an editor action. For an engine at this stage that is the conspicuous gap: "spawn a thing at
runtime" is the first capability any gameplay prototype reaches for — projectiles, pickups, enemy
waves, impact effects.

This document is a plan, not a description. Nothing in it is built.

---

## What already exists, verified rather than assumed

Read this section first: three of these findings change the shape of the work, and one of them is a
hole nobody has fallen into yet only because nothing spawns.

| | |
|---|---|
| `PrefabSerializer::Instantiate(path, scene, handle, options)` | Works, and `InstantiateOptions::RootUUID` already lets a caller **pre-mint the root's UUID**. That is the hook the whole API below hangs off |
| `ECS::CommandQueue` | Deferred structural changes already exist, with `PendingEntity`, flushed once per frame from `Scene::FrameBegin` — *before* the systems run |
| Lua `Entity` usertype | Transform get/set, animation, physics impulses, audio. No creation, no destruction |
| Lua `Scene` table | **One function**, `FindEntityByName`. No `FindEntityByUUID`, no spawn, no despawn |

And the three that matter most:

**1. Spawning cannot use the immediate `Entity` API.** `Entity::AddComponent` and `RemoveComponent`
assert `!m_Scene->IsUpdating()`, and scripts run inside the update. `Instantiate` uses that immediate
API throughout, so calling it from a script is an assert in Debug and undefined behaviour in
Release. Everything must go through the command queue.

**2. A spawned entity is therefore visible on the *next* frame.** `FlushCommands` runs at the top of
`FrameBegin`, before any system. So a change queued by a script during frame *N* becomes real at the
start of frame *N+1* and is visible to every system for the whole of that frame. This is the
existing, deliberate contract of the command queue — "a structural change made by a system becomes
visible on the next frame" — and it is not negotiable without rewriting that queue. It has to become
part of the spawn API's stated semantics rather than a surprise.

The useful corollary for Decision 3: **the flush point is at the start of a frame, not the end**, so
anything reconciling against newly-created entities can run immediately after it and still be ahead
of the systems that consume the result in the same frame.

**3. `PhysicsScene::CreateBodies` is called from exactly one place: `Start()`.** There is no
per-frame reconciliation of new entities. **A prefab spawned mid-run with a `RigidBodyComponent`
would render and never simulate** — no Jolt body is created for it, ever. This is the largest single
piece of work in the milestone and it is invisible today because nothing creates entities at
runtime.

---

## Decision 1 — spawn returns a UUID, not an entity

The entity does not exist yet, so the API cannot hand one back. Three options were considered:

- **Return nothing.** Fire-and-forget. Simple, and useless the moment a script wants to set the
  projectile's velocity it just spawned.
- **Return a `PendingEntity`.** The queue's own handle. It is meaningful only within the frame it
  was made in, and handing a Lua script a handle whose validity expires at a frame boundary is a
  lifetime problem the engine would then have to police — the same argument `ScriptBindings` already
  makes for not exposing audio `VoiceId`.
- **Pre-mint the root UUID and return that.** `InstantiateOptions::RootUUID` exists for exactly this
  reason. The script gets a `uint64` immediately, and resolves it with `Scene.FindEntityByUUID` on a
  later frame. A UUID that does not resolve yet is indistinguishable from one whose entity has been
  destroyed, which is the correct thing for a script to have to handle anyway.

**Take the third.** It needs `Scene.FindEntityByUUID` added to the Lua table, which is a one-line
binding over an existing `Scene` method and is independently useful.

## Decision 2 — the Prefab asset manager, finally justified

`Instantiate` takes a path and **re-reads the `.gprefab` on every call**. That costs nothing today
because all four call sites are editor gestures. At spawn rates it is a file read per spawn, and a
bullet-hell firing 100 projectiles a second would do 100 parses a second of the same file.

That is the measurement-backed trigger the asset layer has been waiting for.
[assets.md](../engine/assets.md#path-resolved-types) records that Script, Audio and Prefab are
path-resolved by design, and the stated reason is an **external owner** — Lua owns its chunks,
miniaudio owns decoded audio, so a manager cache would be a second owner of the same resource. That
reason does not apply to Prefab: nothing else owns a parsed prefab, and the editor already keeps one
by hand in `EditorPrefabOverrides.cpp`'s template cache.

So this milestone adds a `Prefab` asset type whose parsed form is the YAML document (or a detached
`Scene`, as the editor's template cache already uses), with the usual Parse/Apply split — **parsing a
prefab moves to a worker**, which is the other half of the win. The editor's template cache then
becomes that manager's cache rather than a second one.

**Open**: whether the cached form is the parsed YAML or a detached `Scene`. The template cache uses
a `Scene` because it wants entities to diff against; a spawner wants whatever makes `Instantiate`
cheapest. Settle it in Phase 2 with both call sites in view.

## Decision 3 — physics bodies for entities that appear mid-run

The hole from finding 3. Two shapes:

- **Reconcile once per frame.** Immediately after `FlushCommands` in `FrameBegin` — which is before
  `PhysicsSystem` runs, so a body spawned this frame simulates from this frame rather than the next.
  Walk entities with a `RigidBodyComponent` and no body and create them. Simple, and it costs a scan
  proportional to the scene every frame — or a dirty set to avoid that.
- **React to component construction.** entt's `on_construct<RigidBodyComponent>` signal. Precise and
  free when nothing spawns, but fires during the queue flush, which is exactly the reentrancy the
  command queue exists to avoid.

**Lean to the first, driven by a dirty set the queue fills** — the queue already knows which
entities it created, so it can hand the physics system a list rather than have it search. That keeps
the per-frame cost at zero when nothing spawned, without a signal firing mid-flush.

Note this is not only a spawning problem: **it is a latent bug today** for any path that adds a
`RigidBodyComponent` during play. Worth fixing on its own merits, and worth recording in
[physics.md](../engine/physics.md) whichever way the milestone goes.

## Decision 4 — despawn, and the budget

Spawning without despawning is half an API, and an unbounded spawn is a way to run a machine out of
memory from a script typo.

- `CommandQueue::DestroyEntity` already exists; the Lua side is a binding and the same next-frame
  semantics.
- **A per-frame spawn cap**, refused loudly rather than silently, in the spirit of the asset layer's
  `kMaxParsesInFlight`. A script looping `Scene.Spawn` without a guard should hit a warning naming
  the script, not a swap-thrashing machine.
- **Explicitly out of scope: pooling.** It is the obvious next optimisation and it is a different
  decision, wanting a measurement of spawn cost first.

---

## Sketched phases

Each phase should land building, documented and verified on its own, as the threading and asset
milestones did.

**P1 — `Scene.FindEntityByUUID` in Lua.** One binding. Independently useful, and Decision 1 depends
on it. The smallest possible first step, deliberately.

**P2 — `Prefab` asset type and manager.** Parse on a worker, Apply on the main thread, and the
editor's template cache re-pointed at it. No spawning yet: the gate is that the editor behaves
exactly as before, including the prefab-override diff and hot reload, with the file parsed once
instead of per call.

**P3 — `CommandQueue::InstantiatePrefab` + `Scene.Spawn` in Lua.** The spawn command runs the
existing `Instantiate` at flush time, where `IsUpdating` is false and it is legal unchanged. Returns
the pre-minted UUID. Gate: a script spawns a prefab, finds it by UUID next frame, and the entity is
identical to a drag-drop instantiation of the same prefab.

**P4 — physics bodies for spawned entities.** Decision 3. Gate: a spawned prefab with a rigid body
falls under gravity and collides, and a scene with no spawning pays nothing per frame for the
mechanism.

**P5 — despawn and the spawn cap.** Decision 4. Gate: a script that spawns in an unguarded loop is
refused with a named warning rather than exhausting memory.

---

## Risks

- **The next-frame delay is a gameplay-visible semantic**, not an implementation detail. A script
  that spawns a projectile and expects to set its velocity in the same call cannot. If that proves
  intolerable in practice the answer is a second flush point, not a special case for spawning — and
  that is a change to the ECS contract that should be made deliberately and separately.
- **Prefab-within-prefab.** `Instantiate` handles nesting today via the editor path; whether a
  spawn-time nested instantiate has the same cost profile is unmeasured.
- **P2 touches the editor's override diff**, which is recently-finished work with per-field revert
  and apply hanging off it. The gate for P2 is explicitly "the editor is unchanged", and it is worth
  keeping that strict.

## Explicitly out of scope

- **Pooling and spawn-cost optimisation.** Needs a measurement first.
- **Networked or deterministic spawning.** No multiplayer, no replay system.
- **Spawning from C++ native scripts.** `NativeScriptSystem` exists, but Lua is where the gap is
  felt; the command is shared either way, so this is a binding away once P3 lands.

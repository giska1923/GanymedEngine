# Runtime Prefab Spawning — Scope Sketch

The engine has prefabs, a scripting layer and a physics system, and **a script cannot instantiate a
prefab at all**. There is no binding for it, and every `PrefabSerializer::Instantiate` call site is
an editor action. For an engine at this stage that is the conspicuous gap: "spawn a thing at
runtime" is the first capability any gameplay prototype reaches for — projectiles, pickups, enemy
waves, impact effects.

**Complete.** P1–P5 all landed; this is the record of how, and of the three things the plan got
wrong. It was written as a plan before any of it was built, and the phase notes were added as each
one landed — so where the two disagree, the phase note is what happened.

The three corrections worth reading before trusting the decisions above:

- **Decision 1 was invalid as written.** "The script gets a `uint64`" — sol2 throws for any
  `uint64_t` above `INT64_MAX` under `SOL_ALL_SAFETIES_ON`, which is about half of all UUIDs. P1
  found it, and found that `Entity:GetUUID()` already had the bug.
- **The cached form is the parsed document, not a detached `Scene`** (P2), and the editor's template
  cache did *not* become the manager's cache — they hold different things.
- **P4 used a scan, not the dirty set Decision 3 leaned toward**, and "pays nothing per frame" is
  not literally true: ~150 ns per rigid body.

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
  reason. The script gets an opaque 64-bit id, and resolves it with `Scene.FindEntityByUUID` on a
  later frame. A UUID that does not resolve yet is indistinguishable from one whose entity has been
  destroyed, which is the correct thing for a script to have to handle anyway.

**Take the third** — with the caveat P1 then turned up: the id must cross into Lua as `int64`, not
`uint64`, or sol2 throws for half of all ids. See P1 below.

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

**Settled in P2: the parsed YAML.** The template cache wants entities to diff against, but it is the
only consumer that does; the document is what it and a spawner both instantiate *from*, and it is
the read and the parse — the expensive half — that caching removes.

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

**P1 — `Scene.FindEntityByUUID` in Lua — done.** It was not one binding. Pushing a `uint64_t` above
`INT64_MAX` makes sol2 throw under `SOL_ALL_SAFETIES_ON`, so Decision 1 as written — "the script
gets a `uint64`" — was **invalid**, and `Entity:GetUUID()` had the same bug already: it threw for
about one entity in two, silently, because nothing shipped called it. Both sides now reinterpret
through `int64_t`, verified bit-exact at 1, 2^53+1, 2^63+12345 and 2^64-2, plus the pure-Lua
`GetUUID() -> FindEntityByUUID()` round trip and an unknown id reading as `nil`. See
[scripting.md](../engine/scripting.md).

This is why the phase existed. Had P3 been written first, spawning would have handed back a UUID
that killed the script host for half of all spawns.

**P2 — `Prefab` asset type and manager — done.** Parse on a worker, Apply a move, and all three
editor call sites re-pointed at `PrefabSerializer::InstantiateFromAsset`. Gate met and measured:
**five instantiations produce one document load**, the instantiated entity is unchanged
(components, values, `PrefabInstanceComponent` / `PrefabMemberComponent`), the override diff reads
clean against an untouched instance, per-property apply still writes exactly one field, and a
`.gprefab` edited on disk still reaches the diff. See
[assets.md](../engine/assets.md#path-resolved-types).

Two things settled differently from the sketch above:

- **The cached form is the parsed document, not a detached `Scene`.** The document is what both
  consumers instantiate *from* and is the expensive half; a `Scene` would serve only the editor.
- **The editor's template cache did not become the manager's cache.** They hold different things —
  instantiated entities to diff against, versus a document — so the template cache remains, now
  built from the manager rather than from its own file read. The plan said otherwise; the plan was
  wrong about that.

One consequence found while building it: per-property apply writes the `.gprefab` but deliberately
does *not* invalidate the template it just mutated, so the manager's document would have stayed
stale until the watcher noticed (~0.45 s). Anything rebuilding a template in that window — a scene
change, entering play — would have shown the applied field as overridden again. The apply now evicts
the asset, which closes the window without disturbing the template.

**P3 — `CommandQueue::InstantiatePrefab` + `Scene.Spawn` in Lua — done.** The spawn command runs
`InstantiateFromAsset` at flush time, where `IsUpdating` is false and it is legal unchanged, and
returns the pre-minted UUID. Gate met, driven from Lua exactly as a gameplay script would:

| | |
|---|---|
| `Scene.Spawn(path, Vec3(3,4,5))` returns an id | PASS |
| the entity does **not** exist in the spawning frame | PASS |
| next frame it resolves, named `Sparks`, at (3,4,5) | PASS |
| identical to a drag-drop instantiation, component for component | PASS |
| the root's own UUID is the id that was returned | PASS |
| an unindexed path is refused with a named warning | PASS |
| a non-prefab asset is refused with a named warning | PASS |
| no position at all uses the prefab's stored transform | PASS |

The signature ended up `Spawn(path, position?, rotation?)`. A path rather than a handle because that
is the currency scripts already use for audio; rotation because a projectile needs a direction and
adding it later would have meant a second overload.

**P4 — physics bodies for spawned entities — done.** Gate met: a prefab spawned at y=10 fell to
y=-8.98 under gravity (before this it would have sat at 10 forever), exists on the next frame, and
its body is destroyed when it despawns. See
[physics.md](../engine/physics.md#reconciling-bodies-with-the-registry).

Settled against Decision 3's lean: **a scan, not a dirty set.** `CreateBodies` became idempotent -
it skips entities already in the body map - so "build the initial set" and "pick up whatever
appeared" are one function rather than two to keep in step. Measured cost when nothing has changed
is ~150 ns per rigid body per frame (30 us for 200 bodies, x64 Release), so "pays nothing" is not
literally true and the docs say so; a scene with thousands of bodies would want the dirty set after
all.

`RemoveDeadBodies` was not in the plan and is half the work: nothing could destroy an entity mid-run
before, so a despawned projectile would have kept colliding invisibly with the body count growing
for the session.

**P5 — despawn and the spawn cap — done.** `Entity:Destroy()` in Lua, on
`CommandQueue::DestroyEntityTree`, plus a 64-per-frame cap. Gate met:

| | |
|---|---|
| a 4-entity prefab spawns: 2 → 6 entities | PASS |
| `e:Destroy()` from Lua returns it to 2 — the **whole subtree** | PASS |
| 200 requests in one frame → 64 accepted, 136 refused | PASS |
| exactly one warning for the frame, naming the count | PASS |

The subtree half was not in the plan and is the more important of the two. `Scene::DestroyEntity`
*unparents* children rather than destroying them — right for deleting one hand-authored entity,
wrong for a projectile going away, whose children would have accumulated for the session. P4's test
missed it because `SparkBurst` is a single entity; this phase's test authors a prefab with children
specifically to catch it.

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

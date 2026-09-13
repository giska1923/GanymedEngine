# Physics

3D rigid-body physics via [Jolt Physics](https://github.com/jrouwe/JoltPhysics), integrated so that
**Jolt never leaks into engine headers**: components are plain data
([scene.md — physics components](scene.md#physics-pure-data--jolt-never-appears-here)), and
[`PhysicsScene`](../../GanymedEngine/source/GanymedE/Physics/PhysicsScene.h) hides every Jolt type
behind a pimpl (`PhysicsScene.cpp` is the only file that includes Jolt).

## Lifecycle

Physics exists only while the scene is playing:

```
Play  → PhysicsSystem::OnRuntimeStart → new PhysicsScene → Start(scene)  (Jolt world + bodies)
Stop  → PhysicsSystem::OnRuntimeStop  → Stop() → destroyed
```

`PhysicsScene::Start` walks every entity with `RigidBodyComponent` + a collider and creates a Jolt
body from its **world-space** transform (world scale is baked into the shape dimensions — Jolt
shapes don't scale). Collider `Offset` becomes a rotated-translated compound shape. Body settings
map 1:1 from the components: motion type, friction/restitution, damping, gravity factor
(`UseGravity` → 0/1), mass override for dynamic bodies. Two object layers exist — `NON_MOVING`
(static) and `MOVING` — with the usual "static doesn't collide with static" filter.

Identity: `body.mUserData = UUID`, plus `UUID → BodyID` and `BodyID → UUID` maps. Everything
crossing the physics/ECS boundary is keyed by UUID, so it survives the play-mode scene copy.

### Locked rotation

`RigidBodyComponent::LockRotation` forbids rotation entirely while leaving translation and
collision untouched, via Jolt's `mAllowedDOFs`:

```cpp
settings.mAllowedDOFs = EAllowedDOFs::TranslationX | TranslationY | TranslationZ;
```

It exists because **a walking character cannot be a plain dynamic capsule**. Give one a horizontal
velocity and the first thing it brushes applies a torque it has no reason to resist; it topples and
stays down. Measured on a capsule (radius 0.35, half-height 0.6, mass 70) pushed at 6 m/s across a
0.15 m kerb, after three seconds:

| | Rotation (rad) | Centre height | Distance travelled |
|---|---|---|---|
| `LockRotation: false` | `(0, 0, -1.141)` — **65° over** | 0.60 (fallen) | 3.29 |
| `LockRotation: true` | `(0, 0, 0)` | 0.95 (upright) | 3.28 |

Identical travel, so the constraint costs nothing in translation.

The bits are never cleared to `EAllowedDOFs::None`, which Jolt documents as invalid and which
crashes — a body that may not move at all is a **Static** body, a different authoring choice.
Only `Dynamic` consults the flag: `Static` never integrates, and `Kinematic` takes its orientation
from the transform every step, so restricting its DOFs would change nothing while appearing to.

Two limits worth knowing before this is mistaken for a character controller:

- **It is read at body creation**, so toggling it during play does nothing until the body is
  rebuilt. Jolt exposes a runtime setter; nothing pushes it per-frame yet because nothing has
  needed to.
- **It is not step-up, slope limits, or wall-sticking.** An upright capsule still catches on
  geometry a walking character should climb. That is `CharacterVirtual`'s job, and this flag is
  the cheap thing that unblocks a moving character without it — see
  [ToDo/PROVING_GROUND.md](../ToDo/PROVING_GROUND.md).

### Reconciling bodies with the registry

Bodies used to be created **once at Start**, so an entity that gained a `RigidBodyComponent` during
play never simulated. That was invisible for as long as nothing could create an entity mid-run;
runtime prefab spawning made it reachable, and it was in fact a latent bug for *any* path that adds
a rigid body during play, not only for spawning.

`PhysicsScene::SyncBodies` now runs **once per frame from the top of `PhysicsSystem::OnUpdate`** —
after the command queue flushed in `FrameBegin`, and before the step, so a prefab spawned last frame
simulates from this one rather than the next. It is two passes:

- **`CreateBodies` became idempotent.** It skips entities already in the `UUID → BodyID` map, which
  turns "build the initial set" and "pick up whatever appeared" into one function with no second
  code path to keep in step. `Start` still calls it; so does every frame.
- **`RemoveDeadBodies`** destroys any body whose entity is gone or has lost its component, and drops
  the interpolation poses keyed on the same UUID. Nothing could destroy an entity mid-run before, so
  this had no counterpart — without it a despawned projectile keeps colliding invisibly and the body
  count grows for the session.

**Cost when nothing has changed: ~150 ns per rigid body per frame** (measured, x64 Release: 30 us
for 200 bodies, averaged over 120 frames). A 20-body scene pays ~3 us. It is linear in body count,
so a scene with thousands would want the dirty set the milestone plan originally sketched — the
queue knows what it created, so it could hand over a list instead of being searched. The scan was
chosen instead for the reason the asset layer sums `PendingCount` rather than keeping a counter: a
second structure to keep in step can drift, and drift here looks like an entity that silently never
simulates.

One consequence worth knowing: `CreateBodies` warns about a `RigidBodyComponent` with no collider,
and it now runs every frame, so that warning is **once per entity** rather than once per run. The
same loud-but-not-scrolling posture the asset layer takes for unknown handles.

## The step

[`PhysicsSystem`](../../GanymedEngine/source/GanymedE/Scene/Systems/PhysicsSystem.cpp) drives a
fixed-timestep accumulator using the `PhysicsSettings` singleton (default 1/60s, max 5 steps per
frame as a spiral-of-death guard — hitting the cap drops the surplus time):

```
accumulator += dt
while accumulator >= fixedTimestep (max 5×):
    PhysicsScene::Step(fixedTimestep)
    dispatch collision events
alpha = accumulator / fixedTimestep
PhysicsScene::SyncTransforms(scene, alpha)
```

Inside `Step`:
1. **Kinematic bodies** are pushed *into* Jolt from their current world transform (gameplay code
   moves the component; physics follows).
2. Previous poses ← current poses (for interpolation), then `PhysicsSystem::Update` (1 collision
   step; Jolt's own temp allocator, and the shared job system below).
3. Current poses are captured, and contact add/remove events queued by the contact listener
   (thread-safe; Jolt callbacks fire from job threads) are drained and translated to
   `PhysicsCollisionEvent{ EntityA, EntityB, Entered }` via the body↔UUID maps.

## Transform writeback & interpolation

`SyncTransforms(alpha)` blends previous→current poses (`mix`/`slerp`) so rendering at high frame
rates doesn't stutter at the 60 Hz sim rate, converts back to **local** space through the parent's
world transform, and writes `TransformComponent`. Two details matter:

- Writes go through `Scene::MarkChanged<TransformComponent>` **only when the pose actually moved**
  (epsilon compare) — sleeping/resting bodies don't dirty the world-transform cache, which is the
  entire point of the cache.
- Only **dynamic** bodies write back; static ones never move and kinematic ones are authored by
  gameplay.

Order matters and is validated: `PhysicsSystem` declares `AccessView<RW<TransformComponent>>` so
`SystemManager::ValidateOrdering` knows `TransformSystem` (which consumes transform changes) must
run *after* physics.

## Collision events → scripts

`PhysicsSystem::DispatchCollisionEvents` resolves each event's UUIDs to entities and calls
`OnCollisionEnter/OnCollisionExit(other)` on both sides' scripts. Events accumulate per fixed step
and are cleared after dispatch.

Both script kinds are notified, through separate views: `AccessView<RW<NativeScriptComponent>>` for
`ScriptableEntity` instances, and `AccessView<RO<ScriptComponent>>` for Lua ones (read-only — the
instance lives in `ScriptEngine`, so only "does this entity have a script" is needed here). An
entity may carry either, both, or neither, and dispatch stays inside the fixed-step loop so the two
see identical timing. See [scripting.md](scripting.md).

## Runtime body control (for scripts)

`SetLinearVelocity` / `GetLinearVelocity` / `AddImpulse` / `AddForce`, keyed by entity UUID against
the `EntityToBody` map. This is the only correct way for gameplay code to move a dynamic body:
writing its `TransformComponent` instead is overwritten by `SyncTransforms` on the very next step.

Each wakes the body before acting — Jolt sleeps idle bodies and silently discards a velocity set on
a sleeping one. All of them no-op when the entity has no body or play is not running, rather than
asserting: a script poking at the wrong entity should not take the editor down.

## The job system

**Jolt's jobs run on `Core/JobSystem`, not on a thread pool of Jolt's own.** `JoltJobSystem` in
`PhysicsScene.cpp` implements `JPH::JobSystemWithBarrier`, which supplies the barrier half and
leaves four functions to the host: `GetMaxConcurrency`, `CreateJob`, `FreeJob` and `QueueJob(s)`.

Why it was worth doing ([THREADING_ROADMAP.md](../history/THREADING_ROADMAP.md) decision 4): Jolt's
stock `JobSystemThreadPool` creates `hardware_concurrency() - 1` threads, and
[`JobSystem`](core.md#job-system) had already created that many, so the process ran two full-width
pools for one machine. Measured on a 16-thread box, the editor went from **75 threads to 60** —
exactly the 15 Jolt was creating.

What did **not** change: Jolt still owns its job graph, its dependencies and its barriers. Only the
threads underneath are shared. `GetMaxConcurrency()` reports `JobSystem::ThreadCount()`, which is
the same number the old pool reported (`workers + 1`, because the thread waiting on a barrier runs
jobs too) — so Jolt splits its stages into the same number of chunks it always did.

Three details carry the design:

- **A job is queued by handing an enkiTS task a reference to it.** Jolt guarantees the job is alive
  only for the duration of `QueueJob`, so the adapter calls `AddRef()` and the task calls
  `Release()` once the job has run.
- **Task objects are pooled with a two-condition claim.** A slot is reusable only when it is both
  unclaimed *and* `GetIsComplete()` — enkiTS touches a task once more after `ExecuteRange` returns
  in order to retire it, and documents that the task must not be accessed after its running count
  reaches zero. Claiming on either condition alone is a race: the flag alone hands out a slot enkiTS
  is still finishing with, and completion alone lets two threads claim the same idle slot.
- **Falling back to the calling thread is always legal.** If every slot is busy, or the scheduler is
  unavailable (a tool with no `Application`, or physics stepped from a thread enkiTS does not know),
  the job runs inline. That is not a degraded special case — it is the same assumption Jolt's own
  pool makes when it has no worker threads, and it is self-limiting, since the thread that would
  have queued work performs it instead.

Verified by running the same scene through both implementations at a fixed timestep: body positions
are identical to six decimal places after 300 steps, and 8,688 of 8,688 jobs were dispatched to
enkiTS workers with none falling back inline — the check that separates "correctly parallel" from
"accidentally serial", since both produce the same answer.

## Debug draw

When built with `JPH_DEBUG_RENDERER` (Debug/Release, not Dist), `PhysicsScene::DebugDraw` pipes
Jolt's own body/constraint visualization into `Renderer3D::DrawLine`. Toggles live in the
`PhysicsSettings` singleton's `PhysicsDebugDrawSettings` (wireframe, bounding boxes, velocities,
center of mass, constraints), editable in the editor Stats panel. During play with the toggle on,
`RenderSystem` draws Jolt's view of the world *instead of* the authored collider gizmos — a
divergence between the two is itself diagnostic (it means components and bodies disagree).

## Extending

- **New collider type**: add the component (+ `ComponentList`, serializer, editor UI), extend the
  shape-creation switch in `PhysicsScene::CreateBodies`, add a gizmo in
  `RenderSystem::DrawColliderGizmos`.
- **Mesh colliders / physics materials as assets / raycasts**: not implemented yet; raycast support
  would slot naturally next to `DebugDraw` on `PhysicsScene` (Jolt's `NarrowPhaseQuery`), and is
  also the fallback picking path the bgfx migration doc mentions.

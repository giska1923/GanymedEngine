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

Bodies are created **once at Start** — entities added mid-play do not get bodies (an
`EntityChangedView`-based incremental path is the documented future fix in the ECS guide).

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
   step; Jolt's own temp allocator + job system).
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
`OnCollisionEnter/OnCollisionExit(other)` on both sides' `ScriptableEntity` instances (looked up
through an `AccessView<RW<NativeScriptComponent>>`). Events accumulate per fixed step and are
cleared after dispatch.

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

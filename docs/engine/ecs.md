# ECS: Views, Access Wrappers & Systems

The engine's ECS is entt underneath (`Scene` owns an `entt::registry`), with a declarative layer on
top in `GanymedEngine/source/GanymedE/ECS/`. That layer exists to answer three questions entt does
not:

1. **What does this code read and write?** — declared per view, checkable at compile time and
   at runtime (scheduling metadata).
2. **What changed since I last looked?** — per-consumer change tracking with `Modify()`-gated
   writes, plus reactive views for component creation/removal.
3. **When do structural changes happen?** — never mid-iteration; they are queued and applied at a
   fixed point with a fixed ordering.

The design was ported from a production engine; the full rationale and the porting plan live in
[`ECS_VIEWS_IMPLEMENTATION_GUIDE.md`](../toDo&done/ECS_VIEWS_IMPLEMENTATION_GUIDE.md). This page
documents what is actually in the tree.

## File map

| File | Contents |
|---|---|
| [`TypeList.h`](../../GanymedEngine/source/GanymedE/ECS/TypeList.h) | Compile-time list toolbox: Merge, Contains, IndexOf, Element, Map, Filter, `ForEachType` |
| [`ComponentTraits.h`](../../GanymedEngine/source/GanymedE/ECS/ComponentTraits.h) | Per-component opt-in flags + **`ComponentList`**, the single registry of components |
| [`AccessWrappers.h`](../../GanymedEngine/source/GanymedE/ECS/AccessWrappers.h) | The wrapper grammar (`Has/RO/RW/Opt*/No/React*`, `EntityId`) and pack traits |
| [`ComponentAccessor.h`](../../GanymedEngine/source/GanymedE/ECS/ComponentAccessor.h) | The read/write proxies with `Modify()` |
| [`ChangeBuffer.h`](../../GanymedEngine/source/GanymedE/ECS/ChangeBuffer.h) | Two-frame change log with virtual indices and per-consumer cursors |
| [`Views.h`](../../GanymedEngine/source/GanymedE/ECS/Views.h) | Slot/tuple machinery + `IterView`, `AccessView`, `InitView`, `FiniView`, `ChangeView` |
| [`ViewHolder.h`](../../GanymedEngine/source/GanymedE/ECS/ViewHolder.h) | Per-system persistent view state (CRTP) |
| [`System.h`](../../GanymedEngine/source/GanymedE/ECS/System.h) | `ISystem`, `System<Impl>`, `SystemManager` (+ ordering validation) |
| [`CommandQueue.h/.cpp`](../../GanymedEngine/source/GanymedE/ECS/CommandQueue.h) | Deferred structural changes, `PendingEntity` |
| [`Graveyard.h`](../../GanymedEngine/source/GanymedE/ECS/Graveyard.h) | One-frame storage of removed component instances (for FiniView) |
| [`Singleton.h`](../../GanymedEngine/source/GanymedE/ECS/Singleton.h) / [`SingletonTraits.h`](../../GanymedEngine/source/GanymedE/ECS/SingletonTraits.h) | Singleton access/change views over `registry.ctx()` |
| [`ViewDesc.h`](../../GanymedEngine/source/GanymedE/ECS/ViewDesc.h) | Access bitmasks derived from view declarations; feeds ordering validation |
| `*Tests.cpp` | Compile-time (`static_assert`) tests pinning the slot rules — they emit no code |

## Component registration

[`ComponentTraits.h`](../../GanymedEngine/source/GanymedE/ECS/ComponentTraits.h) holds the one list
every "for all components" operation iterates:

```cpp
using ComponentList = TypeList<TransformComponent, WorldTransformComponent, RelationshipComponent, ...>;
```

`IDComponent` and `TagComponent` are deliberately absent — they are entity identity, created by
`CreateEntityWithUUID` and never copied generically.

**Adding a component type** means:
1. Define the struct in `Scene/Components.h` (plain data, copyable).
2. Add it to `ComponentList`. This automatically gives it: `Scene::Copy` support, a slot in the
   `ViewDesc` bitmask space, and signal hookup if flagged below.
3. Optionally specialize `ComponentTraits<T>` to opt into features:
   - `TrackChanges` — writes must go through `Modify()`; enables `ChangeView<ReactRO<T>...>`.
     Currently: `TransformComponent`, `RelationshipComponent`.
   - `EnableInit` — component creation is recorded; enables `InitView`. Currently:
     `NativeScriptComponent`.
   - `EnableFini` — removed instances are buried in a graveyard for one frame; enables `FiniView`.
     Currently: `NativeScriptComponent`.
4. Add serialization in `SceneSerializer.cpp` and editor UI in `SceneHierarchyPanel.cpp` (these two
   are still per-component by hand).
5. If a `Scene` needs post-add fixup, declare a `Scene::OnComponentAdded<T>` specialization in
   `Scene.h` (see the `CameraComponent` one — it sizes the camera viewport).

## The wrapper grammar

A view's template arguments declare exactly what it includes, excludes, reads and writes:

| Wrapper | Filter | Access | Tuple slot |
|---|---|---|---|
| `Has<T>` | include | none | *(no slot)* |
| `No<T>` | exclude | none | *(no slot)* |
| `RO<T>` (or bare `T`) | include | read | `const T&` |
| `RW<T>` | include | write | `T&` if untracked, `ComponentAccessor` if tracked |
| `OptRO<T>` | none (optional) | read | nullable accessor |
| `OptRW<T>` | none (optional) | write | nullable accessor |
| `ReactHas/ReactRO/ReactRW/ReactOpt/ReactOptRO/ReactOptRW<T>` | as above | as above | as above, and the view *reacts to* T |
| `EntityId` | — | — | `entt::entity` |

Wrappers are variadic — `RW<A, B>` ≡ `RW<A>, RW<B>` — and tuple slots appear in declaration order.
Filters (`Has`, `No`, `ReactHas`, `ReactOpt`) constrain matching without producing a slot.

`Detail::AccessorPackTraits` flattens a pack into five type lists (`IncludeTypes`, `ExcludeTypes`,
`ReadTypes`, `WriteTypes`, `ReactTypes`) used by everything downstream.

## Accessors and the Modify() invariant

[`ComponentAccessor<T, Optional, Writeable, Tracked>`](../../GanymedEngine/source/GanymedE/ECS/ComponentAccessor.h)
is the proxy tuple slot. The load-bearing rule:

> For a component that is both change-tracked and writeable, `operator*`/`->` yield **const**, and
> the only way to get a mutable reference is **`Modify()`** — which logs the entity into that
> type's `ChangeBuffer` (at most once per accessor). A tracked+writeable slot is never collapsed to
> a bare `T&` anywhere; the `*Tests.cpp` static_asserts pin this.

Corollaries:

- Untracked writeable slots are plain `T&` (zero overhead); `Modify()` still exists on the
  optional accessor so call sites can be uniform.
- Writes that bypass views — editor panels, gizmos, `PhysicsScene::SyncTransforms` — must call
  **`Scene::MarkChanged<T>(entity)`** or downstream caches (the world-transform cache) go silently
  stale. If a change appears not to propagate, this is the first thing to check.
- Component **creation counts as a change**: `Scene`'s constructor connects `on_construct` for
  every tracked type, so a remove+re-add within one frame still surfaces in ChangeViews.

## ChangeBuffer

One per tracked component type, owned by `Scene`, holding **exactly two frames** of history.
Entries are addressed by a monotonically increasing *virtual index*, so any number of consumers can
each keep an independent cursor:

- `Add(entity)` appends to the current frame (duplicates allowed; consumers dedup).
- `NextFrame()` (called from `Scene::FrameBegin`) retires current → previous.
- `CollectSince(cursor, out)` returns everything logged after the cursor and the new cursor.
  A cursor older than the previous frame **asserts** — the consumer skipped a frame and events are
  gone. Cursor `0` means "never read" and is handled by the views (first read = whole world).

## The views

All views are **throwaway stack objects** — construct, iterate, discard; construction is O(1).
There is deliberately no archetype index: entt's sparse sets do the matching
(`registry.view<Is...>(exclude<Xs...>)`, `all_of`/`any_of`, `try_get`).

### IterView — iterate everything that matches

```cpp
using MeshView = ECS::IterView<ECS::EntityId, ECS::RO<WorldTransformComponent>, ECS::RO<StaticMeshComponent>>;

for (auto [entity, world, mesh] : View<MeshView>()) { ... }
```

Extras:
- `Subset<Us...>()` — same entity set, tuple narrowed to the listed components (EntityId kept).
- `Modify<Us...>()` — pre-calls `Modify()` on the listed tracked components: their slots become
  plain `T&` and the change is logged once per iterated entity. Use only when the loop writes
  *unconditionally*, or the change log fills with false positives.

### AccessView — random access by entity

```cpp
using ScriptAccess = ECS::AccessView<ECS::RW<NativeScriptComponent>>;
auto script = scripts.FindOne<NativeScriptComponent>(entity);   // nullable accessor
```

`Has(e)`; `Find/Get(e)` (full tuple — Find yields all-null accessors on a miss, Get asserts);
`FindOne/GetOne<U>(e)`; `FindSome/GetSome<Us...>(e)`. Requesting a type the view never declared is
a **compile error**, not a wrong answer.

### Reactive views — InitView / FiniView / ChangeView

All three answer "what happened since *this system* last looked", which is why their `State`
(cursors, epochs, scratch buffers) lives in the system's `ViewHolder`, not globally.

- **`InitView<ReactRW<T>, ...>`** — entities whose `T` was created since the last read. First-ever
  read reports *everything currently matching* ("the world is new"). Requires
  `ComponentTraits<T>::EnableInit`.
- **`FiniView<ReactRW<T>, ...>`** — entities whose `T` was removed (including via entity
  destruction). The react-type slots read the **graveyard copy** (see below); first-ever read
  reports nothing. Requires `EnableFini`. A FiniView may only access its react types — anything
  else on the entity may already be gone (enforced by static_assert).
- **`ChangeView<ReactRO<T>, ..., RW<Cache>>`** — entities whose tracked `T` was modified (via
  `Modify()`/`MarkChanged`/creation) since this system's last read. Cursor-based (exact across
  uneven cadences); first read = whole matching world; results are deduped and re-filtered against
  the entity's *current* state. Requires `TrackChanges`.

**The epoch protocol** (Init/Fini): `Scene::GetFrameEpoch()` increments every update; each view
state remembers the epoch it last read. Same epoch → empty (already reacted this update);
previous epoch → this update's buffers; older → **assert, events were lost**. The consequence:

> **Every reactive view must be read every update, in both runtime and editor paths.**
> `NativeScriptSystem::OnUpdateEditor` drains its views without acting for exactly this reason.

### Singleton views

For scene-wide state in `registry.ctx()` (see [scene.md](scene.md#singletons)):

```cpp
ECS::SingletonAccessView<PhysicsSettings> settings{ m_Scene };          // read-only
ECS::SingletonAccessView<ECS::RW<RenderContext>> render{ m_Scene };     // writeable
RenderContext& ctx = render.Get().Modify();                              // bumps the epoch
```

`SingletonChangeView<T>` reports "present *and* changed since my last read", driven by a per-type
epoch that `SingletonAccessor::Modify()` bumps (requires `SingletonTraits<T>::TrackChanges`;
currently only `RenderContext`).

## Systems

```cpp
class TransformSystem : public ECS::System<TransformSystem>
{
public:
    using DirtyView = ECS::ChangeView<ECS::EntityId,
        ECS::ReactRO<TransformComponent>,
        ECS::ReactRO<RelationshipComponent>,
        ECS::RW<WorldTransformComponent>>;
    using Views = TypeList<DirtyView>;          // <- ViewHolder allocates one State per entry

    using ECS::System<TransformSystem>::System;
    void OnUpdate(Timestep) override;           // View<DirtyView>() inside
    const char* Name() const override { return "TransformSystem"; }
};
```

- `System<Impl>` (CRTP) = `ISystem` + `ViewHolder<Impl>`. Declare `using Views = TypeList<...>`
  and call `View<V>()`; asking for an undeclared view is a compile error.
- `ISystem` hooks: `OnRuntimeStart/Stop` (play mode), `OnUpdate` (play), `OnUpdateEditor`
  (edit mode — default no-op).
- `Access()` returns the union `ViewDesc` of all declared views — derived, never hand-written.
- `ViewHolder` type-erases its state tuple behind an interface pointer because
  `Implementation::Views` is not nameable while the derived class is still incomplete.

### SystemManager

Owned by `Scene`; registration order **is** execution order. The built-in registration (in
`Scene`'s constructor) is: `PhysicsSystem` → `NativeScriptSystem` → `LuaScriptSystem` →
`TransformSystem` → `CameraSystem` → `RenderSystem`.

- **Lifecycle runs opposite to update order**: `OnRuntimeStart` iterates in reverse so scripts are
  instantiated *before* `PhysicsScene::Start` builds bodies (a rigid body added in a script's
  `OnCreate` must be in the initial simulation); `OnRuntimeStop` runs forward, stopping physics
  before the scripts it references die.
- **`ValidateOrdering()`** checks registration order against the declared access: if system A
  *only reads* component X and a later system writes X, A sees last frame's values — that is
  logged and counted, and the `Scene` constructor asserts the count is zero. Two writers of the
  same component are not flagged (an order is needed, but the declarations cannot pick it).
- `Get<S>()` gives direct system access — used only where no data path exists yet (the renderer
  fetching the live `PhysicsScene` for Jolt debug draw).

## Deferred structural changes

Adding/removing a component of an iterated type mid-iteration is UB in entt, so:

- **Inside systems** (while `Scene::IsUpdating()`): mutate through
  [`Scene::Commands()`](../../GanymedEngine/source/GanymedE/ECS/CommandQueue.h) —
  `AddComponent<T>(entity, args...)` (args captured by value), `RemoveComponent<T>`,
  `CreateEntity` (returns a `PendingEntity` you can queue components onto), `DestroyEntity`.
  Changes become visible **next frame**.
- **Outside the update loop** (editor panels, serializer, importers): keep using the immediate
  `Entity::AddComponent`/`RemoveComponent`/`Scene::DestroyEntity`. These **assert** if called while
  systems are running.

`CommandQueue::Flush` (called from `Scene::FrameBegin`) applies ops in a guaranteed order —
**removes → adds → creates (+ their components) → destroys** — which is what makes a same-frame
remove+re-add and "create an entity and give it components" both work. A queued add on an entity
that already has the component asserts (two callers queued the same add); an add on an entity that
died in the meantime is silently dropped (not an error).

## Graveyard

When a component with `EnableFini` is removed, `Scene`'s `on_destroy` handler copies it into a
per-type [`Graveyard`](../../GanymedEngine/source/GanymedE/ECS/Graveyard.h) (entt fires `on_destroy`
*before* the instance is freed, the only moment a copy is possible). FiniView slots read the buried
copy first, then fall back to the live pool (covers remove+re-add in one frame, where both a corpse
and a new instance exist). Graveyard accessors never get a change buffer — modifying a corpse is
not a change.

Lifetime: graveyards and the init/fini buffers are cleared in `Scene::FrameEnd`, i.e. **after** the
systems that needed them have run. (This is a deliberate deviation from the guide, which cleared at
FrameBegin: that would drop anything the editor removed between frames before any system could
react, leaking script instances.)

## Frame lifecycle (Scene::FrameBegin / FrameEnd)

Both `OnUpdateRuntime` and `OnUpdateEditor` run:

```
FrameBegin:
  m_FrameEpoch++                       // reactive-view epoch
  every ChangeBuffer.NextFrame()       // rotate change history
  FlushCommands()                      // apply queued structural changes
                                       //   -> fills init/fini buffers + graveyards via signals
m_IsUpdating = true
  systems run                          // structural changes only via Commands()
m_IsUpdating = false
FrameEnd:
  clear init/fini buffers + graveyards // this update's systems have seen them
```

## ViewDesc & ordering metadata

[`ViewDesc.h`](../../GanymedEngine/source/GanymedE/ECS/ViewDesc.h) projects a view's type lists
into `std::bitset` masks (Include/Exclude/Read/Write/React), indexed by position in
`ComponentList`. Today the payoff is `ValidateOrdering`; the same masks are what an automatic
scheduler would consume if the engine goes multithreaded (systems conflict iff
`A.Write ∩ (B.Read ∪ B.Write) ≠ ∅`). Auto-*reordering* is deliberately not implemented — ordering
stays explicit and validated.

## Rules that must never break

From the implementation guide, each guarding a class of silent bugs:

1. Tracked+writeable slots never collapse to `T&`; `Modify()` is the only write path.
2. `Modify()` logs at most once per accessor; graveyard accessors log never.
3. Component creation is a change (`on_construct` feeds ChangeBuffers).
4. Reactive views are read every update — the epoch/cursor asserts stay.
5. First-ever read of a Change/Init view = the whole matching world; of a Fini view = nothing.
6. Change history, init/fini buffers and graveyards live exactly one update.
7. Structural changes during system update go through `CommandQueue` only; code outside the update
   loop may use the immediate `Entity` API.
8. Writes that bypass views call `Scene::MarkChanged<T>()`.

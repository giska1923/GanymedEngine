# Milestone — Map Editor

**Status: planned. Nothing here is built.**

> **This document is on `first-game`, and the work it plans is not allowed to be.** Per
> [PROVING_GROUND.md](PROVING_GROUND.md)'s branch policy, everything under `GanymedEditor/source/`
> and `GanymedEngine/source/` lands on `master` first, and the merge direction is master → game — so
> nothing written on this branch ever arrives there. **Cherry-pick this file to `master` before
> executing any phase**, and delete this note when you do.

An in-editor toolset for authoring maps: a palette, a surface-snapping placement mode, a snap model
shared with the gizmo, a collider-versus-mesh audit, a scatter brush, gameplay markers, and a
top-down orthographic view.

---

## Why this, and why now

The evidence is in the one real map the engine has. `Game/assets/scenes/ProvingGround.ganymede` is
48 entities, and its buildings are hand-assembled entity-by-entity:

```
Warehouse
  Warehouse Mesh
  Warehouse Wall X+   Warehouse Wall X-   Warehouse Wall Z+   Warehouse Wall Z-   Warehouse Roof
Blockhouse
  Blockhouse Mesh
  Blockhouse Wall X+ … Blockhouse Wall Z+ Left / Right … Blockhouse Door Lintel
```

Each of those walls is a box collider whose half-extents and offset were **typed in by hand next to
a mesh that was authored somewhere else**. The predictable result is recorded in
[ToDo/README.md](README.md): the Warehouse had a 0.90 m hole in a wall of a building with no door,
the Blockhouse had three holes none of which were at its one real doorway, and P2's gate — *walk
inside and out of every building* — has therefore never been met for the Warehouse. P4's occlusion
probe was authored against one of those holes in the belief that it was the doorway.

That is not a careless-author story. Three things in the editor make it close to inevitable:

| Cause | Verified at |
|---|---|
| **Collider wireframes are drawn in Play mode only.** `ShowColliderGizmos` is pushed into `PhysicsSettings` inside the `SceneState::Play` branch and nowhere else, so while you are authoring a wall you cannot see its collider | `EditorLayer.cpp:355` |
| **A new `BoxColliderComponent` defaults to unit half-extents** regardless of the mesh it sits on, so every collider starts wrong and is corrected by typing | `Components.h:494` |
| **There is no way to ask "what world point is under the cursor".** GPU picking returns an entity ID, asynchronously, with no depth — so placement is done by typing numbers into the inspector or by dragging a gizmo against nothing | `SceneRenderer.h:69`, `EditorLayer.cpp:1113` |

A map tool that fixes those three is not a convenience feature. It removes a class of bug that has
already cost this project a failed gate and a probe that has to be re-run.

The second argument is throughput. The Proving Ground is one flat plane and two buildings because
that is what hand-authoring affords. Every engine question worth asking next — occlusion, streaming
budgets, draw-call scaling, audio falloff in enclosed space — needs a map with more in it than a
human will type.

## What it is

A **Map** panel plus a viewport mode. Concretely:

- A palette of prefabs, meshes and markers, pinned per project.
- **Placement**: pick a palette item, the thing follows the cursor across real surfaces, click to
  place. Grid snap, rotation snap, sit-on-bounds, align-to-normal.
- One snap model, shared by placement and the gizmo, replacing today's hard-coded `0.5 / 45°`.
- **Collider ↔ mesh parity**: colliders visible while editing, an audit that lists every collider
  that disagrees with its mesh, and a one-click *generate collider from mesh*.
- **Scatter brush**: paint prefab instances with density, jitter and a fixed seed; erase with the
  same brush.
- **Markers**: spawn points, patrol nodes and triggers as a real component with a viewport
  representation and a Lua query, instead of invisible tagged empties.
- **Top-down orthographic camera** for laying out a footprint.

## What it deliberately is not

- **Not terrain.** No heightfield, no sculpting, no splat materials, no `HeightFieldShape`. That is
  a renderer path, an asset type and a collider type — its own milestone, and it tests terrain
  rather than the interaction bugs this engine actually has.
- **Not brush/CSG geometry.** The Hammer-style alternative — author a box brush, generate its render
  mesh *and* its collider from one source of truth — would make collider/mesh disagreement
  structurally impossible rather than merely detectable. It was considered and not chosen: it needs
  a procedural-mesh path the renderer does not have, a new component and serialization, and a
  second authoring model beside prefabs. **If the parity audit in M2 keeps finding things after the
  tool exists, that is the signal to revisit this**, and the audit is what would tell us.
- **Not a level-streaming or sublevel system.** One scene, one file, as today.
- **Not prefab propagation.** Applying to a prefab still does not update instances already in the
  scene; that limitation is unchanged and is documented in
  [editor.md](../editor/editor.md#prefabs).

## Shape of the milestone, and its honest size

Seven phases. M0 is a hard prerequisite for M1, M3 and M4 — everything that places something needs
a synchronous ray. After that the order is by value, not by dependency.

| Phase | What | Size | Standalone value |
|---|---|---|---|
| **M0** | Synchronous edit-mode surface raycast | ~1 day | None on its own — it is the primitive |
| **M1** | Snap model + palette + placement mode | ~3 days | High. This is "the map tool" to a user |
| **M2** | Collider ↔ mesh parity | ~1.5 days | **Highest value per line in the milestone** |
| **M3** | Scatter brush | ~2 days | High for dressing, none for structure |
| **M4** | Gameplay markers | ~1.5 days | Medium; the only phase touching engine + Lua |
| **M5** | Top-down orthographic view | ~2 days, riskiest | Lowest. Cut this first |
| **M6** | Prove it on the Proving Ground | ~1 day | Closes two open gates |

**Two pieces of pushback, stated before the plan rather than after it.**

First, **M2 is worth doing on its own, immediately, even if the rest of this never happens.** Its
first step is setting one boolean in the Edit branch of `EditorLayer::OnUpdate`, and it makes the
bug that cost this project a gate visible while authoring. If the milestone gets cut to one phase,
cut it to that one.

Second, **M5 is the weakest item here and was included by request.** Most of its value — seeing a
map's footprint — is already had by orbiting the editor camera to look down. What it buys beyond
that is a true-scale view with no perspective foreshortening, which matters for sightlines and
spacing. What it costs is an orthographic path through `EditorCamera`, `ImGuizmo`, the grid shader
and — the actual risk — shadow-cascade fitting, which has never been run against a 200 m view
volume. Keep it last, and drop it without ceremony if M1–M4 run long.

---

## Phase M0 — a synchronous surface ray in edit mode

### Goal

`RaycastScene(scene, ray)` returns the world point, normal and entity under the cursor, this frame,
with no physics world.

### Why it cannot reuse what exists

| Existing | Why it does not serve |
|---|---|
| `SceneRenderer::RequestEntityID` / `PollEntityID` | Asynchronous under bgfx (~3 frames), and returns an **ID only** — no depth, so no world point. Fine for hover highlight, useless for "put the crate here" |
| `PhysicsScene::CastRay` | Needs Jolt bodies. Bodies exist only while playing — `CreateBodies` runs from `Start()` and reconciles per frame during play. Edit mode has no physics world at all |
| The depth attachment | Readback is the same asynchronous blit path, and reconstructing a world position from a depth sample means inverting the projection per pick for a value the CPU can compute exactly |

So: CPU, against render geometry. `Mesh` retains its vertices and indices after upload
(`Mesh.h:68`), which is what makes this cheap to write.

### Steps

1. **`Math::ScreenPointToRay`** (engine, `GanymedE/Math/Math.h`) — takes `inverse(viewProjection)`
   and an NDC point, returns `{ Origin, Direction }`. Engine-side because it is projection algebra,
   not editor policy, and the orthographic path in M5 needs it to keep working when the ray origin
   stops being the camera position.
2. **`EditorPicking.{h,cpp}`** (editor, new files → **premake regeneration required**):
   ```cpp
   struct SurfaceHit {
       Entity     Hit;              // invalid when FromWorkPlane
       glm::vec3  Point;            // world
       glm::vec3  Normal;           // world, always facing the ray
       float      Distance = 0.0f;
       uint32_t   Submesh  = 0;
       bool       FromWorkPlane = false;
   };
   SurfaceHit RaycastScene(const Ref<Scene>&, const Ray&, const RaycastFilter&);
   ```
3. **Broad phase**: one linear pass over `(WorldTransformComponent, StaticMeshComponent)`,
   `mesh->GetBounds().Transformed(world)` (already exists, `BoundingVolumes.h:23`) against a slab
   test. Collect `(entity, tMin)`.
4. **Sort by `tMin`, walk near→far, and stop as soon as the next candidate's `tMin` exceeds the best
   confirmed hit.** This is what keeps a brute-force loop honest — a cursor over the ground tests
   the ground's triangles and nothing else's.
5. **Narrow phase**: transform the ray into the entity's local space by `inverse(world)` — cheaper
   than transforming triangles — then Möller–Trumbore over each submesh's index range, skipping
   submeshes whose own `Bounds` the local ray misses.
6. **Normal**: `cross(e1, e2)` through `transpose(inverse(world))`, then flipped to oppose the ray
   direction. **Flip by ray direction, not by winding** — a negative-scaled entity inverts winding
   and would otherwise report an inward normal.
7. **Work-plane fallback**: on a miss, intersect the horizontal plane at `SnapSettings.GridHeight`
   and return `FromWorkPlane = true`. A ray pointing up at empty sky returns no hit at all — the
   difference matters, because placement should refuse rather than place something 400 m behind the
   camera.
8. **Filter**: skip entities in `SceneHierarchyPanel::HiddenEntities()`, skip the placement preview
   entity, and skip anything with an `AnimatorComponent`.

### Decisions, with reasoning

**Trace render geometry, not colliders.** Unity's in-editor placement traces colliders, because
Unity maintains a live physics world in edit mode; Unreal traces the editor world for the same
reason. Ganymed has no edit-mode physics world, and building one purely to place objects would mean
maintaining a shadow body set that updates on every collider edit — a second source of truth, for a
feature whose entire point is that the first source of truth is untrustworthy. Tracing triangles
also means the tool works on geometry that has no collider yet, which is the normal state of a
building halfway through being built.

**Skinned meshes are excluded, not approximated.** Their CPU vertices are the bind pose;
`Mesh::SkinnedBoundsPadding` exists precisely because the posed bounds are not the bind-pose bounds.
Reporting a hit against a pose the character is not in is worse than reporting nothing, and nobody
places a crate on an enemy's shoulder.

**No BVH.** A per-`MeshSource` BVH is the textbook answer and is the answer at scale. At the scale
that exists — 48 entities, `GroundTile` and box buildings — the sorted-broad-phase walk tests a few
hundred triangles per ray. Build a `TriangleBudget` (default 250 000 per raycast) and fall back to
the AABB hit point with a one-time warning when a single mesh blows it; that turns the missing BVH
from a silent hitch into a logged, visible limit. Revisit when a real scene trips it.

### Risks

- **Async loading**: a ray during a cold open hits meshes that are not resident yet. Fall back to
  the work plane and do not cache the miss.
- **Scale after M3**: a scatter brush can put 500 entities in the scene, all of which enter the
  broad phase. Linear AABB over 5 000 entities is still well under the budget below, but it is the
  first thing to measure again after M3 lands.
- **Precision** at grazing angles far from the origin. Clamp ray length to the editor camera's far
  clip.

### Verification

| Probe | Expected |
|---|---|
| Ray straight down from (3, 10, 3) over `GroundTile` | Hits `GroundTile`; `Point.y` = the pad's top surface ± 0.001; `Normal ≈ (0, 1, 0)` |
| Ray at a Warehouse wall face, oblique | `Point` on the wall plane; `Normal` = wall's outward normal, facing the ray |
| Ray into empty sky, above the horizon | No hit, `FromWorkPlane == false` |
| Ray at the ground with no mesh under it | `FromWorkPlane == true`, `Point.y == GridHeight` |
| Ray through an entity hidden in the outliner | Passes through to what is behind it |
| Entity with a negative scale component | Normal still faces the camera |
| 1 000 rays swept across the ProvingGround viewport, Release | Median and p99 ms logged; **budget: < 0.2 ms median** |

---

## Phase M1 — the snap model, the palette, and placement

### Goal

Pick a thing, point at a surface, click. It lands on the grid, sitting on the surface, facing where
you asked.

### Steps

1. **One snap settings struct**, owned by `EditorLayer`, surfaced in the viewport toolbar and read
   by *both* placement and `ImGuizmo`:
   ```cpp
   struct MapSnapSettings {
       bool  Enabled       = true;   // Ctrl inverts
       float Translate     = 0.5f;   // metres
       float Rotate        = 15.0f;  // degrees
       float Scale         = 0.1f;
       bool  SnapToSurface = true;
       bool  AlignToNormal = false;
       bool  SitOnBounds   = true;
       float GridHeight    = 0.0f;
   };
   ```
   This replaces the literals at `EditorLayer.cpp:1113-1117`.
2. **`Panels/MapPanel.{h,cpp}`** (new files → premake regeneration). Sections: palette, placement
   options, parity audit (M2), scatter (M3), markers (M4).
3. **Palette**: enumerate candidates with `AssetManager::ForEachAsset` filtered to `Prefab` and
   `StaticMesh`; a pinned subset is what the palette shows. Rows are `AssetTint` icon + name —
   **there is no thumbnail system and this milestone does not build one**.
4. **Palette persistence** at `<project>/.editor/map_palette.yaml`. Project-relative, not
   `imgui.ini`: a palette is a fact about the *content*, and `imgui.ini` is per-install window
   layout that no one wants merged.
5. **Placement mode**: clicking a palette item instantiates the item **once** as a live preview
   entity — `SceneHierarchyPanel::InstantiatePrefab` for a prefab, `MeshImporter::Instantiate` for a
   mesh — and thereafter only its transform is written per frame. Esc or RMB cancels and destroys
   it.
6. **Per-frame transform**, in this order:
   - `Point` from M0's raycast;
   - quantize the **hit point** to `Translate` in world axes — quantize the point, never the final
     origin, or the sit-on-bounds offset gets rounded too;
   - `+ bounds offset` when `SitOnBounds`: the mesh's local `Bounds.Min` projected onto the
     placement axis, so a crate authored around its centre sits *on* the floor rather than half
     through it. **This is the single setting that makes modular kits usable**;
   - rotation = accumulated yaw (`[` / `]` step by `Rotate`), composed with the align-to-normal
     quaternion when enabled;
   - then **`Scene::MarkChanged<TransformComponent>`** — writing the component directly is invisible
     to change tracking and the world-transform cache goes stale. The gizmo already learned this
     (see [editor.md](../editor/editor.md#viewport)); placement must not relearn it.
7. **Commit**: LMB pushes one `AddEntitiesCommand` for the placed subtree, then instantiates a fresh
   preview so placement chains. Shift+LMB keeps the mode, plain LMB exits it after one placement.
   Alt+LMB places unsnapped.
8. **Duplicate along an axis** in the panel: count, spacing, axis — one `CompositeCommand`. A run of
   six identical crates should be four fields, not six drags.

### Decisions, with reasoning

**Snapping becomes on-by-default and Ctrl *disables* it.** Today Ctrl *enables* a hard-coded
0.5 / 45°. Unity defaults snapping off with Ctrl to enable; Unreal and Blender default it on with a
modifier to disable. For a *map* tool the Unreal/Blender default is right — modular kit pieces only
line up if snapping is the resting state, and the failure mode of accidentally-off snapping (a wall
0.03 m from its neighbour, a seam you find in play) is much worse than accidentally-on. **This is a
behaviour change you will feel on the first drag**, so it is called out rather than slipped in. The
45° rotation step is kept as a preset next to the new 15° default.

**The preview is a real entity, not a wireframe ghost.** The alternative — draw the mesh's wire
bounds with `Renderer3D::DrawWireBox` — costs nothing and pollutes nothing, but shows a box where
the author needs to see a silhouette against real geometry. Drawing the actual mesh translucently is
not available: unlit/alpha variants of the mesh shader do not exist, and
[editor.md](../editor/editor.md#viewport) already records why a Lit/Unlit dropdown was omitted.
So the preview is the real instantiated entity. Two consequences, accepted rather than engineered
around: it appears in the outliner while placement is active, and a crash mid-placement leaves a
stray entity in memory — never on disk, since nothing saved it. A dedicated preview-entity registry
that hides it from every panel is more machinery than a transient state deserves.

**The preview must be excluded from the M0 filter**, or the first frame of placement snaps the
object to itself.

### Risks

- Re-instantiating a prefab every frame would stall; instantiate once, move the transform. Stated
  because it is the obvious first implementation and it is wrong.
- `AddEntitiesCommand` snapshots a subtree by UUID; placement must push **after** the transform is
  final, or undo restores the preview's last hover position.

### Verification

| Probe | Expected |
|---|---|
| Place 10 crates on flat ground, `Translate = 0.5`, `SitOnBounds` on | Every X/Z is an exact multiple of 0.5; every crate's `Bounds.Min.y` in world equals the floor's surface ± 0.001 |
| Save, reload | Transforms byte-identical in the `.ganymede` file |
| Ctrl+Z ten times | Entity count returns to the pre-placement value; outliner shows no residue |
| Place on a sloped/rotated surface with `AlignToNormal` | Object's local +Y equals the surface normal ± 0.5° |
| Gizmo-drag with snapping on, no Ctrl | Moves in `Translate` steps (the behaviour change above) |
| Duplicate-along-axis, count 6, spacing 2.0 | Six entities, exact spacing, **one** undo entry |

---

## Phase M2 — collider ↔ mesh parity

### Goal

The Warehouse hole is impossible to author without seeing it, trivial to find if it already exists,
and one click to fix.

### Steps

1. **Show colliders while editing.** `RenderSystem::DrawColliderGizmos` reads
   `BoxColliderComponent` / `SphereColliderComponent` / `CapsuleColliderComponent` directly and
   needs no physics world (`RenderSystem.cpp:197`) — it is simply never asked to run in Edit. Add
   the toggle to the viewport's Visualizers popup, default **on** in Edit, and push
   `PhysicsSettings::ShowColliderGizmos` from the `SceneState::Edit` branch as well. The engine
   default stays `false`, so a non-editor front-end is unaffected — the same opt-in shape the Play
   branch already uses and for the same reason.
2. **Seed a new box collider from the mesh.** When the editor's add-component path adds a
   `BoxColliderComponent` to an entity that has a `StaticMeshComponent` with a resident mesh, set
   `HalfExtents = (Bounds.Max - Bounds.Min) * 0.5` and `Offset = (Bounds.Max + Bounds.Min) * 0.5`.
   Both are in the entity's local space and are scaled by the world matrix at draw and at body
   creation, so **no division by the entity's scale** — confirmed against the gizmo's own transform
   composition, `world * translate(Offset) * scale(HalfExtents * 2)`.
3. **Generate collider from mesh** as an explicit action, on the selection, recorded as one
   `ComponentEditCommand<BoxColliderComponent>` per entity inside a `CompositeCommand`.
4. **The audit**, a Map panel section walking every entity with both a mesh and a box collider:

   | Finding | Test |
   |---|---|
   | `No collider` | Mesh, no collider of any kind, and not excluded by a filter |
   | `Collider smaller than mesh` | Per-axis world-AABB delta > tolerance (default 0.02 m) |
   | `Collider larger than mesh` | Same test, other sign — usually harmless, occasionally a snag |
   | `Offset mismatch` | Centres differ by > tolerance while extents agree |

   Each row selects and frames its entity, and draws that entity's mesh bounds and collider bounds
   in two colours via the existing `DrawWireBox`.
5. **A footprint roll-up per building root**: union of descendant collider AABBs versus union of
   descendant mesh AABBs, reported as a coverage figure.

### The honesty clause

**This does not detect a hole between two correctly-sized colliders.** The Warehouse bug was a
collider *shorter than its wall*, which the per-entity delta catches outright. A genuine interior
gap where two well-formed boxes fail to meet is a coverage problem, and answering it properly means
voxelizing the shell or casting a grid of probe rays — neither is attempted here. The roll-up
narrows where to look; it does not certify. Say that in the panel's own text, not only here: an
audit that implies a guarantee it does not make is worse than no audit.

### Decisions, with reasoning

**Collision is per-entity, not per-asset.** Unreal generates simple collision at *import* and stores
it on the static mesh asset; Unity computes bounds when a `BoxCollider` is added to a renderer.
Ganymed's colliders live on components, so the same crate mesh can carry different collision per
placement — more flexible, and the reason the wrong value can be typed in the first place.
**The better long-term answer is a collision default in the mesh's `.meta` sidecar**, so a crate
brings its collider with it and placement never types anything. That is a real design change with an
asset-format consequence and it is not folded in here — it is written into
[assets.md](assets.md) as a follow-up instead.

### Risks

- Turning collider gizmos on by default in Edit adds debug-line geometry to every frame of every
  scene. `DrawLine` accumulates and flushes in `EndScene`, so it is a batch, not N draws — verify
  that the draw-call count does not move.
- The audit walks every entity every frame if written naively. Run it on demand and on scene load,
  cache the result, invalidate on a relevant component change.

### Verification

| Probe | Expected |
|---|---|
| Open ProvingGround in Edit | Collider wireframes visible without pressing Play; draw calls unchanged ± 1 |
| Run the audit on ProvingGround as committed | Reports the buildings' current state; if the scene fix has landed and it reports clean, **reintroduce a 0.9 m shortfall by hand and confirm the audit names that entity** |
| Generate-from-mesh on that wall, re-run | Zero findings for it; the `.ganymede` diff is exactly that component |
| Add a `BoxColliderComponent` to a mesh entity | Half-extents match the mesh, not `(0.5, 0.5, 0.5)` |
| Undo the generate | Prior half-extents restored, one Ctrl+Z for a multi-selection |
| Play, walk every building's interior and exit | **P2's open gate. Record the result in PROVING_GROUND.md** |

---

## Phase M3 — scatter brush

### Goal

Paint rubble, crates and props in strokes, reproducibly, with one undo entry per stroke.

### Steps

1. **Brush state**: prefab (or a weighted list), radius, density (instances/m²), minimum spacing,
   yaw jitter, uniform scale range, align-to-normal, surface filter, and a **seed**.
2. **Stroke**: on mouse-down, seed a `Random` (`Core/Random.h`, PCG32, the same generator the
   particle system uses per emitter) from the stroke seed. Each frame, sample candidate points in
   the brush disc around the M0 hit, drop each straight down onto a surface with a second ray,
   reject any closer than the minimum spacing to an instance already placed this stroke (spatial
   hash), and instantiate the rest.
3. **Grouping**: instances are parented under a lazily created `Scatter/<PrefabName>` entity, so the
   outliner does not become 500 siblings.
4. **Eraser**: the same brush with Shift held deletes instances of the active group within the
   radius.
5. **Undo**: one `AddEntitiesCommand` composite on mouse-up for a paint stroke, one
   `DeleteEntitiesCommand` composite for an erase stroke. Never one entry per instance.
6. **Caps**: `MaxInstancesPerStroke` (default 500) and a scene-wide soft warning. The runtime spawn
   cap set this precedent for the same reason — an unbounded authoring action is a way to lose work.

### Decisions, with reasoning

**Scattering produces ordinary entities, and that is the phase's real limitation.** Unity's tree and
detail systems and Unreal's Foliage mode both store scattered instances as *non-entity* instance
arrays — `FFoliageInstance` behind a hierarchical instanced static mesh — precisely because one
entity per pebble does not scale: each one costs a transform, a world transform, change-tracking
membership, a line in the scene file and a slot in every view that touches meshes.

Ganymed has no instance-array component, so v1 produces entities, and the cap exists because of it.
The renderer *does* batch — `Renderer3D` reports `InstancedDraws`, and instances sharing a mesh and
material will collapse into few draw calls — so the pain is CPU-side per-entity work and scene file
size, not draw calls. **Measure both and record where the wall is**; the follow-up, a
`ScatterVolumeComponent` holding a transform array submitted through the existing instanced path,
belongs in ToDo and not in this milestone.

**A fixed seed per stroke, stored on the group.** Re-running a stroke with the same seed, radius and
density must reproduce it. Without that, "the scatter looks wrong, undo and redo it slightly
differently" is not a workflow.

### Risks

- Entity-count explosion, above.
- Scene file growth: 500 instances is roughly 500 × the per-entity YAML block. Measure the file size
  and load time before and after.
- A stroke crossing a building places rubble on roofs. The surface filter is the answer; default it
  to "the surface I started the stroke on".

### Verification

| Probe | Expected |
|---|---|
| Paint ~200 instances in one stroke | One undo entry; Ctrl+Z removes all 200 |
| Same seed, same path, twice | Identical transforms (compare the serialized blocks) |
| Draw-call count with 200 instances of one prefab | `InstancedDraws` rises; `DrawCalls` roughly flat |
| Frame time in Release, before/after 500 instances | Logged; the delta is the honest cost of the entity model |
| Scene file size and load time, before/after | Logged |
| Erase stroke | Removes only the active group's instances; one undo entry |

---

## Phase M4 — gameplay markers

### Goal

Spawn points, patrol nodes and triggers are visible while editing and queryable from Lua, instead
of being empties with an agreed-upon name.

### Steps

1. **`MarkerComponent`** in `Scene/Components.h`:
   ```cpp
   struct MarkerComponent {
       std::string Kind  = "Spawn";
       glm::vec4   Color = { 0.2f, 0.9f, 0.35f, 1.0f };
       float       Size  = 0.5f;
       bool        DrawForward = true;
   };
   ```
   Registered in `ComponentList` (which gets it into undo snapshots for free — see
   [editor.md](../editor/editor.md#undo--redo)), reflected in `ComponentReflection.cpp` (which gets
   it into the generic inspector and the generic serializer), and that is the whole engine-side
   data cost.
2. **Drawing**: `RenderSystem` draws a `DrawWireSphere` at the origin plus a `DrawLine` forward
   arrow, behind an editor-opt-in flag with exactly the shape `ShowColliderGizmos` has — engine
   default off, editor sets it per frame.
3. **Lua**: `Scene.FindMarkers(kind)` returning a table of entities, and `Entity:GetMarkerKind()`.
   **This is the phase's real cost.** `ScriptBindings.cpp` is hand-written flat getters per
   component (`GetParticleRateOverTime`, `GetLinearVelocity`, …) and `Scene` currently exposes only
   `FindEntityByName`, `FindEntityByUUID` and `Spawn` — there is no generic component access and no
   "find all of kind" query, so both are new hand-written bindings over a scene-wide view.
4. **Palette**: markers sit beside prefabs in the Map panel; placing one creates an entity with a
   `TagComponent` and a `MarkerComponent`.
5. **The viewport Icons toggle** that [editor.md](../editor/editor.md#viewport) deliberately omitted
   for having no backing feature now has one. Add it here and update that list in the same change.

### Decisions, with reasoning

**`Kind` is a string, not an enum.** An enum would give compile-time checking and a fixed palette —
and it would live in engine source, which
[the branch policy](PROVING_GROUND.md#branch-policy) forbids the game from touching. A game that
wanted a `Patrol` node would have to land an engine change, merge it to the game branch, and only
then author the node. That is the wrong dependency direction for what is fundamentally game data.
The cost is typos that only show up as a Lua query returning nothing; the mitigation is a list of
known kinds in the Map panel's project config driving the palette and colours, while an unknown kind
still round-trips untouched.

**Markers are a component, not an editor-side convention over tags.** A tag convention needs no
engine change at all and was considered. It fails on two counts: the editor would be guessing which
empties are markers by parsing names, and the *data* on a marker — a patrol node's wait time, a
spawn's team — would have nowhere typed to live, which is how `ScriptComponent` field overrides end
up carrying it as loose strings.

### Risks

- 100 markers must be a debug-line *batch*, not 100 draw calls. `Renderer3D::DrawLine` accumulates
  and flushes in `EndScene`, so it should be — verify rather than assume.
- A new component touches `ComponentList`, reflection registration and undo's `EntitySnapshot`
  tuple. The reflection milestone made this a registration rather than a code sprawl; confirm the
  self-validation in `ComponentReflection.cpp` still passes.

### Verification

| Probe | Expected |
|---|---|
| Place 8 patrol markers, save, reload | All 8 present, kinds and colours intact |
| `Scene.FindMarkers("Patrol")` from Lua | Returns 8 entities |
| 100 markers on screen | `DrawCalls` rises by the debug-line batch count, not by 100 |
| Inspector on a marker | Generic reflected section, no hand-written drawer needed |
| Undo a marker placement, redo | Kind and colour survive the snapshot round-trip |

---

## Phase M5 — top-down orthographic view

### Goal

A true-scale plan view for laying out footprints and sightlines.

### Steps

1. **`EditorCamera` gains a projection mode**: `Perspective | Orthographic`, plus `OrthoHeight`.
   `UpdateProjection` branches; `MouseZoom` adjusts `OrthoHeight` instead of `m_Distance` in ortho.
   It is perspective-only today (`EditorCamera.h`).
2. **Viewport camera combo** gains a *Top (Ortho)* entry beside Editor Camera and the scene cameras.
   Pitch locked to −90°.
3. **`ImGuizmo::SetOrthographic(true)`** — the call site already exists for scene cameras with an
   orthographic projection and just needs to see this mode too.
4. **The grid.** `Renderer3D::DrawGrid` scales a quad by a fixed `100.0` and fades in the shader
   against world XZ (`Renderer3D.cpp:1014`). From 200 m up, the grid ends well before the view does.
   Both the scale and the fade distance have to derive from the camera's visible extent, which is a
   shader change as well as a C++ one. **Size this honestly — it is most of the phase.**
5. **A metres-per-pixel readout** in the viewport header, beside the existing `Free Aspect` chip.

### Risks

- **Shadow cascades are the real risk.** Cascade fitting has only ever run against a perspective
  editor camera; a 200 m orthographic view volume will either blow the cascade extents out to
  uselessness or produce a degenerate split. Check this first, before building the rest of the
  phase — if cascade fitting has to be clamped or driven by a separate "shadow camera", that is a
  larger change than the camera mode itself and is grounds for cutting the phase.
- Frustum culling under ortho is fine in principle — `Frustum::FromViewProjection` is
  Gribb–Hartmann and projection-agnostic — with one wrinkle noted under *Found while planning*
  below: the near plane uses the OpenGL convention while the workspace builds with
  `GLM_FORCE_DEPTH_ZERO_TO_ONE`. It is conservative, so ortho culls correctly but slightly
  over-includes. Not a blocker; worth fixing separately.

### Verification

| Probe | Expected |
|---|---|
| Switch to Top (Ortho) over ProvingGround | Whole map visible, no perspective convergence on the building walls |
| Measure a known 10 m span against the readout | Within 1 % |
| Gizmo-drag in ortho | Handles track the cursor 1:1; `ImGuizmo` is in orthographic mode |
| Grid at 20 m, 80 m, 200 m ortho height | Grid fills the view at every height, fade is stable |
| Shadows in ortho | Compared against the perspective view: either correct, or the phase is cut and the reason recorded here |
| Frustum culling in ortho | `CulledMeshes` non-zero when the view is tight; nothing pops that should be visible |

---

## Phase M6 — prove it on the Proving Ground

The milestone's own gate. Not "the tool works" — *the tool fixed something that was broken*.

1. **Rebuild the Warehouse with the tool**, from nothing, and time it against the hand-authored
   original (which git history still holds). Record both numbers.
2. **Run the parity audit over the whole map.** Target: zero findings.
3. **Re-run P2's open gate** — walk inside and out of every building, in `GanymedRuntime`, both
   doorways. This gate has never been met for the Warehouse.
4. **Re-run P4's occlusion probe on the `+Z` side**, whose original probe positions were authored
   against a hole mistaken for a doorway.
5. **Author the step-up ledge the ToDo list has been asking for**: something between 0.2 m and
   `CharacterControllerComponent::StepHeight` (0.4), so where step-up stops working is measured
   rather than inferred. With a placement tool and grid snap this is a two-minute job, which is
   rather the point.
6. Write the results into [PROVING_GROUND.md](PROVING_GROUND.md) and strike the corresponding
   entries from [README.md](README.md).

---

## Explicitly not doing

| Not doing | Why, and what would change if we did |
|---|---|
| Heightfield terrain | A renderer path, an asset type and a Jolt collider type. Its own milestone |
| Brush / CSG geometry | Would make collider-mesh disagreement structurally impossible; needs a procedural-mesh path and a second authoring model. M2's audit is what tells us whether we still need it |
| Vertex / edge / face snapping | Unreal's V-key, Blender's snap-to-vertex. Needs a per-mesh vertex acceleration structure — the same BVH M0 defers |
| Asset thumbnails in the palette | Needs an offscreen thumbnail renderer and a disk cache. Icons and names are honest; a blank thumbnail grid is not |
| Instance-array scattering | The right answer at scale, and the reason M3 has a cap. Written into ToDo instead |
| Level streaming / sublevels | One scene, one file, unchanged |
| Navmesh, lighting bake, occlusion volumes | PROVING_GROUND cut navmesh on purpose and nothing since has changed that argument |
| Prefab propagation to live instances | Still v1 behaviour; unchanged by this milestone |
| Undo for asset writes | The editor's undo stack is the scene's, deliberately. The palette file and any `.meta` change stay outside it |

## Design tensions, recorded

1. **The placement ray traces render geometry; the game traces colliders.** That is deliberate — the
   tool that finds collider bugs must not depend on colliders being right — but it means you can
   place a crate on a wall that has no collision and see nothing wrong. M2's audit is the answer,
   and the two halves have to ship together for the tool to be trustworthy.
2. **Snapping flips from opt-in to opt-out.** Right for modular authoring, and a change to muscle
   memory on day one.
3. **Scatter produces entities where production engines produce instance arrays.** A known ceiling,
   bounded by a cap rather than hidden.
4. **Marker `Kind` is a string because of the branch policy.** Type safety traded for the game's
   ability to define its own vocabulary without an engine change. If the branch policy ever ends,
   revisit.
5. **The placement preview is a real scene entity.** Visible in the outliner during placement;
   accepted rather than engineered around.
6. **The palette is editor state with no scene representation.** A map cloned to another machine
   carries no palette. Putting the palette in the scene file was rejected: it is a tool preference,
   and it would make every author's UI state a merge conflict on the one file two people are most
   likely to edit at once.
7. **M0 duplicates, in the editor, a capability Jolt already provides.** Justified today because
   edit mode has no physics world. If one ever appears — for collider preview, or in-editor
   simulation — this becomes a second raycast implementation, and the choice of which is
   authoritative has to be made deliberately. Recorded so that is a decision and not a discovery.

## Docs this milestone must update

| Phase | Doc |
|---|---|
| M0 | [engine/core.md](../engine/core.md) for `Math::ScreenPointToRay`; [editor/editor.md](../editor/editor.md) for the editor-side raycast |
| M1 | [editor/editor.md](../editor/editor.md) — the Map panel, the snap model (**and the changed gizmo-snap semantics in the Controls table**), placement |
| M2 | [editor/editor.md](../editor/editor.md); [engine/rendering.md](../engine/rendering.md) for edit-mode collider gizmos; [engine/physics.md](../engine/physics.md) if the collider-seeding rule is described there |
| M3 | [editor/editor.md](../editor/editor.md) |
| M4 | [engine/scene.md](../engine/scene.md) for `MarkerComponent`; [engine/scripting.md](../engine/scripting.md) for the Lua surface; [editor/editor.md](../editor/editor.md) for the palette and the Icons toggle |
| M5 | [engine/rendering.md](../engine/rendering.md) for the camera mode and the grid; [editor/editor.md](../editor/editor.md) for the viewport combo |
| M6 | [PROVING_GROUND.md](PROVING_GROUND.md), and strike the closed entries from [README.md](README.md) |

`GanymedE/Math/` has no row in AGENTS.md's doc-mapping table; M0 is the first thing to notice it.
Treat `core.md` as its home and say so there, or add the row.

**New source files in M0, M1 and M4 mean premake regeneration** — `GanymedEditor/premake5.lua`
globs `source/**`, and globs are expanded at generation time, not at build time.

## Found while planning, out of scope

Three things this plan absorbed as work (edit-mode collider gizmos, unit-default box colliders,
hard-coded gizmo snap) and one it did not:

- **`Frustum::FromViewProjection` extracts the near plane with the OpenGL convention.** It uses
  `row(3) + row(2)` (`w + z ≥ 0`), which is right for GL's `[-1, 1]` depth. The workspace defines
  `GLM_FORCE_DEPTH_ZERO_TO_ONE` for every project (`premake5.lua:29`), where the near plane is
  `z ≥ 0` — `row(2)` alone. The plane it computes is therefore behind the true near plane, making
  the frustum strictly larger: culling is **conservative**, so nothing renders incorrectly, and it
  has been invisible for that reason. Written into [rendering.md](rendering.md).

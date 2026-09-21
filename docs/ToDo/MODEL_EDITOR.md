# Milestone — Model Asset Editor

**Status: P1–P3 done. P4–P7 planned.**

> **Same branch rule as [MAP_EDITOR.md](MAP_EDITOR.md).** Every phase touches
> `GanymedEditor/source/` or `GanymedEngine/source/`, which the
> [branch policy](PROVING_GROUND.md#branch-policy) puts on `master`; the game branch receives it by
> merge and never sends anything back. Build these off `master`, not off `first-game`.

An inspector for assets rather than entities: select a `.glb` in the Content Browser and see what it
contains, what it will import as, and what it costs — with a real 3D preview, import settings that
write to the sidecar, and a collision default the map editor can consume.

---

## Why this, and what it is not

**This is not a modeller.** Vertex-and-face editing inside the engine is not planned, not in scope,
and would be a bad trade: the pipeline is glTF, Blender already does it, a writable mesh format does
not exist, and the engine would learn nothing from owning it. The defensible part of that idea —
parametric blockout primitives that generate mesh and collider together — is a different tool and is
recorded as the deferred CSG option in [MAP_EDITOR.md](MAP_EDITOR.md#what-it-deliberately-is-not).

What *is* missing is everything between "a file exists in `assets/`" and "an entity references it":

| Gap | Verified at |
|---|---|
| **There is no asset inspector at all.** The Content Browser lists and drags; the Properties panel inspects entities. Selecting a `.glb` shows nothing. Everything known about a mesh is learned through an entity that happens to reference it | `SceneHierarchyPanel.cpp:1917`, `ContentBrowserPanel.h` |
| **Import settings are plumbed end-to-end and unreachable.** `AssetMeta::Config` is a flat key/value map with an `ImportConfigVersion` for migration; `CompiledCache` hashes it into the epoch so a change invalidates the compiled output; `TextureCompiler` already reads `Format`, `NormalMap`, `GenerateMips` and `MaxSize`. **Nothing in the editor writes a key** — the only way to set one is to hand-edit a `.meta` in a text editor | `AssetMeta.h:31`, `CompiledCache.cpp:378`, `TextureCompiler.cpp:47` |
| **`MeshCompiler` reads no config at all.** No import scale, no axis correction, no tangent policy — the tangent rule is hard-coded "generate when the file has none" with no way to force or suppress it | `MeshCompiler.cpp`, [assets.md](../engine/assets.md#tangents-are-generated-when-the-file-has-none) |
| **Nothing can render an asset outside the scene.** No preview, no thumbnails — which is why [MAP_EDITOR.md](MAP_EDITOR.md) had to cut palette thumbnails explicitly | `SceneRenderer.h` |
| **A mesh carries no collision default**, so every placement types its own box — the authoring failure the map editor's M2 exists to catch | `Components.h:494` |

The last two are why this milestone pays twice: the preview renderer is also the map palette's
thumbnails, and the collision default is what turns M2's *"generate collider from mesh"* from a
repair action into something that never needed repairing.

## Phase order, and the honest size

**P1–P3 deliver most of the value and none of the risk.** P4 is the one structural change in the
milestone and everything visual depends on it, so it sits after the cheap wins have already shipped
and before the two phases that need it.

| Phase | What | Size | Depends on |
|---|---|---|---|
| **P1** | Asset Inspector panel — readouts, no preview | **done** | — |
| **P2** | Import settings written to `AssetMeta::Config` | **done** | P1 |
| **P3** | Collision default on the mesh asset | **done** | P2, and pairs with [MAP_EDITOR](MAP_EDITOR.md) M2 |
| **P4** | Multi-target rendering: view-ID bases | ~1.5 days | — (**the risk**) |
| **P5** | The asset preview renderer | ~2 days | P4 |
| **P6** | Thumbnails: Content Browser and map palette | ~1.5 days | P5 |
| **P7** | Docs and a measured pass | ~0.5 day | all |

~9.5 days. **P4 is the phase to read first** — if its refactor turns out worse than it looks, P5 and
P6 are cut and P1–P3 still stand on their own.

---

## Phase P1 — the Asset Inspector panel

### Goal

Click a `.glb` in the Content Browser; see what it is.

### Steps

1. **`Panels/AssetInspectorPanel.{h,cpp}`** (new files → **premake regeneration**). Dockable, next
   to Properties in the default DockBuilder tree — which means **bumping the dock-layout version**
   in `imgui.ini` (`[GanymedEditor][Dock] Version`, currently 2), or an existing ini keeps a tree
   with no node for the new panel.
2. **Content Browser selection has to become readable.** `m_Selected` is private and nothing outside
   the panel reads it. Add an accessor plus a changed-callback; `EditorLayer` wires it to the
   inspector the same way `RetargetPanels` wires the hierarchy.
3. **Header, every type**: relative path, `AssetHandle`, `AssetType`, file size, sidecar present /
   missing / quarantined, and whether a compiled output exists for the current epoch.
4. **Mesh body**:
   - submesh table — index, name, material index, triangle count, local bounds;
   - vertex and index counts, total bounds, and whether skin data is present;
   - the material slot list, reusing the slot rows already written for
     `DrawComponent<StaticMeshComponent>` but in *asset defaults* terms rather than per-entity
     overrides;
   - `MaterialSerializer::GenerateSidecars` as a button — it already exists and already extracts
     embedded textures to real files;
   - skeleton summary (joint count, clip list with durations) when the mesh has one.
5. **Texture body**: dimensions, source format, mip count, compiled format and size.
6. **Warnings the importer already knows and currently only logs**, surfaced where an author will
   see them:
   - more than one `cgltf_skin` (only the first is imported);
   - a normal map present with no `TANGENT` attribute (tangents are generated — say so);
   - **mirrored UV shells**, because `MeshVertex::Tangent` is a `vec3` and glTF's tangent `w` is
     dropped, so a mirrored shell lights as though it were not
     ([assets.md](../engine/assets.md#tangents-are-generated-when-the-file-has-none)). The engine
     cannot fix this without a vertex-format change; the inspector can at least stop it being a
     mystery.

### Decisions, with reasoning

**A separate panel, not the Properties panel.** Unity puts assets and entities in one Inspector and
pays for it with a selection model that has to arbitrate between two domains; Unreal opens a
separate editor window per asset, which is heavier than ImGui docking justifies. A third dockable
panel is the middle path: the Properties panel stays entity-scoped and multi-select-aware, and no
entity code path has to start asking "is this selection actually an asset?" It costs one dock node.

**The asset transaction model is already decided, and this follows it exactly.** `.gmat` editing
(`DrawMaterialAssetEditor`, `SceneHierarchyPanel.cpp:1105`) states the rule in its own comment: edits
are live on the shared `Ref` and visible everywhere immediately, they are **not undoable** because
the undo stack is the scene's, and Save / Revert are the transaction — with Revert being
`AssetManager::Reload`. Do not invent a second model here. Do repeat the warning text: a global edit
that looks local is the worst version of this UI.

### Risks

- The panel reads a mesh that may not be resident. Every section must tolerate `GetAsset` returning
  null for a frame or two — that is the async loading contract, and
  [assets.md](../engine/assets.md#there-are-no-placeholders-and-that-is-a-decision) is explicit that
  there are no placeholders.
- Triangle counts over a large mesh are cheap but not free; compute once per selection, not per
  frame.
- The first two warnings are *source* facts, lost on a compiled-cache load. P1 reads them with
  `MeshImporter::InspectSource` (glTF JSON, no buffers) rather than bumping the mesh blob: a
  compiler-version bump would invalidate every `.gres` for three UI strings. A huge `.glb` can
  hitch once on select; persist the bits in P2 if that shows up.

### Verification

| Probe | Expected |
|---|---|
| Select `BoxTextured.glb` | Submesh, vertex, index and material counts match what the compiled blob holds |
| Select a rigged `.glb` | Joint count and clip durations match `Animation.h`'s data |
| Select a `.glb` mid-load | Panel shows "loading", no crash, fills in when resident |
| Select a texture, a `.gmat`, a `.lua` | Each gets its body or an honest "no inspector for this type" |
| Select an asset whose sidecar was deleted | Reported as missing rather than silently re-minted |

---

## Phase P2 — import settings, written to `AssetMeta::Config`

### Goal

Change an import setting in the editor; the asset recompiles and reloads.

### Steps

1. **`AssetManager::SetAssetConfig(handle, AssetConfig)`** — the write API that does not exist. It
   must: rewrite the sidecar through `AssetMetaSerializer::Write` (already atomic, tmp + rename),
   update the in-memory index entry's `Config`, then `Reload(handle)`.
2. **Texture settings first**, because they already work end-to-end and need only widgets:
   `Format` (auto / BC1 / BC3 / BC5 / BC7 / raw), `NormalMap`, `GenerateMips`, `MaxSize` — the exact
   four keys `TextureCompiler::Compile` reads, with the same defaults, so the UI cannot disagree
   with the compiler about what "unset" means.
3. **Mesh settings**, which need compiler support as well as UI:

   | Key | Default | What it does |
   |---|---|---|
   | `ImportScale` | `1.0` | Uniform scale baked at import. The common fix for a DCC that exports in centimetres |
   | `TangentPolicy` | `WhenMissing` | `WhenMissing` is today's hard-coded behaviour, kept as the default so nothing changes silently. `Always` overrides a file's own tangents; `Never` trusts them |
   | `UpAxis` | `Y` | glTF is Y-up by spec; `Z` corrects an exporter that ignored it |

   Applied in `MeshImporter::Import`, which runs on a worker and feeds the compiler — and since
   `CompiledCache` hashes `Config` into the epoch, a changed key invalidates the blob with no new
   invalidation machinery.
4. **A "Reimport" button** that forces the round trip even when nothing changed, for when a source
   file was edited by a tool the watcher's stamp did not catch.

### Decisions, with reasoning

**No hot-reload storm, and the reason is worth knowing.** `AssetWatcher` stamps
`metadata.FilePath` — the asset file — not its sidecar (`AssetWatcher.cpp:158`). Writing a `.meta`
therefore does **not** trip the watcher, which is why `SetAssetConfig` has to call `Reload` itself,
and also why doing so cannot double-fire.

**Keys stay flat scalars.** `AssetMeta`'s own comment is explicit that nested config is deliberately
not representable and that needing it is a format version bump, not a `YAML::Node` in a public
header. Respect that: if a setting wants structure, it wants a version bump and a migration switch
in the importer, which is exactly what `ImportConfigVersion` was added for and has never yet been
used for.

**Unknown keys already survive a rewrite** — the serializer reads and writes them back untouched, so
a sidecar from a newer engine is not truncated by an older one. The new write path must not break
that; it is the one property that makes forward-compatible sidecars possible.

**The default values live in the compiler, not the UI.** `ConfigBool(config, "GenerateMips", true)`
is where "unset means mips on" is decided. The panel must read the same defaults rather than
restating them, or an unset sidecar and a UI-default sidecar start behaving differently.

### Risks

- **`Reload` drops the loaded object**, so every `AssetRef` re-resolves. It is documented as safe
  mid-frame, but a reimport of a mesh used by 500 scattered entities is a visible hitch — measure it
  once with the map editor's scatter output rather than guessing.
- A bad `ImportScale` silently rescales every existing placement of that mesh. It is an asset-level
  edit with scene-level consequences and is not undoable; the panel should say so in the same words
  the `.gmat` editor uses.

### Verification

| Probe | Expected |
|---|---|
| Set `MaxSize = 512` on a 2048 texture | Sidecar rewritten; compiled output rebuilt; the Stats panel's **Compiled** line counts one build |
| Reopen the project | The setting persists and the second run builds **0** ([assets.md](../engine/assets.md#compiled-outputs)) |
| Hand-add an unknown key to the sidecar, then change a setting in the UI | The unknown key is still there afterwards |
| Set `ImportScale = 0.01` on a mesh | Entities using it render 100× smaller; bounds and the generated collider follow |
| `TangentPolicy = Never` on a file with no tangents | Normal mapping degrades exactly as the pre-generation code did — the setting is doing what it says |
| Reimport a mesh used by 500 entities | Hitch measured and recorded, Release |

---

## Phase P3 — a collision default on the mesh asset

**Done.** `Collision` (`None` | `Box`, default `None`) lives on the mesh sidecar. `MeshImporter::Instantiate`
(viewport drop, map place, scatter) adds a fitted `BoxColliderComponent` when it is `Box`. Add-component
and M2 generate-from-mesh share `MeshCollision::SeedBoxCollider`, which reads `Mesh::GetBounds()`.
`Collision = None` is today's behaviour. The inspector combo shows the fitted half-extents / offset as
numbers; the 3D overlay waits on P5.

### Decisions, with reasoning

**The asset default is a seed, never an override.** Collision lives on the component, per placement,
and that stays true — the same crate mesh must be able to carry different collision in two places.
Unreal stores simple collision on the static mesh asset and treats the component as the exception;
Ganymed inverts it because its colliders were always component-side and the flexibility is real.
What the asset default removes is only the *typing*, which is where the wrong numbers came from.

**`Box` only.** Sphere and capsule are placement decisions, not properties of a mesh, and convex
hulls do not exist — [PROVING_GROUND](PROVING_GROUND.md#what-it-deliberately-is-not) cut mesh
colliders on purpose and nothing has changed that.

**One Config key, not two.** The plan asked for fitted extents cached alongside `Collision`. Those
extents are already on the compiled mesh: `Mesh::ComputeBounds` runs at `BuildMesh`, and
`GetBounds()` is O(1). Writing them back into Config would hash into the epoch and force a
recompile every time the cache was refreshed (and go stale the moment `ImportScale` changed).
`Collision` itself is authoring metadata, not an importer input — `ConfigAffectsCompile` excludes
it, and `SetAssetConfig` skips `Reload` when only that hash is unchanged, so flipping a crate to
`Box` does not evict the live mesh.

**Add-component and generate still live-fit.** They do not wait on `Collision = Box`. A mesh with
no default must still get a sensible collider when the user asks for one; the key only decides
whether *placement* brings one unasked.

### Verification

| Probe | Expected |
|---|---|
| Set `Collision = Box` on the crate mesh, drag it into the viewport | New entity arrives with a fitted `BoxColliderComponent` |
| Run M2's parity audit over a map built this way | Zero findings, with no repair action having been run |
| A mesh with `Collision = None` | Behaves exactly as today |

---

## Phase P4 — multi-target rendering: view-ID bases

### Goal

`SceneRenderer` can be instantiated more than once. This is the blocker for everything visual.

### The problem, precisely

`RenderPassIDs.h` is a table of **absolute** view-ID constants, and the scene IDs are hard-coded at
the submit sites: `Renderer3D` at `Renderer3D.cpp:268` and `:817`, `Renderer2D` at three places
around `:148`, `Environment.cpp:440`, and `SceneRenderer.cpp:88-137`. A second `SceneRenderer` would
bind *its* framebuffer to view 73 as well — and within one `bgfx::frame()` a view has exactly one
framebuffer, so the second bind wins and both renderers draw into the same target.

So a preview cannot simply be "another `SceneRenderer`", and it cannot be "the same views, earlier in
the frame" either.

### Steps

1. Turn the pass constants into **offsets from a base**, and give `SceneRenderer` a `viewBase`.
2. `Renderer3D` / `Renderer2D` / `Environment` read an **active base** rather than the constant.
   `SceneRenderer::BeginFrame` sets it; `EndFrame` restores it.
3. **Assert that scene renders do not nest.** `Renderer3D`'s `s_Data` is a single static frame
   state, so a preview render must be a complete, sequential `BeginFrame … EndFrame` outside the
   main one — never inside it. Make that an assert, not a comment.
4. **Budget the ranges.** `BGFX_CONFIG_MAX_VIEWS` is 256 (`extern/bgfx/src/config.h:315`). The main
   renderer occupies 0–96 and the editor's ImGui pass sits at 200, so 97–199 is free. A preview
   range needs neither `EnvironmentBake` (67 views — it shares the scene's already-baked
   environment) nor `Shadow` (4), so ~24 views at 100–123 is ample.

### Decisions, with reasoning

**Reuse the whole pass chain rather than writing a preview shader.** The tempting cheap version is a
standalone forward draw with its own shader and a fixed light rig — no refactor, one view ID. It is
rejected because the preview would not go through the same material and tonemap path, so it would
not match the viewport. **A preview that lies about how the asset looks is worse than no preview**,
and the moment someone tunes a material against it, the lie costs real time.

**One global "active base", not a renderer instance handle.** `Renderer3D` is already a static
singleton with static frame state; threading a renderer pointer through every submit would be a much
larger change that buys nothing until there are genuinely concurrent scene renders, which bgfx's
single submit thread does not offer anyway. The honest cost is a second piece of global state, and
the mitigation is the nesting assert above.

**The preview shares the scene's baked environment rather than baking its own.** `EnvironmentBake`
is a prepass at view 1 whose ordering was itself a bug fix ([RenderPassIDs.h](../../GanymedEngine/source/GanymedE/Renderer/RenderPassIDs.h)
records the 24–26 ms it used to cost), and baking a second environment per preview would reintroduce
exactly that hitch.

### Risks

- **This refactor can silently change the main viewport.** It touches the ordering table that decides
  the whole frame. The verification below is a pixel comparison for that reason, following the
  precedent set by [BGFX_MIGRATION.md](../history/BGFX_MIGRATION.md)'s backend-parity checks.
- An off-by-one in a base offset produces a pass drawing into a neighbouring view — which looks like
  a missing effect, not like a crash.

### Verification

| Probe | Expected |
|---|---|
| Same scene, same frame, before vs after the refactor | **Pixel-identical** on D3D11; mean per-pixel difference 0.000 |
| All four backends after the refactor | Render, pick and agree on colour, as [ToDo/rendering.md](rendering.md) records they do today |
| A second `SceneRenderer` drawing a cube into its own target | Its output appears; the main viewport is untouched |
| A nested `BeginFrame` | Asserts, in Debug, at the nesting site |
| View-ID range overrun at construction | Asserts with the requested base and count named |

---

## Phase P5 — the asset preview renderer

### Goal

A live, orbitable 3D preview of the selected asset, rendered on demand.

### Steps

1. **`AssetPreview` service**: handle → framebuffer + camera state, created lazily, evicted with the
   selection.
2. **No scratch `Scene`.** The preview submits one mesh through `Renderer3D::SubmitMesh` with a
   fixed light rig and the environment, directly. A `Scene` would drag in systems, singletons, a
   change tracker and a second entity registry to draw one mesh.
3. **Render on demand, not per frame** — a dirty flag set by camera movement, selection change, and
   `AssetManager::AddAssetChangedListener` (which already exists; it is how the prefab template
   cache is invalidated when a `.gprefab` changes on disk).
4. **A per-frame preview budget**, N renders per frame, mirroring
   [the Apply budget](../engine/assets.md#the-apply-budget) — same reasoning, same shape, and it is
   what makes P6's thumbnails affordable rather than a stall.
5. Auto-frame from `Mesh::GetBounds()`; LMB orbits, wheel zooms; camera state remembered per asset
   for the session.
6. Skinned meshes render their **bind pose**, labelled as such in the panel. `Mesh`'s CPU vertices
   are the bind pose and `SkinnedBoundsPadding` exists precisely because the posed bounds differ —
   showing a pose the asset is not in would be the same lie P4 refused.

### Decisions, with reasoning

**A fixed studio environment for previews, not the open scene's.** Sharing the *baked* environment
is a performance decision (P4); using the *current scene's* HDR is a correctness question, and the
answer is no — an asset would look different depending on which scene happened to be open, so two
authors comparing the same material would disagree. Bake one studio environment for the preview
renderer and keep it for the session. The cost is that a preview does not predict how the asset
looks in *this* map; the viewport is where that question belongs.

### Risks

- Per-frame cost if the dirty flag is wrong — an always-dirty preview is a second full scene render
  every frame. Verify the render count, not the frame time.
- Framebuffer churn: one target per selected asset, not one per asset in the project.

### Verification

| Probe | Expected |
|---|---|
| Select a mesh, do not touch it | Preview renders **once**; render count stays flat over 100 frames |
| Orbit the preview | One render per input frame, none while idle |
| Edit the `.gmat` a mesh uses | Preview updates without a selection change |
| Frame time with the panel open and idle, Release | Within noise of the panel closed |
| A 200k-triangle mesh | Renders; the budget defers it rather than dropping a frame |

---

## Phase P6 — thumbnails

### Goal

The Content Browser grid and the map editor's palette show what the asset looks like.

### Steps

1. Render thumbnails through P5's budgeted queue, **only for grid cells actually visible**, and only
   when the grid is idle.
2. Cache to disk beside the other compiled outputs, keyed by the **same epoch** — so a reimport or a
   config change invalidates the thumbnail for free rather than needing its own invalidation rule.
3. Fall back to the existing `AssetTint`-ed type icon while a thumbnail is pending or absent. It is
   the current behaviour, so nothing regresses if the queue is saturated.
4. Wire the map editor's palette to the same source. This closes the item
   [MAP_EDITOR](MAP_EDITOR.md#explicitly-not-doing) cut explicitly.

### Risks

- Scrolling a large folder must not queue hundreds of renders. The visible-cells rule plus the
  budget is the answer; verify with a folder of 200 meshes.
- Thumbnail cache size. Small images, but they are per-asset — report the total in the Stats panel's
  Asset Cache readout rather than letting it grow unobserved.

### Verification

| Probe | Expected |
|---|---|
| Open a folder of 200 meshes, scroll fast | No frame over budget; thumbnails fill in behind the scroll |
| Reopen the project | Thumbnails come from cache; **0** re-rendered |
| Change an import setting | That asset's thumbnail is rebuilt, its neighbours are not |
| Map palette | Shows the same thumbnails, from the same cache |

---

## Phase P7 — docs and a measured pass

1. Import one real model cold with the panel open and record: import time, compiled size, preview
   render cost, thumbnail cost.
2. Set a collision default on the Proving Ground's building meshes and re-run the map editor's
   parity audit — target zero findings with no repair action.
3. Update the docs listed below, and strike the closed items from [README.md](README.md).

---

## Explicitly not doing

| Not doing | Why |
|---|---|
| Mesh geometry editing (vertices, faces, extrude) | Blender does it; the pipeline is glTF; the engine has no writable mesh format. Weeks of work to duplicate a tool you already run |
| LOD generation | There is no LOD system to generate *for*. That is a renderer milestone, not an inspector feature |
| Mesh optimization (vertex cache, overdraw, simplification) | Means a new third-party dependency (meshoptimizer), which AGENTS.md requires sign-off for. Worth asking about separately |
| Convex-hull or mesh colliders | Jolt supports them; the engine deliberately has box/sphere/capsule only |
| Animation clip editing, retargeting, socket authoring | [SKELETAL_ATTACHMENTS.md](SKELETAL_ATTACHMENTS.md)'s territory. P1 *reports* skeleton and clip data; it does not edit it |
| A separate OS window per asset (the Unreal model) | Heavier than ImGui docking warrants for a single-window editor |
| Texture channel packing or editing | An image editor, not an asset inspector |
| Undo for asset edits | The stack is the scene's, deliberately. Save / Revert is the asset transaction model and this milestone follows it |

## Design tensions, recorded

1. **P4 adds a second piece of renderer global state** (the active view base) on top of
   `Renderer3D`'s existing static frame state, and makes nested scene renders illegal. Asserted
   rather than documented, because the failure mode — a pass drawing into a neighbouring view — looks
   like a missing effect rather than a bug.
2. **Assets and entities are separate selection domains.** A separate panel keeps them apart at the
   cost of one dock node; merging them is Unity's model and its arbitration cost.
3. **Asset edits sit outside undo**, and this milestone makes that rule far more visible than
   `.gmat` editing did: changing `ImportScale` silently rescales every placement of a mesh, is not
   undoable, and is two clicks away.
4. **The asset's collision default is a seed, not an override.** Tempting to make the asset
   authoritative; that would remove the ability to give one mesh different collision in two places,
   which is a capability the component model currently has for free.
5. **The preview uses a fixed studio environment**, so it deliberately does *not* predict how an
   asset looks in the open map. Comparability across scenes was judged the more valuable property.
6. **`AssetMeta::Config` is flat by design.** The first setting that genuinely wants structure is a
   format version bump and a migration switch — and `ImportConfigVersion` has existed unused since
   the asset pipeline milestone precisely for that day.

## Docs this milestone must update

| Phase | Doc |
|---|---|
| P1 | [editor/editor.md](../editor/editor.md) — the Asset Inspector panel, and the dock-layout version bump |
| P2 | [engine/assets.md](../engine/assets.md) — the `.meta` Config section becomes "settings you can set", plus the mesh keys; [editor/editor.md](../editor/editor.md) |
| P3 | [engine/assets.md](../engine/assets.md); [engine/physics.md](../engine/physics.md); [MAP_EDITOR.md](MAP_EDITOR.md) M2, whose generate-action now has a source |
| P4 | [engine/rendering.md](../engine/rendering.md) — the view-ID table becomes a table of *offsets*, and that is the section's whole ordering story |
| P5 | [engine/rendering.md](../engine/rendering.md); [editor/editor.md](../editor/editor.md) |
| P6 | [editor/editor.md](../editor/editor.md) — Content Browser; [engine/assets.md](../engine/assets.md) — the thumbnail cache beside the compiled outputs |
| P7 | [README.md](README.md) — strike what closed |

**New source files in P1 and P5 mean premake regeneration** — `GanymedEditor/premake5.lua` globs
`source/**`, expanded at generation time.

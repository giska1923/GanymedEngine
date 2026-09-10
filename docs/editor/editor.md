# GanymedEditor

The editor application (`GanymedEditor/source/`). It is a thin client of the engine: one
`Application` subclass ([`GanymedEditorApp.cpp`](../../GanymedEditor/source/GanymedEditorApp.cpp))
pushing a single [`EditorLayer`](../../GanymedEditor/source/EditorLayer.h), plus two panels.
Run it with `GanymedEditor/` as the working directory (assets resolve relative to CWD); a scene
path may be passed as `argv[1]`.

## Layout

A dockable ImGui workspace. On first run (no `imgui.ini` yet) `EditorLayer::OnImGuiRender` builds
a default layout with DockBuilder: toolbar strip on top (no tab bar), Scene Hierarchy left,
Properties below it, Viewport center, Stats right, Content Browser bottom. After that, layout
changes persist in `GanymedEditor/imgui.ini`.

## EditorLayer

Owns the `SceneRenderer` (HDR target + post stack), the active/editor `Scene` pair, the
`EditorCamera`, panels, and the play/edit state machine.

### Per-frame (`OnUpdate`)

1. Resize the scene renderer / editor camera / scene cameras when the viewport panel size changed.
2. `SceneRenderer::BeginFrame` (bind + clear HDR target, entity IDs to −1).
3. Update the scene: `OnUpdateEditor(ts, editorCamera)` in Edit,
   `OnUpdateRuntime(ts, &editorCamera)` in Play (the editor camera is the fallback when the scene
   has no primary `CameraComponent`; the physics-debug toggles **and `ShowColliderGizmos = true`**
   are pushed into the scene's `PhysicsSettings` each frame). The gizmo flag is engine-default
   **false** so a non-editor front-end draws no collider wireframes — the editor opts in, and it has
   to do so every frame because `Scene::Copy` does not carry singletons onto the play-mode scene.
4. **Hover picking**: mouse position → viewport-local coordinates (Y flipped only when
   `bgfx::getCaps()->originBottomLeft` — render-target origin is backend-dependent), then
   `RequestEntityID` + `PollEntityID`. Picking is asynchronous under bgfx (~3 frames latency),
   invisible for hover highlighting. The result feeds `m_HoveredEntity` (shown in Stats,
   click-to-select).
5. `SceneRenderer::EndFrame` — bloom → tonemap → FXAA → composite.

### Viewport

- Shows the composite target via `ImGui::Image`; UVs flip vertically per
  `originBottomLeft` (a render target's orientation follows the backend — hard-coding either way
  is wrong on half of them).
- **Event blocking**: `ImGuiLayer::BlockEvents(false)` while the viewport is hovered/focused, so
  camera and shortcut input reaches the layer.
- **Drag-drop from the Content Browser** via `EditorUI::AcceptAssetDrop` (see
  [below](#typed-drag-drop)): a `Scene` drop opens the scene; a `StaticMesh` drop (edit mode only)
  instantiates it via `MeshImporter::Instantiate` and selects it.
- **Gizmos** (edit mode, with a selection): ImGuizmo manipulates the entity's **world** transform
  (`Scene::GetWorldSpaceTransform`, so parented entities gizmo correctly), converts back to local
  through the parent's inverse world matrix, decomposes (`Math::DecomposeTransform`), applies
  rotation as a delta to avoid gimbal jumps — and then calls
  **`Scene::MarkChanged<TransformComponent>`**, because a direct component write is invisible to
  change tracking and the world-transform cache would go stale (the entity would keep rendering at
  its pre-drag position). Ctrl snaps (0.5 units, 45° for rotation).

### Controls

| Input | Action |
|---|---|
| Alt+LMB drag / MMB drag / scroll | Orbit / pan / zoom the editor camera |
| LMB in viewport | Select hovered entity (ignored over the gizmo or with Alt held) |
| Q / W / E / R | Gizmo: hide / translate / rotate / scale (ignored while using the gizmo or RMB-flying) |
| Ctrl+Z / Ctrl+Y / Ctrl+Shift+Z | Undo / redo (Edit state only) |
| Ctrl+D / Delete | Duplicate / delete the selected entity, subtree included (Edit state only) |
| Ctrl+N / Ctrl+O / Ctrl+S / Ctrl+Shift+S | New / Open / Save / Save-As scene |
| Ctrl (held while dragging gizmo) | Snapping |
| Ctrl+U | Toggle the RmlUi game-UI Debugger (also View → Game UI Debugger; Debug builds only) |
| F1 | bgfx stats overlay |

**Two shortcut layers, on purpose.** Q/W/E/R and Ctrl+U go through the engine event path
(`EditorLayer::OnKeyPressed`), which `ImGuiLayer::BlockEvents` gates on viewport focus/hover.
Everything else - undo, redo, duplicate, delete, and the file shortcuts - is polled inside the
ImGui frame by `EditorLayer::HandleShortcuts` using `ImGui::IsKeyChordPressed`, so it fires
wherever the mouse is.

The file shortcuts used to live on the engine path too, and dead-zoned over every panel: Ctrl+Z
above the Properties panel simply did nothing. Relaxing `BlockEvents` was the alternative and is
worse - it would leak *every* key into the engine path while typing in a panel, firing camera
keys and gizmo-mode switches mid-rename. A command layer above widget focus is the production
norm, and polling ImGui inside the ImGui frame is that layer at this scale. Q/W/E/R stay
viewport-gated deliberately for the same reason.

`HandleShortcuts` returns early on `ImGui::GetIO().WantTextInput`: while a text field is focused,
Ctrl+Z is ImGui's own text undo, which is what every editor does.

## Undo / redo

[`EditorUndo.h`](../../GanymedEditor/source/EditorUndo.h) - `EditorCommand`, `EditorUndoStack`,
and the command types. `EditorLayer` owns the stack; the hierarchy panel records into it.

**Scope: scene edits only.** Inspector property edits, add/remove component, create / delete /
duplicate entity, re-parenting and gizmo drags are undoable. Asset-level edits are deliberately
not - a scene-local stack would lie about their scope, since undoing one would silently change
every scene using that asset. Unity draws the same line for most asset properties; Unreal's
transaction system does cover assets, and Ganymed diverges toward Unity's model because it has no
per-asset dirty/transaction infrastructure.

The stack lives editor-side rather than in the engine: the runtime has no consumer for undo. This
mirrors how `EditorCamera` lives engine-side while the *editing model* does not.

| Piece | Notes |
|---|---|
| `EditorUndoStack` | Linear, capped at 100, `Push` clears the redo stack. `MarkSaved`/`IsDirtySinceSave` track dirtiness by stack *position*, so undoing back to the saved state correctly clears it |
| `ComponentEditCommand<T>` | Before/after values. The after-value is filled in at the commit boundary, not at construction |
| `AddComponentCommand<T>` / `RemoveComponentCommand<T>` | Remove stores the whole value, so undo is a re-add rather than a default-construct |
| `AddEntitiesCommand` / `DeleteEntitiesCommand` | One subtree-snapshot mechanism, differing only in which way `Undo` runs. Create, duplicate and delete are all built on it |
| `ReparentCommand` | Records the **old sibling index** explicitly - `Scene::SetParent` push_backs, and since the canonical save order is a hierarchy DFS, sibling order is content |

**Every command keys entities by UUID**, resolved through `Scene::FindEntityByUUID`. `entt::entity`
handles are not validity-checked by `Entity::operator bool` and do not survive a destroy/recreate
cycle, so a raw handle in an undo record is a dangling reference waiting for a redo. A command
whose UUID no longer resolves warns and does nothing.

**Snapshots are in-memory component tuples, not YAML.** `EntitySnapshot` holds a
`tuple<optional<Ts>...>` over `ComponentList`, filled through `ForEachType`. That is lossless -
it round-trips `AnimatorComponent::Time`, which the serializer deliberately drops, and it
preserves the *absence* of a `ScriptComponent` field override, which means something different
from "present and equal to the default". It also keeps undo correctness from depending on
serializer completeness, and a new component type joins undo for free; the YAML path could not
promise that, since its per-component lists are hand-maintained.

Restoring a tracked component calls `Scene::MarkChanged<T>` - writing one behind the change
tracker's back is the silent-staleness trap, where the value moves and the cached world transform
keeps its pre-undo matrix.

**The selection is the one thing keyed by handle, not UUID**, so it is validated once a frame
(`ValidateSelection`). Undo can destroy the selected entity behind the panel's back — Ctrl+Z on a
"Create Entity" is exactly that — and `Entity::operator bool` does not check registry validity, so
the stale handle would look live all the way into `GetComponent`.

**Scope guards.** Recording happens only when the panel holds a non-null stack pointer, and
`EditorLayer::RetargetPanels` passes `nullptr` in Play state - so play-mode edits to the throwaway
scene copy are structurally unrecordable rather than filtered downstream. The stack survives
play/stop (`m_EditorScene` is the same object) and is cleared by New/Open Scene, where every UUID
in it stops meaning anything.

## Prefabs

An authored subtree becomes a `.gprefab` asset; instances spawn from it, remember it, and can be
re-applied or reverted. The format and its ownership rules are in
[scene.md](../engine/scene.md#prefabs-gprefab); what follows is the editor half.

| Operation | Where | Undoable |
|---|---|---|
| **Create Prefab…** | Entity context menu | The file write is not; linking the source entity is (it is a component add) |
| **Instantiate** | Drop a `.gprefab` on the viewport, or the hierarchy's blank-space menu | Yes — a subtree add |
| **Apply to Prefab…** | Instance context menu, and a button in the inspector | **No** — it is an asset write |
| **Revert Instance** | Same two places | Yes — one composite command |

**Create** links the source entity to the file it just wrote, so the thing you made a prefab *from*
becomes an instance of it. That is the Unity behaviour authors expect. The path must be inside
`assets/`; a prefab outside the asset root gets no handle and no sidecar, so nothing could reference
it.

**Apply** is the milestone's one silently destructive click — it overwrites an asset, and undo
covers scene edits only — so it is the one operation behind a confirmation modal. The modal names
the file and says the thing an author would otherwise have to discover: **other instances already in
the scene do not update**. There is no propagation in v1.

**Revert** deletes everything below the root and rebuilds it from the file, keeping the root entity
itself: its UUID, so references to it survive, and its transform, because placement belongs to the
instance. It is a scene edit, so it *is* undoable — as a single `CompositeCommand` holding the
delete of the old subtree and the add of the new one, which is why one Ctrl+Z takes you back to the
pre-revert state rather than halfway. If instantiation fails, the captured subtree is restored
rather than leaving a hole.

Structural freedom inside an instance is **allowed and unmarked**: add, remove and re-parent
children at will. "Create from selection" means the *primary* selection's subtree — prefab actions
are single-entity even though the selection no longer is.

### Per-property overrides

A field of a prefab instance that differs from the prefab is **tinted blue in the inspector**, and
right-clicking it offers **Revert to Prefab**. A component with any differing field gets a `*` on its
section header.

**Overrides are computed, not stored.** Unity records an override list on the instance; Ganymed diffs
the instance against the prefab instead. A recorded list is a second source of truth that goes stale
when the prefab changes, needs migrating when a field is renamed, and has to be maintained by every
edit path. A diff cannot be stale — it is recomputed from the two things it compares — and it needs
no format change beyond the canonical link.

The comparison is the serializer's own: **a field that would serialize identically is not an
override** (`EmitReflectedValue`). That keeps "overridden" and "would be written differently" the
same statement. A type with no YAML codec reports *not* overridden — "cannot tell" must not become a
claim.

| | |
|---|---|
| What makes it possible | `PrefabMemberComponent::CanonicalID` on every instantiated entity — see [scene.md](../engine/scene.md#prefab-member-links) |
| Per-field affordance | Reflected sections only; the per-field hook lives in the property drawer |
| Hand-written sections | Section-level `*` marker only — "something in here differs", not which field |
| Cost | **0.14 ms/frame** worst case (a selected prefab instance with a particle emitter: 46 fields, section marker plus every per-field query, Release). Zero when nothing selected is a prefab member |

Two limitations worth knowing:

- **Prefab instances already in committed scenes have no canonical link**, because they were
  instantiated before it existed. They report no overrides until they are re-instantiated — which
  *Revert Instance* does, since it rebuilds the subtree from the file.
- The template cache is keyed on the prefab handle and dropped when the scene changes. Editing a
  `.gprefab` on disk while a scene is open will not refresh it until the scene is reopened; the
  cache has no way to notice a file edit on its own.

### Play / Stop (toolbar)

```
Play: m_ActiveScene = Scene::Copy(m_EditorScene); OnRuntimeStart(); panels retarget the copy
      UIEngine::LoadDocument("assets/ui/hud.rml")
Stop: UIEngine::CloseAllDocuments(); OnRuntimeStop(); m_ActiveScene = m_EditorScene; selection cleared
```

Game UI (RmlUi) is loaded on Play and closed on Stop, and renders *inside* the viewport image
rather than over the whole editor — `RenderPass::UI` composites into the same LDR target the
viewport displays. The document path is hard-coded for now. Details: [ui.md](../engine/ui.md).

While playing, `OnEvent` forwards input to `UIEngine` **before** the editor's own handlers, but
only when the viewport is hovered or focused — otherwise clicking a panel would be routed at a HUD
sitting underneath it. If the UI claims the event (pointer over an actual widget, or a focused UI
element taking a key), `EditorLayer::OnEvent` returns early and the gizmo/selection shortcuts never
see it. Mouse coordinates are translated by `m_ViewportBounds[0]`, the same origin picking uses.

The runtime scene is a disposable UUID-keyed deep copy — physics and scripts can do anything to
it, and Stop restores the authored scene untouched. Scene switching (`OpenScene`) stops play
first.

New scenes are seeded by `SetupDefaultEnvironment`: a "Sun" (directional light tilted from above,
shadow-casting) and a "Sky Light" (HDR environment `environments/studio_small_08_1k.hdr` when
present, procedural fallback otherwise), so imported meshes are lit immediately.

### Stats panel

Hovered entity, Renderer2D/3D counters (draw calls, quads, meshes, frustum-culled, instanced,
transparent, particle emitters/billboards/draws/culled), an **Asset Cache** readout (below),
live post-processing settings (exposure, bloom threshold/knee/intensity/radius,
FXAA), and Jolt debug-draw toggles (visible during Play; draws Jolt's body state instead of the
authored collider gizmos).

A **Compiled** line sits under them: assets built this session, the wall clock they cost, and how
many came out of `assets/.compiled/` instead. A second run over an unchanged project must read
0 built ([assets.md](../engine/assets.md#compiled-outputs)).

The Asset Cache rows come from `AssetManager::GetCacheStats()`, one per registered manager, printed
as `resident / tracked / loading` — live objects, cache entries including ones whose object has
already been collected, and parses in flight. The gap between the first two *is* eviction: close a
scene and resident falls while tracked does not, until those handles are loaded again. The third
column is a cold open in progress (see
[assets.md](../engine/assets.md#asynchronous-loading)).

`Apply: N done, M deferred, x / 4.0 ms` is the main-thread half of asynchronous loading against its
per-frame budget. A non-zero **deferred** during a burst is the budget working, not a backlog — those
parses are finished and land on the next frames (see
[assets.md](../engine/assets.md#the-apply-budget)).

Below it, **Hot reload assets/** is the watcher's switch, with `watched / ms-per-poll / reloaded /
settling` under it and the last reloaded path. Editing an asset in an external tool updates the
viewport within about half a second with no restart. Two of those numbers are worth reading rather
than ignoring:

- **ms/poll** is what decides whether this stays a poll. It is 0.5-1.5 ms for this project's assets;
  the roadmap's alternative is a Win32 `ReadDirectoryChangesW` watcher, worth writing the day this
  column shows up in a frame and not before.
- **settling** is changes seen but not yet acted on - a debounce in progress, or a batch deferred
  because more than 16 files changed at once.

Turn the switch **off before a large `git` operation** and back on afterwards: a branch switch
touching hundreds of assets is a change event for every one of them. Re-enabling adopts what is on
disk rather than reloading it, so the switch does not defeat itself. Full details in
[assets.md](../engine/assets.md#hot-reload).

## Scene Hierarchy panel

[`SceneHierarchyPanel`](../../GanymedEditor/source/Panels/SceneHierarchyPanel.h) — tree of root
entities, children drawn recursively (child lists are copied before iterating: re-parenting during
drag mutates the vector being walked). It is a `friend` of `Scene` and uses the immediate Entity
API — legal because panels run outside the system update.

- Select by click; click empty space to deselect.
- **Drag-drop re-parenting**: drag an entity onto another → `Scene::SetParent` (cycle-safe); onto
  empty space → unparent. Both record a `ReparentCommand`, but only after confirming the parent
  actually changed: `SetParent` silently no-ops on a cycle, and recording a move that did not
  happen would corrupt sibling order on undo.
- Right-click empty space → Create Empty Entity; right-click an entity → Delete.
- **Editor delete takes the whole subtree.** `Scene::DestroyEntity` keeps its orphan-the-children
  semantics as engine API, but no production editor deletes that way - Unity, Unreal and Godot all
  take the subtree - and the safety argument for orphaning ("you would lose the children")
  evaporates once the delete is one Ctrl+Z away.
- Deletion is deferred to after the hierarchy walk. Destroying a subtree mid-walk would invalidate
  the entt view the enclosing loop is iterating.
- ImGui IDs use the entt handle, not the UUID — old scene files could contain colliding UUIDs.

### Properties (drawn by the same panel)

Tag edit; **Add Component** popup (every component type not already present — camera, sprite,
lights, sky light, animator, script, audio source, audio listener, particle emitter, rigid body, colliders, one
`DrawAddComponentEntry<T>` line each); one collapsible section per component
(`DrawComponent<T>` helper with a remove-component menu).

**The section contract**: `uiFunction` is `bool(T&)` — "did any widget in this section edit the
component this frame", the OR of the returns the widgets already produce. The rule it establishes,
which every section lambda must follow: *an inspector lambda mutates the component only when a
widget actually reported an edit, and returns true when it does.*

That contract is what makes undo possible without value diffing. Diffing would need 16
`operator==`s and would still be wrong: the Transform section round-trips rotation through degrees
and back, which can change bits with no user input at all, so a diff scheme mints a phantom
command for merely selecting an entity. "The widget said so" is ground truth ImGui already
computes.

**The commit boundary.** A two-second drag writes the component on ~120 frames; undo wants one
command per gesture. `DrawComponent<T>` reads ImGui's active item before and after the section's
widgets: the frame an item inside the section becomes active starts a *pending* edit and that
frame's pre-copy is the before-value; the frame it stops being active commits, capturing the
after-value then. Widgets with no active phase — a drag-drop assignment landing on the section —
report their edit and commit in the same frame. A pending edit no frame of which reported an edit
is dropped, which is what makes a click-without-drag and an opened-then-closed combo free. If the
section stops being drawn mid-gesture, an end-of-frame flush commits what was recorded.

### The generic (reflected) inspector

Most sections no longer have a hand-written body. `EditorUI::DrawReflectedComponent(component)` in
[`EditorInspector.h`](../../GanymedEditor/source/EditorInspector.h) draws a component from what
`entt::meta` knows about it — field order, labels, ranges, drag speeds, the colour-vs-position widget
choice, enum entries and notes all come from
[`ComponentReflection.cpp`](../../GanymedEngine/source/GanymedE/Reflection/ComponentReflection.cpp).
Adding a field to a converted component is one `.data<>` line in the registration, not an edit here.

**It does not touch the undo protocol, and that is the point.** REFLECTION_ROADMAP R2 expected the
generic drawer to "own `ActiveId` across a drag exactly as the hand-written path does". It does not
have to: the commit boundary lives in `DrawComponent<T>`, which wraps the *section*, so a generic
body only has to keep the section contract above — mutate on a reported edit, return true when it
does. Every drawer does, so a converted section keeps one-command-per-gesture with **zero** change
to `EditorUndo`.

Dispatch is a `meta_type`-keyed map of `PropertyDrawer` function pointers, editor-side. It has to be
editor-side: `.custom<>` holds exactly one payload per meta object, so a second registration pass
from the editor would *overwrite* the engine's attributes rather than add to them. Defaults cover
`float`, `bool`, `int32`/`uint32`, `std::string`, `glm::vec2/3/4`, every registered enum, and
`AssetRef<T>`; `RegisterPropertyDrawer` overrides one for a type. A field whose type has no drawer is
skipped and named once in the log — a missing drawer should be loud, not invisible.

Trait handling: `Hidden` and `Custom` are skipped, `ReadOnly` draws disabled (and never reports an
edit), `Color` selects `ColorEdit` over the X/Y/Z row, `Radians` converts to degrees for display, and
`Flatten` draws a nested struct's fields as siblings — the shape `SceneSerializer` already forces for
a collider's `PhysicsMaterial`.

**Converted (15 of 20):** Sprite Renderer, Directional / Point / Spot Light, Transform, **Camera**,
**Sky Light**, Audio Listener, Audio Source, Prefab Instance, Particle Emitter, Rigid Body, and the
three colliders.

Two of those needed something the generic path alone cannot do, and both are handled *around* it
rather than inside it:

- **Transform** does `MarkChanged<TransformComponent>` after an edit. Editing a component directly
  is invisible to change tracking, so the cached world transform would never refresh. A side effect
  is not something reflection can express — but nothing stops the section from running one after
  `DrawReflectedComponent` returns true. Its degrees round-trip and reset values *are* expressed, as
  `Trait::Radians` and `Attr::Reset`.
- **Spot Light** clamps outer ≥ inner afterwards. A generic drawer sees one field at a time and
  cannot express a cross-field invariant, but the section can fix up the component the drawer just
  wrote. The clamp is the section's job; the widgets are not.

**Still hand-written, and none of it for want of effort** — each is blocked by something the
vocabulary deliberately cannot say:

| Section | Why |
|---|---|
| Static Mesh | The material-override list is sized by the **mesh asset**, not by component members |
| Animator | Clip names come from the mesh asset |
| Script | The field schema comes from Lua, not from C++ |

**Camera and Sky Light converted via a field filter.** Their blocker was field *visibility*
depending on another field's value — and unlike a clamp, that cannot be applied after the generic
drawer has run, because you cannot un-draw a field. `EditorUI::FieldFilter` is a predicate asked per
field *before* anything is submitted, so the section keeps its one cross-field rule and stops
hand-drawing every widget around it:

- **Camera** shows the perspective fields or the orthographic ones. `SceneCamera`'s own fields appear
  as rows of the section via the nested-struct fallback — a reflected struct with no drawer of its
  own is drawn inline. That is an *inspector* decision and says nothing about serialization, where
  the camera really is a nested map on disk; using `Trait::Flatten` to get the layout would have been
  a lie the first generic writer believed.
- **Sky Light** hides the two procedural colours when an environment is assigned, because they are
  unreachable fallbacks then. Showing an author a control that cannot affect anything is worse than
  not showing it.

The filter is deliberately a predicate in editor C++ rather than an attribute: "show this when that
other field equals X" is a small expression language, and the vocabulary is not the place for one.

**The Particle Emitter converted once ranges became a type.** Its five min/max pairs are now
`RangeF` fields (see [scene.md](../engine/scene.md#ranges)), so one drawer owns both halves and can
clamp in the direction the edit implies — which two independent float drawers never could. Its four
`CollapsingHeader` groups are `Attr::Section`, its curve and gradient editors are drawers keyed on
`FloatCurve` and `ColorGradient`, and its three asset slots are `AssetRef<T>`. What stayed
hand-written is the Play / Stop / Restart transport, which are **actions, not fields**.

Converting it also corrected two registrations that only a generic consumer could expose:
`Playing` and `Time` were `ReadOnly | NotSerialized`, which was right while the section drew its own
status line and nothing else — as generic fields they became two disabled rows repeating that line,
so they are `Runtime` (hidden) now.

**Audio Source and Prefab Instance were converted with a deliberate layout change.** Audio Source's
checkboxes no longer share lines and `Group` moved to the end, because the field order is now the
registration order and the vocabulary has no way to say "put these two together" — adding layout
knobs to it was rejected in R1. Prefab Instance's `Source` is `ReadOnly`, so the drawer renders it
disabled and reports no edit, which is what the hand-written section did by returning false.

`ReadOnly` on an asset slot also suppresses the drop target, which `BeginDisabled` alone would not:
a payload drop is not an item click, so without the explicit guard a read-only field would silently
accept one.

**Custom canvas widgets** live in [`EditorWidgets.cpp`](../../GanymedEditor/source/EditorWidgets.cpp)
(`CurveEditor`, `GradientEditor`, and `DrawVec3Control`, which moved there from the panel in R2 so
the reflected vec3 drawer produces the same widget the hand-written sections do) and are the first
house-drawn controls. They participate in that
protocol only if they own `ActiveId` for the whole gesture. The rule, and the pattern for any
future custom widget: **one `InvisibleButton` spans the canvas**. A held InvisibleButton owns
`ActiveId` until release (verified in the vendored ImGui 1.91.9b). Hit-testing against keys
decides what the drag moves; `ImDrawList` draws. Pure `ImDrawList` plus manual hit-test without
an item is one command per frame — the documented failure. Return true only on frames a key's
value actually changed, so a grab-and-release with no motion drops the pending edit. Mutation
goes through `FloatCurve` / `ColorGradient` `AddKey` / `RemoveKey` / `SetKey` only; the keys
vector is never written directly.

The vendored `ImCurveEdit` / `ImGradient` under `extern/ImGuizmo/src` stay uncompiled: they bring
an unverified ActiveId story, which is the one property this protocol cannot live without.

Notable behaviors:

- Transform edits go through `DrawVec3Control` (the X/Y/Z colored reset buttons, which returns
  `bool`) and call `MarkChanged<TransformComponent>` only when a row reported an edit. Rotation is
  written back **only** on an actual edit, for the round-trip reason above.
- Static mesh: shows the mesh asset (assign with `AcceptAssetDropRef<Mesh>()`), then **one
  row per renderer slot**. Each row shows either the assigned `.gmat` or `(default: <imported
  name>)`; dropping a `.gmat` on a row overrides that slot, and **Clear** removes the override.
  Both are ordinary component edits, so undo covers them with no new code. Assigning a different
  mesh clears the overrides — the new mesh has its own slot count and its own material identities,
  so keeping them would apply material 2 of one mesh to material 2 of an unrelated one (the
  `ScriptComponent::Fields` precedent).

  Below an assigned slot sits the **inline `.gmat` editor**: scalar and flag fields, texture maps
  assigned by dropping a texture asset, plus **Save** and **Revert**. Those edits are live on the
  shared `Ref`, so they show up immediately in every entity and every scene using that material —
  the header text says so, because a global edit that looks local is the worst version of this UI.
  They are also **not undoable**: undo covers scene edits only, and Save / Revert (Revert is
  `AssetManager::Reload`) are the asset-level transaction model instead. Only the slot assignment
  and Clear contribute to the section's `edited` return.
- Camera: projection type combo, per-type parameters, Primary / FixedAspectRatio.
- Static mesh: shows the mesh asset (handle + path) — assign with
  `AcceptAssetDropRef<Mesh>()`.
- Animator: a **combo over the clip names the entity's own mesh carries**, rather than a free text
  field. The clip reference is a name, so a text field would let you type one that resolves to
  nothing and get a silent bind pose. Plus Speed, Playing, Loop, and a **Time** slider bounded by
  the selected clip's duration. Time is the useful one in edit mode: `AnimationSystem` evaluates
  poses there but never advances the clock, so dragging Time is how you inspect a rig without
  entering play. **Dragging Time clears Playing** — in play mode the clock would otherwise
  overwrite the scrubbed value on the next update and the slider would look broken. Falls back to
  "No rigged mesh on this entity" when the mesh has no skeleton. Scripts drive the same component
  through `PlayAnimation` and friends — see [scripting.md](../engine/scripting.md).
- Script: shows the `.lua` asset (handle + path) with a Clear button — assign with
  `AcceptAssetDropHandle(Script)`. Below it, one row per property the
  script declares in its `Properties` table, typed (checkbox / drag float / text / vec3). The
  schema is read from the script itself in edit mode, so the rows appear without entering play.
  Only values you actually change are stored on the entity; **Reset** removes an override so the
  field tracks the script's default again. Assigning a different script clears the overrides —
  they are keyed by name against the old script's declarations. Removing the component in edit mode
  is safe: `LuaScriptSystem` drains its `FiniView` there and tears down any instance left from a
  previous play session. See [scripting.md](../engine/scripting.md).
- Sky light: environment asset (`AcceptAssetDropRef<Environment>()`), sky/ground colors, intensity,
  DrawSkybox.
- Audio source: the clip asset (handle + path) with a Clear button — assign with
  `AcceptAssetDropHandle(Audio)`, the helper's first client outside the three it was written for.
  Then a Group combo (Master/Music/SFX), Volume 0–1, Pitch 0.25–4, and Loop / Play On Start /
  Spatialize / Stream. The hint line under them says **"Clip, Spatialize and Stream apply when play
  starts"**, because those three are baked into the voice at creation while the other four are
  pushed every frame — without it, toggling Spatialize during play and hearing nothing reads as a
  bug. See [audio.md](../engine/audio.md).
- Audio listener: a Primary checkbox and a hint that the primary camera is the fallback when the
  component is absent.
- Particle emitter: grouped headers (Emission / Initial / Over Lifetime / Rendering). Over Lifetime
  is `EditorUI::CurveEditor` (size, Y range 0–2) and `GradientEditor` (color bar + `ColorEdit4` on
  the selected key). Double-click empty canvas adds a key; right-click deletes (refused on the last
  key). Min/Max pairs clamp so max ≥ min on edit. Billboard shows Texture + Blend; Mesh shows Mesh +
  Material and the opaque-material rule as a hover tooltip. **Play / Stop / Restart** sit at the top
  of the section and drive runtime `Playing` / pool / RNG — they are not authored, not serialized,
  and must not return `edited` (the same line as the inline `.gmat` editor's live preview). Stop
  freezes the pool; Play resumes the same RNG stream; Restart resets and reseeds. `DrawComponent`'s
  per-frame copy of the open section heap-copies the two keyframe vectors and the live pool.
- Colliders: dimensions, offset, friction/restitution.

Adding a component type means extending this panel's Add-Component popup and `DrawComponents` —
one of the two remaining hand-maintained per-component lists (the other is the serializer). Undo
needs nothing: it is driven by `ComponentList`, so a new component type joins it automatically.

## Multi-entity editing

**Ctrl+click** adds an entity to the selection or removes it; a plain click replaces the selection.
Every selected entity is highlighted in the hierarchy.

The design keeps a **primary** selection — the entity clicked last, `GetSelectedEntity()` — and adds
the full set beside it as `GetSelection()`, primary first. That is why multi-select cost six call
sites outside the panel instead of thirty: gizmos, the tag field and every prefab action still read
the primary and did not change at all.

| Behaviour | Rule |
|---|---|
| Which sections appear | Only components **every** selected entity has. Showing one that only some have would make an edit either silently skip entities or silently add the component to them |
| Fields that disagree | Tinted **amber**. Mixed wins over the prefab-override blue when both apply — "these entities disagree" is the more urgent fact, because the widget is showing one of several values rather than the value |
| Editing | The widgets drive the **primary**; the new value is copied to the rest *after* a widget reports an edit. Merely selecting several entities never flattens their differing values |
| Undo | **One entry per gesture, spanning the whole selection.** Verified: a 3-entity drag produces `UndoDepth == 1`, and one Ctrl+Z restores all three to their *individual* prior values |
| Delete | Deletes every selected entity |

Propagating after the fact, rather than driving N widgets, is what keeps every property drawer
single-entity and unaware that multi-edit exists — the same trick the prefab-override hook uses.

Two gaps, deliberate in v1:

- **Shift-range selection is not implemented.** It needs a flattened view of the tree that this panel
  draws recursively and does not keep; Ctrl covers the case multi-edit exists for.
- **The gizmo still moves the primary only.** Moving N entities is transform composition across a
  selection, which is a viewport feature rather than an inspector one.
- **A drop or popup edit with no active phase records undo for the primary only.** That path has no
  gesture to wait for, so the other entities' before-values are already gone by the time it runs;
  making it multi-entity means moving the pre-copy up into `DrawComponent`. Stated rather than
  hidden.

## Content Browser panel

[`ContentBrowserPanel`](../../GanymedEditor/source/Panels/ContentBrowserPanel.h) — a grid view of
`assets/`. Two classes of entry are hidden: anything whose name starts with `.` (the `.compiled/`
mesh cache today, `.compiled/` when the asset compiler lands) and `.meta`/`.meta.bad` sidecars.
Hiding the sidecars is not cosmetic — one per asset would double every row in the grid and offer
**Import** on a file that is not an asset. They are the `AssetManager`'s to write, never a human's
(see [assets.md](../engine/assets.md#the-meta-sidecar)):

- Directory/file icons, tinted by asset type (mesh blue, environment orange, scene green, texture
  pink, material purple, script yellow, audio cyan). Double-click enters directories; the `<-` button goes up but can never
  escape the asset root (path-normalized check).
- Every item is a drag source (`CONTENT_BROWSER_ITEM`, relative path payload) — the viewport and
  the properties panel accept the relevant types.
- Right-click on an importable file (mesh/environment/texture/material/script/audio/prefab) →
  **Import**, registering it with the `AssetManager` (idempotent). In practice the scan at `Init`
  has already done this for every file under `assets/`; the menu item is for a file that appeared
  since. `ImportAsset` writes the `.meta` sidecar itself, so there is nothing to flush — see
  [assets.md](../engine/assets.md#the-meta-sidecar).
- Right-click on an already-registered file → **Reload**, evicting it from the manager's cache so the
  next fetch re-reads it from disk. For a mesh and a texture this also drops the compiled artifact,
  i.e. a full reimport. Edits land in the viewport on the next frame because every `AssetRef`
  re-resolves after an eviction — see [assets.md](../engine/assets.md#reload) for the invariant that
  makes eviction safe mid-frame.
- On a file whose type has a compiler (meshes, textures) there is also **Reimport**: `Reload` plus
  deleting the compiled artifact outright, for the case the epoch record cannot see — an import
  setting edited by hand, or simple doubt about what is in the cache. It **blocks**, and a large
  texture is seconds; the menu item's tooltip says so rather than letting the editor look hung.
  Making it non-blocking is asset Phase 5's job.

## Typed drag-drop

[`AssetDragDrop.h`](../../GanymedEditor/source/AssetDragDrop.h) —
`namespace GanymedE::EditorUI`. Every drop target goes through it, so
`AssetTypeFromExtension` is the single source of truth for what a target accepts on the editor side
too (it previously wasn't used here at all: each site hand-rolled
`extension()` → `::tolower` → string compare).

| Call | Returns |
|---|---|
| `AcceptAssetDrop(type)` | `optional<path>` — the dropped path relative to `assets/`, iff its type matches |
| `AcceptAssetDrop({types...})` | `AssetDrop { Type, Path }`, falsy when nothing matched — for targets accepting several types. The viewport uses it for Scene / StaticMesh / Prefab, where the list form is **mandatory**: ImGui clears the payload as soon as one target delivers it, so three single-type calls would let only the first ever fire |
| `AcceptAssetDropHandle(type)` | `ImportAsset` (idempotent, and persists identity itself) on match, else `InvalidAssetHandle` |
| `AcceptAssetDropRef<T>()` | The same, typed: an `AssetRef<T>`, unset when nothing matching was dropped |

Call it immediately after the widget that should accept the drop; it wraps
`BeginDragDropTarget` / `AcceptDragDropPayload("CONTENT_BROWSER_ITEM")` / `EndDragDropTarget`.
A mismatched drop is silently ignored.

**Prefer `AcceptAssetDropRef<T>()` for a component slot.** The accepted `AssetType` comes from
`AssetTypeOf<T>`, so the filter is derived from the field rather than passed beside it — a slot can
no longer declare `AssetRef<Environment>` and filter on `AssetType::Texture`, which was one typo
away while every call site wrote both by hand. Assigning the result to the wrong field is a compile
error rather than a drop that silently never fires:

```
error C2440: cannot convert from 'AssetRef<Material>' to 'AssetRef<Mesh>'
```

The particle emitter's three slots are drawn by one `assetSlot(label, slot, hint)` lambda generic
over `decltype(slot)::AssetT`, which is what removed the `AssetType` argument that used to sit next
to an untyped `AssetHandle&`.

**A multi-type target must use the `initializer_list` overload, not two calls in a row.**
`ImGui::EndDragDropTarget` calls `ClearDragDrop` as soon as a payload is delivered, and
`BeginDragDropTarget` early-returns when no drag is active — so a second call after the same widget
sees nothing on the frame the drop actually lands. Both calls would also share the one
`CONTENT_BROWSER_ITEM` payload type (the type filtering happens *after* accepting, on the extension),
so the first call always wins the delivery and the second type would never fire.

## Adding an editor feature — where things hook

| Want to… | Touch |
|---|---|
| New panel | Create under `Panels/`, own it in `EditorLayer`, call `OnImGuiRender`, dock it in the DockBuilder block |
| New component UI | `SceneHierarchyPanel::DrawComponents` (+ Add-Component popup) |
| Custom canvas widget | `EditorWidgets.cpp`; one `InvisibleButton` spanning the canvas so `ActiveId` holds for the drag; return true only on a real value change |
| New asset type in the browser | `AssetTypeFromExtension`, icon tint map, `IsImportableAsset`, then `EditorUI::AcceptAssetDrop(<type>)` at the consumer |
| New shortcut | `EditorLayer::HandleShortcuts` (editor-global) or `OnKeyPressed` (viewport-gated, like the gizmo keys) |
| New undoable operation | An `EditorCommand` subclass in `EditorUndo.h`, pushed where the operation happens; `CompositeCommand` when several steps must undo as one |
| New scene-wide toggle | Prefer a singleton in `SceneSingletons.h`, edit it from the Stats panel like `PhysicsSettings` |

Remember the editor-code rules: panels may use the immediate Entity API (they run outside the
update loop — the asserts in `Entity` enforce this), any direct write to a tracked component must
be followed by `Scene::MarkChanged<T>`, an inspector lambda mutates its component only on a real
widget edit and returns true when it does, and anything that changes the scene should push an
`EditorCommand`.

# GanymedEditor

The editor application (`GanymedEditor/source/`). It is a thin client of the engine: one
`Application` subclass ([`GanymedEditorApp.cpp`](../../GanymedEditor/source/GanymedEditorApp.cpp))
pushing a single [`EditorLayer`](../../GanymedEditor/source/EditorLayer.h), plus the hierarchy,
content browser, and Map panels.
Run it with `GanymedEditor/` as the working directory — the editor's *own* assets (Inter, Lucide,
the checkerboard, its HUD document) resolve relative to CWD. A scene path may be passed
positionally, and `--renderer=<backend>` selects the graphics backend (see
[rendering.md](../engine/rendering.md#backend-selection)). Options and the scene path may appear in
either order.

`--project=<path>` opens a project other than `GanymedEditor/assets/`. Only the **project** moves:
the editor's own assets above still come from the working directory, because they ship with the
editor rather than with the content — see [the project root](../engine/assets.md#the-project-root).
The path is logged at boot, and a path that is not a directory is opened anyway with a warning,
because an empty content browser looks like data loss rather than a typo. Without the switch the
root is `assets`, exactly as before.

The **Content Browser is rooted at the project**, and re-homes itself in `RefreshCaches` whenever
the root differs from the one it holds. It has to: the panel is a by-value member of `EditorLayer`,
so its constructor runs before `OnAttach` sets the root, and a root captured there is always the
default — see [the project root](../engine/assets.md#the-project-root) for the two forms that bug
has taken.

A **new scene's default environment** (`environments/studio_small_08_1k.hdr`) is imported only when
the project actually has that file. It is a project-relative path that exists because there was
once only one project; without the check, every other project logged a failed HDR load on every new
scene. An absent environment is the procedural fallback, which is what a `SkyLight` with no handle
already means.

## Layout

A dockable ImGui workspace. Host chrome is stacked in the dockspace window: a **fixed
40 px `ChromeBg` title bar** (undecorated OS window; see below), a **fixed 41 px `SurfaceBg`
toolbar child**, `DockSpace(size.y = -StatusBarHeight)`, then a **fixed 41 px `ChromeBg`
status bar child**. Those four items are packed with `ItemSpacing (0,0)` — theme spacing is
4 px, and between four host strips that overflows the window by a few pixels at every size
and shows a host scrollbar. The host itself is `NoScrollbar`. None of those strips is a
docked window: they cannot be resized, undocked, or given a tab. On Wayland the title bar is
omitted and the ImGui menu bar stays, because an undecorated window cannot be moved. On first
run, after **View → Reset Layout**, or when the dock-layout version in `imgui.ini` mismatches
(`[GanymedEditor][Dock] Version`, currently 3), `EditorLayer` builds a default DockBuilder
tree: Scene Hierarchy left, Properties below it, Viewport center, Stats and Map tabbed on the
right, Content
Browser bottom. After that, panel layout persists in `GanymedEditor/imgui.ini`. Later chrome
changes that alter the default tree bump that version so an existing ini does not keep a
stale split. The title bar and status bar sit outside the dock tree, so they did not need a
version bump.

## Look and feel

Editor chrome is Inter + Lucide, rasterized by FreeType. Colour, density and geometry come from
`EditorTheme` / `ApplyTheme` — **not** from the engine. `ImGuiLayer` only calls `StyleColorsDark()`
and ships the embedded font, so the runtime does not depend on editor assets and the engine does not
hold a brand palette.

`EditorFonts::Load` and `ApplyTheme(MakeDarkTheme())` run from `EditorLayer::OnAttach`, after
`ImGuiLayer` has created the context. `Load` `Clear()`s the atlas, so `io.FontDefault` is
reassigned in the same call; `ImFont*` values held across a `Clear()` dangle.
`ImGuiRendererBgfx::NewFrame` already rebuilds the bgfx font texture when `!io.Fonts->IsBuilt()`,
which is why this needs no engine API. **View → Theme** switches Dark (default, the shipping
neutral ramp) and Light (the same roles inverted onto a light ramp). Both keep the lilac accent.
The choice is a process-lifetime static, not written to `imgui.ini`.

The menu lives inside the title-bar child, which has `ChildBg` pushed. `ApplyTheme` therefore
patches ImGui's `ColorStack` backups after writing `ImGuiStyle`, so `PopStyleColor` cannot
resurrect the previous ramp. Without that, `WindowBg` (Properties, Stats) updates and
`ChildBg` does not — the outliner tree and the Content Browser folder/file panes are
`BeginChild` with no local `ChildBg`, so they stayed on the old theme.

### Type

| Use                                     | Face          | Size  |
| --------------------------------------- | ------------- | ----- |
| Body (`io.FontDefault`)                 | Inter Regular | 18 px |
| Panel/section headers, XYZ reset labels | Inter Medium  | 18 px |
| Status bar, hints, column headers       | Inter Regular | 16 px |

Rasterizer flags: `ImGuiFreeTypeBuilderFlags_LightHinting` on the atlas. Do **not** use
`io.FontGlobalScale` — it scales an already-rasterized atlas and smears glyphs. Each size is its
own font.

Icons are Lucide, merged into every editor face (`MergeMode`, 16 px, `GlyphMinAdvanceX = 18` so
toolbar cells align). An `ICON_LC_*` string is just text: it scales with DPI, tints with
`ImGuiCol_Text`, and needs no texture. Codepoints live in `EditorIcons.h`, which wraps the
vendored `IconsLucide.h` (IconFontCppHeaders). Swap that one include to change icon sets. The
TTF is `assets/fonts/lucide/lucide.ttf`; `VERSION` next to it is the `lucide-static` package the
font was taken from.

RmlUi game UI is a separate atlas and still uses Montserrat (`UIEngine` loads Regular/Bold/Italic
from `assets/fonts/montserrat/`). Those three faces stay; they are the Play-mode HUD, not editor
chrome.

### Colour tokens

`EditorTheme` is a flat `ImU32` struct in `GanymedEditor/source/EditorTheme.h`. One `ApplyTheme`
call writes the full `ImGuiStyle`. Two presets share the lilac accent and differ in the
neutral ramp (and in the few semantic colours that have to sit _on_ that ramp as text):

| Token                          | Dark                  | Light     | Role                                                                                                                                               |
| ------------------------------ | --------------------- | --------- | -------------------------------------------------------------------------------------------------------------------------------------------------- |
| `ChromeBg` / `Border`          | `#1A1A1A`             | `#DEDEDE` | Title bar, tab strips, inspector headers, 1 px gutters. Same value as each other — panels separate with chrome-coloured gaps, not lighter outlines |
| `SurfaceSunken`                | `#272727`             | `#EBEBEB` | Recessed fills: inputs (`FrameBg`), column headers                                                                                                 |
| `SurfaceBg`                    | `#313131`             | `#F5F5F5` | Panel content, active tab, window background                                                                                                       |
| `GrabBg`                       | `#4D4D4D`             | `#C5C5C5` | Scrollbar grab, slider track hover                                                                                                                 |
| `TextPrimary`                  | `#CCCCCC`             | `#1A1A1A` | Body                                                                                                                                               |
| `TextDim`                      | `#878787`             | `#5C5C5C` | Secondary / hint                                                                                                                                   |
| `TextDisabled`                 | `#717171`             | `#9E9E9E` | Disabled, unselected tab labels                                                                                                                    |
| `Accent`                       | `#B182ED`             | `#B182ED` | Selection fill, checkmarks, grabs                                                                                                                  |
| `AccentHover` / `AccentActive` | `#C39BFF` / `#9152E0` | same      | Hover / pressed                                                                                                                                    |
| `AccentText`                   | `#B07BF4`             | `#7B43C2` | Accent-coloured text on a panel, no fill. Light uses the icon violet; the lilac fails on `#F5F5F5`                                                 |
| `TextOnAccent`                 | `#1A1A1A`             | `#1A1A1A` | Glyphs on an accent fill (outliner primary selection)                                                                                              |
| `Link`                         | `#589FFD`             | `#1565C0` | Entity-name links in the outliner. **Not** the accent                                                                                              |
| `FieldMixed`                   | `#FFC759`             | `#B45309` | Multi-select fields that disagree                                                                                                                  |
| `FieldOverride`                | `#73B8FF`             | `#185ABC` | Prefab instance fields that differ from the prototype                                                                                              |
| `Warning` / `Error`            | `#E6B450` / `#E5534B` | same      | Status                                                                                                                                             |
| `AxisX/Y/Z`                    | existing RGB          | same      | `DrawVec3Control` reset buttons                                                                                                                    |
| `AssetTint[AssetType]`         | per-type              | same      | Content Browser icon tints; directories stay white                                                                                                 |

`FieldOverride` and `Link` are both blue on purpose. They never share a panel — the outliner has
no property rows and the inspector has no entity links — so collapsing them into one colour
would only merge two independent signals later.

The lilac fill (`#B182ED`) with dark glyphs is the shipping accent on both presets: a saturated
copy of the app icon (`#7B43C2`) is too dark as a selection fill, and keeping a dark fill with
light glyphs would invert the treatment (selected row darker than the Dark panel, which reads as
collapsed/disabled). On Light, `#7B43C2` is the `AccentText` stop instead.

`NavWindowingDimBg` / `ModalWindowDimBg` are a fixed `#1A1A1A` veil, not `ChromeBg`. Light's
chrome is pale; using it as the dim overlay would wash the editor out.

### Geometry and the two inversions

`ApplyTheme` sets rounding to 0 everywhere, `FramePadding` to `(6, 3)` (18 px text + 6 = the
measured 24 px row), `WindowMenuButtonPosition = ImGuiDir_None` (kills the dock-tab `▼`), tab
overlines to 0 (active tab is a background change only), and `DockingSeparatorSize = 1`.
Host chrome heights sit on the theme struct, not ImGuiStyle: `TitleBarHeight` 40,
`ToolbarHeight` 41, `StatusBarHeight` 41.

Two mappings invert ImGui's defaults, and they are not bugs:

| ImGui colour               | Token                        | Why                                                                                                                                                                                          |
| -------------------------- | ---------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `FrameBg`                  | `SurfaceSunken`              | Inputs are **recessed** — darker than the window, not lighter. Holds on Light (`#EBEBEB` on `#F5F5F5`) as well as Dark (`#272727` on `#313131`)                                              |
| `Header` / `HeaderHovered` | `ChromeBg` / `SurfaceSunken` | Inspector section headers (`Attr::Section` CollapsingHeaders) are chrome on the panel (`#1A1A1A` on `#313131` in Dark, `#DEDEDE` on `#F5F5F5` in Light). ImGui's default is a _lighter_ fill |

`HeaderActive` is `Accent`, but that colour is only used while the mouse is **held**. An idle selected `TreeNode` / `Selectable` uses `Header`. Do not raise theme `Header` to Accent — inspector sections would go lilac. Selected rows that paint `TextOnAccent` must push `Header` / `HeaderHovered` / `HeaderActive` locally (outliner, Content Browser folder tree and list). Grid cells draw the fill themselves.

Do not "fix" FrameBg or Header back toward ImGui defaults.

### Panel furniture

Chrome helpers in `EditorWidgets.h` — not property editors. `BeginPanel` / `EndPanel` wrap
`Begin` / `End` with `WindowPadding (0,0)` so toolbars and column headers reach the window
edge; `BeginPanelBody` is a child with `AlwaysUseWindowPadding` so tree/inspector content is
not flush. `PanelToolbarRow` is a 44 px `SurfaceBg` strip (the sampled per-panel toolbar; not
`Theme().ToolbarHeight`, which is the main 41 px host strip). `SearchField` is one recessed
`SurfaceSunken` bar (`InputTextWithHint` plus search/clear glyphs) — while it is focused,
`HandleShortcuts` already bails on `WantTextInput`, so Ctrl+Z is ImGui's text undo.
`ColumnHeaderRow` is a 26 px `SurfaceSunken` strip in the 16 px face. `IconButton` /
`ToolbarSeparator` / `OverflowMenuButton` / `RowActionIcons` / `StatusBarItem` are the rest.
Do not hand-roll these, and do not call `OverflowMenuButton` unless a real popup follows.

The outliner, Properties, Content Browser, Map, and Viewport are wrapped (`BeginPanel`). The host
title bar is `EditorTitleBar.cpp`, not a furniture helper — it has to talk to `Window` hit-testing.

### Title bar

The editor sets `ApplicationSpecification::CustomTitleBar`. Windows then runs undecorated with
a Win32 subclass so drag, snap, edge resize and maximize-without-covering-the-taskbar stay
native; Linux (X11) and macOS drag with `glfwSetWindowPos`. Wayland keeps OS decorations and
this strip is not drawn.

The strip is 40 px `ChromeBg`: app icon (`resources/icon.png`), a **Menu** button whose popup
holds File / Edit / View (the same items the old `BeginMenuBar` had), one document tab (scene
filename + dirty `*`, `SurfaceBg` fill — Ganymed has one open scene, so a strip of fake tabs
would be a lie), then min / restore-or-max / close. Close hovers `Error`. The title-bar child
zeros `WindowPadding` / `ItemSpacing` so chrome reaches the edges; the Menu popup pushes the
pre-zero values back on before `BeginPopup`, or File/Edit/View inherit padding 0. Interactive
rects are excluded from `HTCAPTION` so those clicks reach ImGui instead of moving the window.
The engine side is documented in [platform.md](../engine/platform.md#custom-title-bar).

## EditorLayer

Owns the `SceneRenderer` (HDR target + post stack), the active/editor `Scene` pair, the
`EditorCamera`, panels, and the play/edit state machine.

### Per-frame (`OnUpdate`)

1. Resize the scene renderer / editor camera / scene cameras when the viewport panel size changed.
2. `SceneRenderer::BeginFrame` (bind + clear HDR target, entity IDs to −1).
3. Update the scene. In Edit: surface-raycast and write the placement preview transform **before**
   `OnUpdateEditor`, so `TransformSystem` this frame sees the hover pose and the preview renders
   where the cursor is, not where it was last frame. Then `OnUpdateEditor(ts, editorCamera)` (and
   `RenderContext::PreviewCamera` from the viewport camera combo). In Play:
   `OnUpdateRuntime(ts, &editorCamera)` (the editor camera is the fallback when the scene
   has no primary `CameraComponent`; the physics-debug toggles **and `ShowColliderGizmos = true`**
   are pushed into the scene's `PhysicsSettings` each frame). The gizmo flag is engine-default
   **false** so a non-editor front-end draws no collider wireframes — the editor opts in, and it has
   to do so every frame because `Scene::Copy` does not carry singletons onto the play-mode scene.
4. **Hover picking**: mouse position → viewport-local coordinates (Y flipped only when
   `bgfx::getCaps()->originBottomLeft` — render-target origin is backend-dependent), then
   `RequestEntityID` + `PollEntityID`. Picking is asynchronous under bgfx (~3 frames latency),
   invisible for hover highlighting. The result feeds `m_HoveredEntity` (shown in Stats,
   click-to-select).
5. **Surface ray** (Edit only, before `OnUpdateEditor`): the same pointer, but clip-space Y-up with
   **no** render-target origin flip — this is projection algebra, not a texture sample.
   `Math::ScreenPointToRay` through the active viewport camera's `inverse(viewProjection)`, then
   `RaycastScene` against resident static-mesh triangles. Result is `m_SurfaceHit` (world point,
   normal facing the ray, entity or work-plane fallback), shown on the Stats panel as `Surface:`
   plus the per-ray ms. GPU picking stays the click-select path; placement reads this. Hidden
   outliner entities, the placement preview (`RaycastFilter::Exclude`), and anything with an
   `AnimatorComponent` are skipped; a miss against a ray that still hits `y = MapSnapSettings::GridHeight`
   is `FromWorkPlane`, a ray into empty sky is no hit at all.
6. `SceneRenderer::EndFrame` — bloom → tonemap → FXAA → composite.

### Viewport

- `BeginPanel("Viewport")` so the header row reaches the window edges. A 44 px
  `PanelToolbarRow` sits above the image.
- **Header, left:** camera combo (Editor Camera, plus every `CameraComponent` in the scene).
  Selecting a scene camera writes `RenderContext::PreviewCamera`; `RenderSystem::OnUpdateEditor`
  then `BeginScene`s with that camera's projection and world transform. The editor camera is
  not orbited while looking through a scene camera — switch back to move it. The combo is
  disabled in Play and shows the primary camera's tag (the runtime path already uses that
  camera, with the editor camera as fallback).
- **Header, centre:** `Free Aspect: WxH` from `m_ViewportSize` (the _image_ size, not the
  panel — the 44 px header is excluded so the render target matches what picking and RmlUi
  see). There is no aspect lock, so this is a readout, not a dropdown.
- **Header, right:** magnet (opens `MapSnapSettings`; accent-filled while snapping is enabled) ·
  Visualizers popup (the Jolt debug-draw toggles that used to live in Stats — still Play-only,
  they read Jolt body state) · Local / World combo wired to `ImGuizmo::Manipulate`'s mode.
  Previously LOCAL was hard-coded. The magnet is the same snap struct placement reads.
- **Omitted, no backing feature:** Quality tiers, selection filters, billboard-gizmo Icons
  toggle. Lit / Unlit / Wireframe: Unlit needs shader variants that do not exist; a global
  wireframe fill is not "one bgfx flag" — every `SubmitMesh` packs its own state from the
  material. A dropdown whose only working item is Lit is dead furniture.
- Shows the composite target via `ImGui::Image`; UVs flip vertically per
  `originBottomLeft` (a render target's orientation follows the backend — hard-coding either way
  is wrong on half of them).
- **`m_ViewportHovered` is the image**, not the window. A click on the camera combo must not
  also click-select whatever the pick buffer last saw. `BlockEvents` still uses
  focused-or-hovered, so Q/W/E/R keep working while the viewport window is focused.
- **Drag-drop from the Content Browser** via `EditorUI::AcceptAssetDrop` (see
  [below](#typed-drag-drop)): a `Scene` drop opens the scene; a `StaticMesh` drop (edit mode only)
  instantiates it via `MeshImporter::Instantiate` and selects it.
- **Gizmos** (edit mode, with a selection): ImGuizmo manipulates the entity's **world** transform
  (`Scene::GetWorldSpaceTransform`, so parented entities gizmo correctly), converts back to local
  through the parent's inverse world matrix, decomposes (`Math::DecomposeTransform`), applies
  rotation as a delta to avoid gimbal jumps — and then calls
  **`Scene::MarkChanged<TransformComponent>`**, because a direct component write is invisible to
  change tracking and the world-transform cache would go stale (the entity would keep rendering at
  its pre-drag position). Snapping is **on by default**; **Ctrl inverts it**. Steps come from
  `MapSnapSettings` (`Translate` 0.5 m, `Rotate` 15°, `Scale` 0.1) rather than the old hard-coded
  0.5 / 45°. A 45° preset sits next to the rotate field. Mode is `m_GizmoType`
  (select / translate / rotate / scale): Q/W/E/R still go through `OnKeyPressed` (viewport-gated
  so a name containing W does not switch tools), and the toolbar icon cluster writes the same
  int. The active tool is accent-filled. View/projection follow the camera dropdown;
  `SetOrthographic` follows a scene camera's projection type. The gizmo is hidden while placement
  is active so it cannot fight the preview.

  **It drives the whole selection.** The gizmo manipulates the primary, and the world-space change
  it made — `after * inverse(before)` — is applied to every other selected entity through
  `ApplyWorldDelta`. Composing in world space rather than adding a local offset is what makes a
  group rotate and scale _about the primary_ instead of each object spinning about its own origin;
  verified by construction, a 90° yaw moves an entity at (2,0,0) to (0,0,-2). An entity whose
  ancestor is also selected is skipped, or it would take the delta twice — once from its parent's
  transform and once from its own. One drag is one undo entry: the falling edge folds every moved
  entity into a single `CompositeCommand`, the same rule the inspector's multi-edit follows.

- **Transform readout** (bottom-left of the image, `ImDrawList`, no layout): `X`/`Y`/`Z` of the
  primary selection in `AxisX/Y/Z`, values in `TextPrimary`. Local translation, or world
  translation when the gizmo is in World space, so the numbers match the handles. Nothing
  selected → nothing drawn. Entity/draw/FPS counters live on the status bar rather than being
  duplicated here.

### Surface raycast

[`EditorPicking.cpp`](../../GanymedEditor/source/EditorPicking.cpp) is the edit-mode primitive
that GPU picking cannot be: a **synchronous** world point, normal and entity, this frame, with
no physics world.

Unity and Unreal trace an editor physics world for this. Ganymed has no edit-mode bodies —
`PhysicsScene::CreateBodies` runs from `Start()` — and building a shadow body set just to place
objects would be a second source of truth, for a feature whose point is that authored colliders
are untrustworthy. So the ray walks render geometry: `Mesh` keeps its CPU vertices after upload.

`RaycastScene(scene, ray, filter)`:

1. Broad phase: `(WorldTransformComponent, StaticMeshComponent)`, excluding `AnimatorComponent`,
   the filter's `Exclude` UUID (the placement preview), and
   `SceneHierarchyPanel::HiddenEntities()`. World AABB vs the ray, collect `tMin`.
2. Sort near→far; stop when the next candidate's `tMin` exceeds the best confirmed hit.
3. Narrow phase: transform the ray by `inverse(world * Submesh::LocalTransform)` (cheaper than
   transforming triangles; leave the direction un-normalised so `t` stays in world metres), skip
   a submesh whose local AABB misses, Möller–Trumbore over its index range.
4. Normal is `cross(e1, e2)` through `transpose(inverse(meshLocal))`, then flipped to oppose the
   ray. Flip by ray direction, not winding — a negative scale inverts winding.
5. Miss: intersect the horizontal plane at `RaycastFilter::GridHeight` (`FromWorkPlane = true`),
   which placement feeds from `MapSnapSettings::GridHeight`.
   A ray pointing at empty sky (`t < 0` on that plane) returns no hit, so placement can refuse
   rather than put something 400 m behind the camera.
6. A mesh that is not resident yet is skipped (async load); a miss is not cached. A single mesh
   over `TriangleBudget` (250 000) falls back to the AABB hit and warns once — there is no BVH.

Skinned meshes with an animator are excluded: CPU vertices are the bind pose. `ScreenPointToRay`
clamps length to the unprojected far plane, which is the far clip along that ray.

The Stats `Surface:` line is the live probe. It does not replace GPU hover for click-select.

### Controls

| Input                                   | Action                                                                                                                                       |
| --------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------- |
| Alt+LMB drag / MMB drag / scroll        | Orbit / pan / zoom the editor camera                                                                                                         |
| LMB in viewport                         | Select hovered entity (ignored over the gizmo, with Alt held, or while placing)                                                              |
| LMB while placing                       | Commit the preview (`AddEntitiesCommand` after the transform is final). Shift+LMB chains; Alt+LMB places unsnapped                           |
| Esc / RMB while placing                 | Cancel and destroy the preview (no undo entry)                                                                                               |
| `[` / `]` while placing                 | Yaw by `MapSnapSettings::Rotate`                                                                                                             |
| Q / W / E / R                           | Gizmo: select / translate / rotate / scale (viewport-gated; ignored while using the gizmo or RMB-flying). Toolbar icons write the same state |
| Local / World combo (viewport header)   | ImGuizmo LOCAL (default) / WORLD                                                                                                             |
| Ctrl+Z / Ctrl+Y / Ctrl+Shift+Z          | Undo / redo (Edit state only)                                                                                                                |
| Ctrl+D / Delete                         | Duplicate / delete the selected entity, subtree included (Edit state only). While placing, Delete cancels instead                            |
| Ctrl+N / Ctrl+O / Ctrl+S / Ctrl+Shift+S | New / Open / Save / Save-As scene. New, Open and Play cancel an uncommitted preview                                                          |
| Ctrl (held while dragging gizmo)        | **Inverts** snap. Snapping is on by default; steps are `MapSnapSettings`                                                                     |
| Ctrl+U                                  | Toggle the RmlUi game-UI Debugger (also View → Game UI Debugger; Debug builds only)                                                          |
| View → Reset Layout                     | Rebuild the default dock tree. Existing `imgui.ini` otherwise hides layout work                                                              |
| View → Theme                            | Dark (default) or Light — same lilac accent, inverted chrome. Not persisted                                                                  |
| F1                                      | bgfx stats overlay                                                                                                                           |

**Two shortcut layers, on purpose.** Q/W/E/R and Ctrl+U go through the engine event path
(`EditorLayer::OnKeyPressed`), which `ImGuiLayer::BlockEvents` gates on viewport focus/hover.
Everything else - undo, redo, duplicate, delete, placement yaw / Esc, and the file shortcuts - is polled inside the
ImGui frame by `EditorLayer::HandleShortcuts` using `ImGui::IsKeyChordPressed`, so it fires
wherever the mouse is.

The file shortcuts used to live on the engine path too, and dead-zoned over every panel: Ctrl+Z
above the Properties panel simply did nothing. Relaxing `BlockEvents` was the alternative and is
worse - it would leak _every_ key into the engine path while typing in a panel, firing camera
keys and gizmo-mode switches mid-rename. A command layer above widget focus is the production
norm, and polling ImGui inside the ImGui frame is that layer at this scale. Q/W/E/R stay
viewport-gated deliberately for the same reason.

`HandleShortcuts` returns early on `ImGui::GetIO().WantTextInput`: while a text field is focused,
Ctrl+Z is ImGui's own text undo, which is what every editor does.

## Undo / redo

[`EditorUndo.h`](../../GanymedEditor/source/EditorUndo.h) - `EditorCommand`, `EditorUndoStack`,
and the command types. `EditorLayer` owns the stack; the hierarchy panel records into it.

**Scope: scene edits only.** Inspector property edits, add/remove component, create / delete /
duplicate entity, re-parenting, gizmo drags, map placement commits and duplicate-along-axis are
undoable. Asset-level edits are deliberately
not - a scene-local stack would lie about their scope, since undoing one would silently change
every scene using that asset. Unity draws the same line for most asset properties; Unreal's
transaction system does cover assets, and Ganymed diverges toward Unity's model because it has no
per-asset dirty/transaction infrastructure.

The stack lives editor-side rather than in the engine: the runtime has no consumer for undo. This
mirrors how `EditorCamera` lives engine-side while the _editing model_ does not.

| Piece                                                  | Notes                                                                                                                                                                           |
| ------------------------------------------------------ | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `EditorUndoStack`                                      | Linear, capped at 100, `Push` clears the redo stack. `MarkSaved`/`IsDirtySinceSave` track dirtiness by stack _position_, so undoing back to the saved state correctly clears it |
| `ComponentEditCommand<T>`                              | Before/after values. The after-value is filled in at the commit boundary, not at construction                                                                                   |
| `AddComponentCommand<T>` / `RemoveComponentCommand<T>` | Remove stores the whole value, so undo is a re-add rather than a default-construct                                                                                              |
| `AddEntitiesCommand` / `DeleteEntitiesCommand`         | One subtree-snapshot mechanism, differing only in which way `Undo` runs. Create, duplicate, prefab instantiate and map placement are all built on it |
| `CompositeCommand`                                     | Several commands that undo as one. Gizmo group-drags and duplicate-along-axis are both this: N entities, one Ctrl+Z                                                              |
| `ReparentCommand`                                      | Records the **old sibling index** explicitly - `Scene::SetParent` push_backs, and since the canonical save order is a hierarchy DFS, sibling order is content                   |

**Every command keys entities by UUID**, resolved through `Scene::FindEntityByUUID`. `entt::entity`
handles are not validity-checked by `Entity::operator bool` and do not survive a destroy/recreate
cycle, so a raw handle in an undo record is a dangling reference waiting for a redo. A command
whose UUID no longer resolves warns and does nothing.

**Snapshots are in-memory component tuples, not YAML.** `EntitySnapshot` holds a
`tuple<optional<Ts>...>` over `ComponentList`, filled through `ForEachType`. That is lossless -
it round-trips `AnimatorComponent::Time`, which the serializer deliberately drops, and it
preserves the _absence_ of a `ScriptComponent` field override, which means something different
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

| Operation            | Where                                                                  | Undoable                                                                    |
| -------------------- | ---------------------------------------------------------------------- | --------------------------------------------------------------------------- |
| **Create Prefab…**   | Entity context menu                                                    | The file write is not; linking the source entity is (it is a component add) |
| **Instantiate**      | Drop a `.gprefab` on the viewport, or the hierarchy's blank-space menu | Yes — a subtree add                                                         |
| **Apply to Prefab…** | Instance context menu, and a button in the inspector                   | **No** — it is an asset write                                               |
| **Revert Instance**  | Same two places                                                        | Yes — one composite command                                                 |

**Create** links the source entity to the file it just wrote, so the thing you made a prefab _from_
becomes an instance of it. That is the Unity behaviour authors expect. The path must be inside
`assets/`; a prefab outside the asset root gets no handle and no sidecar, so nothing could reference
it.

**Apply** is the milestone's one silently destructive click — it overwrites an asset, and undo
covers scene edits only — so it is the one operation behind a confirmation modal. The modal names
the file and says the thing an author would otherwise have to discover: **other instances already in
the scene do not update**. There is no propagation in v1.

**Revert** deletes everything below the root and rebuilds it from the file, keeping the root entity
itself: its UUID, so references to it survive, and its transform, because placement belongs to the
instance. It is a scene edit, so it _is_ undoable — as a single `CompositeCommand` holding the
delete of the old subtree and the add of the new one, which is why one Ctrl+Z takes you back to the
pre-revert state rather than halfway. If instantiation fails, the captured subtree is restored
rather than leaving a hole.

Structural freedom inside an instance is **allowed and unmarked**: add, remove and re-parent
children at will. "Create from selection" means the _primary_ selection's subtree — prefab actions
are single-entity even though the selection no longer is.

### Per-property overrides

A field of a prefab instance that differs from the prefab is **tinted blue in the inspector**, and
right-clicking it offers **Revert to Prefab** and **Apply to Prefab** — the two directions of the
same diff. A component with any differing field gets a `*` on its section header.

The two are opposites in consequence as well as direction, which is why they read differently:

|                 | Revert to Prefab                                                                           | Apply to Prefab (per field)                                                                  |
| --------------- | ------------------------------------------------------------------------------------------ | -------------------------------------------------------------------------------------------- |
| Writes          | the scene                                                                                  | the `.gprefab` asset                                                                         |
| Undo            | yes — reported as an edit, so the section's commit boundary turns it into one undo command | **no**, like every other asset edit; the undo stack is the scene's and no scene data changed |
| Other instances | n/a                                                                                        | unchanged                                                                                    |

**Per-field apply writes the field onto the cached template and saves the template**, rather than
writing the instance. That is what makes it per-_property_: everything else in the prefab is still
the object that was loaded from the file, so it round-trips untouched. Verified by applying one
field of a particle emitter while a second field of the same component also differed — the
`.gprefab` came back with exactly one line changed, and the second field stayed as the prefab had
it. Whole-instance **Apply to Prefab** (the button on the prefab section, behind a confirmation
modal) is the blunt counterpart: it overwrites the asset with the entire subtree, carrying every
other difference with it.

Saving the template also means no placement guard is needed here. Whole-instance apply passes the
file's _existing_ root transform to `PrefabSerializer::Save` so an instance's position is never
baked into the asset; the template's root transform came from the file to begin with, so writing it
back preserves placement by construction — and applying a root transform field does what it says
rather than being silently dropped by a guard aimed at the other operation.

**Overrides are computed, not stored.** Unity records an override list on the instance; Ganymed diffs
the instance against the prefab instead. A recorded list is a second source of truth that goes stale
when the prefab changes, needs migrating when a field is renamed, and has to be maintained by every
edit path. A diff cannot be stale — it is recomputed from the two things it compares — and it needs
no format change beyond the canonical link.

The comparison is the serializer's own: **a field that would serialize identically is not an
override** (`EmitReflectedValue`). That keeps "overridden" and "would be written differently" the
same statement. A type with no YAML codec reports _not_ overridden — "cannot tell" must not become a
claim.

|                        |                                                                                                                                                                                                 |
| ---------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| What makes it possible | `PrefabMemberComponent::CanonicalID` on every instantiated entity — see [scene.md](../engine/scene.md#prefab-member-links)                                                                      |
| Per-field affordance   | Reflected sections only; the per-field hook lives in the property drawer (`OverrideHook`: `IsOverridden` / `Revert` / `Apply`)                                                                  |
| Hand-written sections  | Section-level `*` marker only — "something in here differs", not which field                                                                                                                    |
| Cost                   | **0.14 ms/frame** worst case (a selected prefab instance with a particle emitter: 46 fields, section marker plus every per-field query, Release). Zero when nothing selected is a prefab member |

Two limitations worth knowing:

- **Prefab instances already in committed scenes have no canonical link**, because they were
  instantiated before it existed. They report no overrides until they are re-instantiated — which
  _Revert Instance_ does, since it rebuilds the subtree from the file.
- The template cache is keyed on the prefab handle and dropped on every scene change (from
  `EditorLayer::RetargetPanels`, which new / open / play / stop all pass through) and after a
  whole-instance apply, which rewrites the file underneath it, and **on a `.gprefab` edited on
  disk** — an external editor, a git checkout, a branch switch. That last one arrives through
  `AssetManager::AddAssetChangedListener`: the watcher had always detected the edit, but nothing
  forwarded it for a type with no asset manager. Per-field apply needs no invalidation at all — it
  edits the template itself, so the two agree by construction and the blue tint clears on the next
  frame.

### Play / Stop (toolbar)

The main toolbar is a 41 px `SurfaceBg` child (`Theme().ToolbarHeight`) between the title bar and
the dockspace. Left cluster: select / translate / rotate / scale (`ICON_LC_MOUSE_POINTER`,
`MOVE`, `ROTATE_3D`, `SCALING`) via `EditorUI::IconButton` — 24×24, accent-filled when that
mode is `m_GizmoType`. Centre: `ICON_LC_PLAY` + "Play" in `Success` (edit) or
`ICON_LC_SQUARE_STOP` + "Stop" (play). There is no settings/screenshot cluster: those have no
backing feature, and a dead icon is worse than an absent one.

```
Play: m_ActiveScene = Scene::Copy(m_EditorScene); OnRuntimeStart(); panels retarget the copy
      UIEngine::LoadDocument("assets/ui/hud.rml")
Stop: UIEngine::CloseAllDocuments(); OnRuntimeStop(); m_ActiveScene = m_EditorScene; selection cleared
```

Game UI (RmlUi) is loaded on Play and closed on Stop, and renders _inside_ the viewport image
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

### Status bar

A 41 px `ChromeBg` child (`Theme().StatusBarHeight`) below the dockspace. Phase 2 deliberately
did not reserve the strip — `DockSpace(..., ImVec2(0, -StatusBarHeight))` would have been a
41 px hole until this existed. `EditorUI::StatusBarItem` draws each chip. The scene name +
dirty `*` also appears on the title-bar document tab; the chip here keeps the full-path tooltip.

Left, middot-separated:

| Chip                 | Source                                                                                                                                                                                                                                     |
| -------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| Scene filename + `*` | `m_EditorScenePath` (or `Untitled`), `m_UndoStack.IsDirtySinceSave()`. Dirtiness is stack _position_, so undoing back to the saved state clears the asterisk. Hover shows the full path. Asset edits (`.gmat`, prefab Apply) do not set it |
| Configuration        | `GE_DEBUG` / `GE_RELEASE` / `GE_DIST` → Debug / Release / Dist                                                                                                                                                                             |
| Backend              | `bgfx::getRendererName(bgfx::getRendererType())`                                                                                                                                                                                           |

Right, live counters: entity count (`m_ActiveScene` `IDComponent` storage — the play copy while
playing), `Renderer3D::GetStats().DrawCalls`, an exponential moving average of `1/ts` (raw
frame time flickers; debugger pauses ≥ 1 s are ignored so they do not pull the average to 1),
and play state (`Success` + `Play` while playing, `TextDim` + `Edit` otherwise).

There is no engine-version chip. Nothing in the repo is a version string, and a hard-coded one
would rot. BlankEngine's `Default` / `mainline` / `game` chips have no Ganymed source either and
are omitted for the same reason.

### Stats panel

Hovered entity, Renderer2D/3D counters (draw calls, quads, meshes, frustum-culled, instanced,
transparent, particle emitters/billboards/draws/culled), an **Asset Cache** readout (below),
and live post-processing settings (exposure, bloom threshold/knee/intensity/radius, FXAA).
Jolt debug-draw toggles live on the viewport header's Visualizers popup, not here.

A **Compiled** line sits under them: assets built this session, the wall clock they cost, and how
many came out of `assets/.compiled/` instead. A second run over an unchanged project must read
0 built ([assets.md](../engine/assets.md#compiled-outputs)).

The Asset Cache rows come from `AssetManager::GetCacheStats()`, one per registered manager, printed
as `resident / tracked / loading` — live objects, cache entries including ones whose object has
already been collected, and parses in flight. The gap between the first two _is_ eviction: close a
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

[`SceneHierarchyPanel`](../../GanymedEditor/source/Panels/SceneHierarchyPanel.h) — World Outliner
look, still docked as **Scene Hierarchy** (renaming the window would bust `imgui.ini`). Tree of
root entities, children drawn recursively (child lists are copied before iterating: re-parenting
during drag mutates the vector being walked). It is a `friend` of `Scene` and uses the immediate
Entity API — legal because panels run outside the system update.

The panel uses `BeginPanel` (padding 0) so the toolbar and column header reach the edges. The tree
itself is a zero-padding child under the header, so eye/lock/link cells line up with
`ColumnHeaderRow`. Properties is a second `BeginPanel("Properties")` from this class — not split
out, because the inspector undo protocol is load-bearing and lives in `DrawComponent`.

- **Toolbar:** `+` create (Empty Entity / Instantiate Prefab — the same items as the blank-space
  menu) and a `SearchField`. Sort / filter-dropdown / view-options are omitted: they have no
  backing behaviour.
- **Search** is a case-insensitive tag substring. A parent whose descendant matches stays visible
  and is forced open while the filter is active; branches that match nothing are omitted (including
  non-matching children of a matching parent).
- **Column header:** `Name` | eye | lock | link.
- **Type icon** by dominant component (prefab instance, camera, light, sky, audio, particle, mesh,
  sprite, script, else empty), tinted with the Content Browser's `AssetTint` where the type maps.
- **Prefab instance roots** use `Link` for the name and a non-interactive `ICON_LC_LINK` in the
  link column. Selected primary overrides that with `TextOnAccent` on the accent fill.
- **Selection fill** spans the row (`SpanAvailWidth`). Primary: solid `Accent` + `TextOnAccent`.
  Other selected rows: `Accent` at 40 % alpha + `TextPrimary`.
- **Eye / lock** are editor-side `std::unordered_set<UUID>` on the panel, not components. They
  survive play/stop (`RetargetPanels` does not clear them; UUIDs are stable across `Scene::Copy`)
  and are cleared on New/Open. Eye hides the entity **and its subtree** from `RenderSystem::
OnUpdateEditor` via the `EditorViewFilter` singleton (play/runtime still draw them). Lock blocks
  viewport click-select and the gizmo; the hierarchy can still select so you can unlock.
- Select by click; click empty space in the tree child to deselect.
- **Drag-drop re-parenting**: drag an entity onto another → `Scene::SetParent` (cycle-safe); onto
  empty space → unparent. Both record a `ReparentCommand`, but only after confirming the parent
  actually changed: `SetParent` silently no-ops on a cycle, and recording a move that did not
  happen would corrupt sibling order on undo.
- Right-click empty space → Create Empty Entity / Instantiate Prefab; right-click an entity → Delete.
- **Editor delete takes the whole subtree.** `Scene::DestroyEntity` keeps its orphan-the-children
  semantics as engine API, but no production editor deletes that way - Unity, Unreal and Godot all
  take the subtree - and the safety argument for orphaning ("you would lose the children")
  evaporates once the delete is one Ctrl+Z away.
- Deletion is deferred to after the hierarchy walk. Destroying a subtree mid-walk would invalidate
  the entt view the enclosing loop is iterating.
- ImGui IDs use the entt handle, not the UUID — old scene files could contain colliding UUIDs.

### Properties (drawn by the same panel)

Also `BeginPanel` (padding 0) so component headers reach the window edges. Tag and prefab
controls are indented 8 px; the header rows are not.

Tag edit (full-width `InputText`, one undo command per typing session). One section per
component type every selected entity has. **Add Component** is a full-width accent-outlined
button at the **bottom** of the stack (every type not already present — camera, sprite, lights,
sky light, animator, script, audio source, audio listener, particle emitter, rigid body,
colliders; one `DrawAddComponentEntry<T>` line each).

Each section is a 26 px `ChromeBg` row (`Theme().RowHeight`): chevron (`ICON_LC_CHEVRON_RIGHT` /
`_DOWN`), a per-type Lucide icon (not the entity's dominant-component icon), the name in Inter
Medium, a blue `*` when `IsComponentOverridden<T>`, and a right-aligned `OverflowMenuButton`
whose menu is **Remove component**. There is no per-component eye: Ganymed has no
component-enable flag, and a dead eye is worse than a missing one. Copy/Paste Component is
also omitted — that is a new editing feature (type-erased clipboard, paste onto an entity that
may already have the component, multi-select, undo), not chrome.

Open state uses the same `ImGuiStorage` key `TreeNodeEx((void*)typeid(T).hash_code())` used to,
so `imgui.ini` collapsed/expanded sections survive the restyle. Inner `Attr::Section`
`CollapsingHeader`s in the reflected drawer are field groups inside the body; they already pick
up `Header = ChromeBg` from `ApplyTheme` and were not restyled.

**The section contract**: `uiFunction` is `bool(T&)` — "did any widget in this section edit the
component this frame", the OR of the returns the widgets already produce. The rule it establishes,
which every section lambda must follow: _an inspector lambda mutates the component only when a
widget actually reported an edit, and returns true when it does._

That contract is what makes undo possible without value diffing. Diffing would need 16
`operator==`s and would still be wrong: the Transform section round-trips rotation through degrees
and back, which can change bits with no user input at all, so a diff scheme mints a phantom
command for merely selecting an entity. "The widget said so" is ground truth ImGui already
computes.

**The commit boundary.** A two-second drag writes the component on ~120 frames; undo wants one
command per gesture. `DrawComponent<T>` reads ImGui's active item before and after the section's
widgets: the frame an item inside the section becomes active starts a _pending_ edit and that
frame's pre-copy is the before-value; the frame it stops being active commits, capturing the
after-value then. Widgets with no active phase — a drag-drop assignment landing on the section —
report their edit and commit in the same frame. A pending edit no frame of which reported an edit
is dropped, which is what makes a click-without-drag and an opened-then-closed combo free. If the
section stops being drawn mid-gesture, an end-of-frame flush commits what was recorded.

**Header items sit outside that window.** The chevron, type icon, name, and `OverflowMenuButton`
are submitted _before_ the `GetActiveID()` read. Clicking them cannot mint a `ComponentEditCommand`
— `activeOnEntry` already equals the header item, so the body sees no ActiveId change. Inner
`Attr::Section` `CollapsingHeader`s sit _inside_ the window; a click that grabs ActiveId with
`Edited` staying false is dropped at commit, same as any click-without-drag.

### The generic (reflected) inspector

Most sections no longer have a hand-written body. `EditorUI::DrawReflectedComponent(component)` in
[`EditorInspector.h`](../../GanymedEditor/source/EditorInspector.h) draws a component from what
`entt::meta` knows about it — field order, labels, ranges, drag speeds, the colour-vs-position widget
choice, enum entries and notes all come from
[`ComponentReflection.cpp`](../../GanymedEngine/source/GanymedE/Reflection/ComponentReflection.cpp).
Adding a field to a converted component is one `.data<>` line in the registration, not an edit here.

**It does not touch the undo protocol, and that is the point.** REFLECTION_ROADMAP R2 expected the
generic drawer to "own `ActiveId` across a drag exactly as the hand-written path does". It does not
have to: the commit boundary lives in `DrawComponent<T>`, which wraps the _section_, so a generic
body only has to keep the section contract above — mutate on a reported edit, return true when it
does. Every drawer does, so a converted section keeps one-command-per-gesture with **zero** change
to `EditorUndo`.

Dispatch is a `meta_type`-keyed map of `PropertyDrawer` function pointers, editor-side. It has to be
editor-side: `.custom<>` holds exactly one payload per meta object, so a second registration pass
from the editor would _overwrite_ the engine's attributes rather than add to them. Defaults cover
`float`, `bool`, `int32`/`uint32`, `std::string`, `glm::vec2/3/4`, every registered enum, and
`AssetRef<T>`; `RegisterPropertyDrawer` overrides one for a type. A field whose type has no drawer is
skipped and named once in the log — a missing drawer should be loud, not invisible.

Trait handling: `Hidden` and **`CustomDrawer`** are skipped, `ReadOnly` draws disabled (and never
reports an edit), `Color` selects `ColorEdit` over the X/Y/Z row, and `Radians` converts to degrees
for display.

The inspector reads `CustomDrawer`, never `CustomWriter` — the two halves of the old single `Custom`
flag, split when the serializer conversion finished. That split is what lets `AnimatorComponent::Clip`
and the two particle curves keep bespoke widgets while serializing generically; see
[scene.md](../engine/scene.md).

**A registered drawer wins over `Trait::Flatten`.** That ordering is load-bearing rather than
incidental: `Flatten` is a statement about the _file_ — "this struct's fields are siblings on disk" —
and the two types that make it want opposite things from the inspector. `PhysicsMaterial` has no
drawer and falls through to the nested-struct fallback below, so a collider still shows `Friction`
and `Restitution` as its own rows. `RangeF` has one, and drawing its `Min` and `Max` as two loose
rows is precisely the widget that type was introduced to replace.

**Converted (15 of 19):** Sprite Renderer, Directional / Point / Spot Light, Transform, **Camera**,
**Sky Light**, Audio Listener, Audio Source, Prefab Instance, Particle Emitter, Rigid Body, and the
three colliders.

Two of those needed something the generic path alone cannot do, and both are handled _around_ it
rather than inside it:

- **Transform** does `MarkChanged<TransformComponent>` after an edit. Editing a component directly
  is invisible to change tracking, so the cached world transform would never refresh. A side effect
  is not something reflection can express — but nothing stops the section from running one after
  `DrawReflectedComponent` returns true. Its degrees round-trip and reset values _are_ expressed, as
  `Trait::Radians` and `Attr::Reset`.
- **Spot Light** clamps outer ≥ inner afterwards. A generic drawer sees one field at a time and
  cannot express a cross-field invariant, but the section can fix up the component the drawer just
  wrote. The clamp is the section's job; the widgets are not.

**Still hand-written, and none of it for want of effort** — each is blocked by something the
vocabulary deliberately cannot say:

| Section     | Why                                                                                 |
| ----------- | ----------------------------------------------------------------------------------- |
| Static Mesh | The material-override list is sized by the **mesh asset**, not by component members |
| Animator    | Clip names come from the mesh asset                                                 |
| Bone Attachment | Joint names come from the **target** entity's skeleton, not this entity's        |
| Script      | The field schema comes from Lua, not from C++                                       |

**Camera and Sky Light converted via a field filter.** Their blocker was field _visibility_
depending on another field's value — and unlike a clamp, that cannot be applied after the generic
drawer has run, because you cannot un-draw a field. `EditorUI::FieldFilter` is a predicate asked per
field _before_ anything is submitted, so the section keeps its one cross-field rule and stops
hand-drawing every widget around it:

- **Camera** shows the perspective fields or the orthographic ones. `SceneCamera`'s own fields appear
  as rows of the section via the nested-struct fallback — a reflected struct with no drawer of its
  own is drawn inline. That is an _inspector_ decision and says nothing about serialization, where
  the camera really is a nested map on disk; using `Trait::Flatten` to get that layout would have
  been a lie, and the generic writer now depends on it not being told one.
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

**Labels are never passed as format strings.** `ImGui::Text`, `TextDisabled` and `TreeNodeEx`'s
trailing argument are all printf formats, and the strings this panel feeds them are component
display names, field labels and entity names — the last of which a user types. `ImGui::Text(name)`
for an entity called `%s` reads an argument that was never pushed. Every such site uses
`TextUnformatted`, or `"%s"` with the string as an argument; the entity tree node already did, and
the component header and `DrawVec3Control` now match it. GCC's `-Wformat-security` is what surfaced
the two that did not — MSVC has no equivalent diagnostic, so the editor had carried them since the
widgets were written.

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
- Bone attachment: **Target** is a drop from the outliner (zero / Parent button = hierarchy parent),
  and **Joint** is a combo over the *target's* `skeleton.JointNames`, not this entity's — the
  inspector has not previously read another entity's mesh for any component. Offset and Rotation
  are reflected (`Trait::Radians` on Rotation). Local transform is ignored while the socket
  resolves; edit Offset, not the gizmo, to place the attached mesh in the hand.
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

**Ctrl+click** adds an entity to the selection or removes it, **Shift+click** selects everything
between the anchor and the clicked entity, and a plain click replaces the selection. Every selected
entity is highlighted in the hierarchy (primary: solid accent; others: 40 % alpha).

Shift-range works off `m_VisibleOrder`, the flattened tree the panel now records as it draws — the
thing it used not to keep, since drawing recursively leaves the visible order existing only as the
shape of the call stack. Children of a collapsed node are never drawn and so are never in it, which
is what makes a range cover what the author can see rather than what the scene contains. Two details
follow the convention every file browser has: the **anchor** is the last entity picked _without_
shift, held apart from the primary so repeated shift-clicks re-extend from the same place rather
than walking it along; and a range **replaces** the selection rather than adding to it, so a
mis-aimed range is fixed by aiming again. The click is recorded and serviced after the walk
completes, because mid-draw the flattened order only holds the nodes drawn so far.

The design keeps a **primary** selection — the entity clicked last, `GetSelectedEntity()` — and adds
the full set beside it as `GetSelection()`, primary first. That is why multi-select cost six call
sites outside the panel instead of thirty: the tag field and every prefab action still read
the primary. The gizmo still _grabs_ the primary, then applies the same world-space delta to the
rest of the selection.

| Behaviour             | Rule                                                                                                                                                                                                                                                |
| --------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Which sections appear | Only components **every** selected entity has. Showing one that only some have would make an edit either silently skip entities or silently add the component to them                                                                               |
| Fields that disagree  | Tinted **amber**. Mixed wins over the prefab-override blue when both apply — "these entities disagree" is the more urgent fact, because the widget is showing one of several values rather than the value                                           |
| Editing               | The widgets drive the **primary**; the new value is copied to the rest _after_ a widget reports an edit. Merely selecting several entities never flattens their differing values                                                                    |
| Undo                  | **One entry per gesture, spanning the whole selection** — including edits with no active phase (a checkbox, a combo, a drop). Verified by driving the editor: a 4-entity checkbox toggle produces `UndoDepth == 1` and one Ctrl+Z restores all four |
| Delete                | Deletes every selected entity                                                                                                                                                                                                                       |

Propagating after the fact, rather than driving N widgets, is what keeps every property drawer
single-entity and unaware that multi-edit exists — the same trick the prefab-override hook uses.

**The before-values are snapshotted in `DrawComponent`, before the widget runs.** They used to be
read at the moment a gesture started, which was wrong twice over: the no-active-phase path (a
checkbox, a combo, a drop) has no such moment at all, so those entities got no undo entry and their
edit was unrecoverable — toggle a checkbox across four entities, press Ctrl+Z, and one came back;
and even on the gesture path, a widget that became active _and_ reported an edit in the same frame
had already propagated to the rest, so their "before" was the new value. The snapshot is taken only
when something else is selected, so single-entity editing — every frame of ordinary work — copies
nothing.

## Content Browser panel

[`ContentBrowserPanel`](../../GanymedEditor/source/Panels/ContentBrowserPanel.h) — a `BeginPanel`
view of `assets/`. Window title stays **Content Browser** (renaming would bust `imgui.ini`). Two
classes of entry are hidden: anything whose name starts with `.` (the `.compiled/` mesh cache
today) and `.meta`/`.meta.bad` sidecars. Hiding the sidecars is not cosmetic — one per asset would
double every row and offer **Import** on a file that is not an asset. They are the
`AssetManager`'s to write, never a human's (see [assets.md](../engine/assets.md#the-meta-sidecar)).

**Toolbar.** `SearchField` (case-insensitive **filename** substring across the whole `assets/`
tree) and a sort popup (Name / Type; directories always first). Empty query shows the current
folder only. BlankEngine's `+` add and toolbar Import are omitted: there is no create-asset path,
and Import already lives on the file context menu. A dead `+` is worse than a missing one.

**Breadcrumb** (`SurfaceSunken`). `←` / `→` history (two `std::vector<path>` stacks), `↑` parent,
`ICON_LC_HOME` for the asset root, then clickable path segments. Every navigation path — history,
parent, home, crumbs, sidebar, double-click — goes through `TryNavigate`, which is the
path-normalized root-escape check the old `<-` button used to own alone.

**Split.** A two-column `BeginTable` (`Resizable | BordersInnerV`); sidebar width persists in
`imgui.ini`. Left: folder tree (directories only, Lucide folder glyphs). The current folder
pushes `Header`/`HeaderHovered`/`HeaderActive` to Accent so `TextOnAccent` glyphs stay on a
lilac fill — theme `Header` is ChromeBg (inspector sections), and ImGui uses that colour for
an idle selected TreeNode, not `HeaderActive`. Right: grid or list of the current folder.
Grid selection is a solid `Accent` cell fill (same contrast contract). List already pushed
those Header colours. The legacy `ImGui::Columns` grid is gone.

**Footer.** Visible item count (after the search filter) on the left; grid / list toggle on the
right. Grid keeps the PNG directory/file thumbnails with `AssetTint`. List uses Lucide type icons.

**Cache.** One recursive walk fills a flat index of every visible file and folder _and_ the
sidebar tree. It rebuilds when a watched directory's `last_write_time` moves, when
`AssetWatcher` reports a reload, or every 0.25 s — `AssetWatcher` only polls _indexed files_,
so an empty folder created in Explorer would never dirty from Reloads alone. Search and the
current-folder view are filters of that index (`std::string::find` on a lowercase name stored
at walk time). Navigate does not re-walk. Keystrokes never hit the filesystem.

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
- Right-click the **panel background** (`BeginPopupContextWindow` with `NoOpenOverItems`, so it
  never competes with the per-file menu) → asset-tree maintenance: **Rescan `assets/`**, and
  **Clean N orphaned `.meta` sidecar(s)**. The clean item carries its own count and is _disabled_
  when the count is zero — the disabled item with "No orphaned `.meta` sidecars" on it is the
  report, which is why it is drawn rather than hidden. Its tooltip says what a sidecar holds,
  because the action is not undoable: see
  [assets.md](../engine/assets.md#orphaned-sidecars) for why the editor asks a person rather than
  reaping at boot.

## Map panel

[`MapPanel`](../../GanymedEditor/source/Panels/MapPanel.h) — `BeginPanel("Map")`, docked with Stats
on the right (dock-layout version 3). Palette, placement options, duplicate-along-axis, and
disabled stubs for the M2–M4 sections. There is no thumbnail system; rows are `AssetTint` icon +
filename.

**Snap model.** `MapSnapSettings` is owned by `EditorLayer` and read by both placement and
ImGuizmo. Defaults: enabled, translate 0.5 m, rotate 15° (45° is a preset), scale 0.1, snap to
surface, sit-on-bounds, align-to-normal off, grid height 0. **Ctrl inverts `Enabled`.** That is a
behaviour change from the old "Ctrl enables 0.5 / 45°". Modular kit pieces only line up if
snapping is the resting state; a wall 0.03 m off its neighbour is worse than an accidentally
snapped drag. Unity defaults snapping off; Unreal and Blender default it on. For a map tool the
Unreal/Blender default is the one that matches the failure mode.

**Palette.** `AssetManager::ForEachAsset` filtered to `Prefab` and `StaticMesh`. The pinned subset
is what the panel shows. Persistence is `<asset-root>/.editor/map_palette.yaml` — a fact about the
content, not window layout, so it does not live in `imgui.ini`. The editor does not link yaml-cpp;
the file is a hand-written `pinned:` list. `.editor/` starts with a dot, so the Content Browser
already hides it.

**Placement.** Clicking a pinned row instantiates **once** (`InstantiatePrefab(..., recordUndo=false)`
or `MeshImporter::Instantiate`) and then only writes the root transform. The preview is a real
entity — it appears in the outliner — because there is no translucent mesh shader to ghost it
with. It is `RaycastFilter::Exclude` so it cannot snap to itself. Esc / RMB / New / Open / Play
destroy it without an undo entry. Save while a preview exists will persist it — it is a real
entity, and the serializer has no notion of "not yet committed". Cancel first.

Per-frame, in order: M0 hit (or the work plane at `GridHeight` when Snap to surface is off) →
quantize the **hit point** on all axes when snap is on (never the final origin, or sit-on-bounds
gets rounded too) → sit offset along local Y from the union of descendant mesh AABBs in root
local space, `q * (0, -bounds.Min.y * scale.y, 0)` → rotation `align * yaw * authoredBase`, then
`Scene::MarkChanged<TransformComponent>`. Sit-on-bounds is the setting that makes a crate authored
around its centre land *on* the floor rather than through it.

LMB pushes one `AddEntitiesCommand` **after** that transform is final, then either exits or (Shift)
instantiates a fresh preview. Alt+LMB is unsnapped. `[` / `]` step yaw by `Rotate`.

**Duplicate along axis** in the panel: count (includes the original), spacing, axis. `count - 1`
calls to `Scene::DuplicateEntity`, world-axis offsets, one `CompositeCommand`.

## Typed drag-drop

[`AssetDragDrop.h`](../../GanymedEditor/source/AssetDragDrop.h) —
`namespace GanymedE::EditorUI`. Every drop target goes through it, so
`AssetTypeFromExtension` is the single source of truth for what a target accepts on the editor side
too (it previously wasn't used here at all: each site hand-rolled
`extension()` → `::tolower` → string compare).

| Call                          | Returns                                                                                                                                                                                                                                                                                                             |
| ----------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `AcceptAssetDrop(type)`       | `optional<path>` — the dropped path relative to `assets/`, iff its type matches                                                                                                                                                                                                                                     |
| `AcceptAssetDrop({types...})` | `AssetDrop { Type, Path }`, falsy when nothing matched — for targets accepting several types. The viewport uses it for Scene / StaticMesh / Prefab, where the list form is **mandatory**: ImGui clears the payload as soon as one target delivers it, so three single-type calls would let only the first ever fire |
| `AcceptAssetDropHandle(type)` | `ImportAsset` (idempotent, and persists identity itself) on match, else `InvalidAssetHandle`                                                                                                                                                                                                                        |
| `AcceptAssetDropRef<T>()`     | The same, typed: an `AssetRef<T>`, unset when nothing matching was dropped                                                                                                                                                                                                                                          |

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
`CONTENT_BROWSER_ITEM` payload type (the type filtering happens _after_ accepting, on the extension),
so the first call always wins the delivery and the second type would never fire.

## Adding an editor feature — where things hook

| Want to…                      | Touch                                                                                                                                                                                            |
| ----------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| New panel                     | Create under `Panels/`, own it in `EditorLayer`, call `OnImGuiRender`, dock it in the DockBuilder block                                                                                          |
| New chrome colour             | Add a token on `EditorTheme`, map it in `ApplyTheme` if it is an ImGui style colour, consume `Theme().X` — never a new literal                                                                   |
| Panel furniture               | `EditorUI::BeginPanel`, `PanelToolbarRow`, `SearchField`, `ColumnHeaderRow`, `IconButton`, `ToolbarSeparator`, `OverflowMenuButton`, `RowActionIcons`, `StatusBarItem` — do not hand-roll chrome |
| Host title bar                | `EditorTitleBar.cpp`; File/Edit/View stay in `EditorLayer::UI_Menus`                                                                                                                             |
| New component UI              | `SceneHierarchyPanel::DrawComponents` (+ Add-Component popup)                                                                                                                                    |
| Custom canvas widget          | `EditorWidgets.cpp`; one `InvisibleButton` spanning the canvas so `ActiveId` holds for the drag; return true only on a real value change                                                         |
| New asset type in the browser | `AssetTypeFromExtension`, icon tint map, `IsImportableAsset`, then `EditorUI::AcceptAssetDrop(<type>)` at the consumer                                                                           |
| New shortcut                  | `EditorLayer::HandleShortcuts` (editor-global) or `OnKeyPressed` (viewport-gated, like the gizmo keys)                                                                                           |
| New undoable operation        | An `EditorCommand` subclass in `EditorUndo.h`, pushed where the operation happens; `CompositeCommand` when several steps must undo as one                                                        |
| New scene-wide toggle         | Prefer a singleton in `SceneSingletons.h`, edit it from the Stats panel like `PhysicsSettings`                                                                                                   |

Remember the editor-code rules: panels may use the immediate Entity API (they run outside the
update loop — the asserts in `Entity` enforce this), any direct write to a tracked component must
be followed by `Scene::MarkChanged<T>`, an inspector lambda mutates its component only on a real
widget edit and returns true when it does, and anything that changes the scene should push an
`EditorCommand`.

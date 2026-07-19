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
   has no primary `CameraComponent`; the physics-debug toggles are copied into the scene's
   `PhysicsSettings` each frame).
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
- **Drag-drop from the Content Browser**: `.ganymede` opens the scene; `.gltf`/`.glb`
  (edit mode only) imports and instantiates the mesh via `MeshImporter::Instantiate` and selects
  it.
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
| Ctrl+N / Ctrl+O / Ctrl+Shift+S | New / Open / Save-As scene |
| Ctrl (held while dragging gizmo) | Snapping |
| F1 | bgfx stats overlay |

### Play / Stop (toolbar)

```
Play: m_ActiveScene = Scene::Copy(m_EditorScene); OnRuntimeStart(); panels retarget the copy
Stop: OnRuntimeStop(); m_ActiveScene = m_EditorScene; selection cleared
```

The runtime scene is a disposable UUID-keyed deep copy — physics and scripts can do anything to
it, and Stop restores the authored scene untouched. Scene switching (`OpenScene`) stops play
first.

New scenes are seeded by `SetupDefaultEnvironment`: a "Sun" (directional light tilted from above,
shadow-casting) and a "Sky Light" (HDR environment `environments/studio_small_08_1k.hdr` when
present, procedural fallback otherwise), so imported meshes are lit immediately.

### Stats panel

Hovered entity, Renderer2D/3D counters (draw calls, quads, meshes, frustum-culled, instanced,
transparent), live post-processing settings (exposure, bloom threshold/knee/intensity/radius,
FXAA), and Jolt debug-draw toggles (visible during Play; draws Jolt's body state instead of the
authored collider gizmos).

## Scene Hierarchy panel

[`SceneHierarchyPanel`](../../GanymedEditor/source/Panels/SceneHierarchyPanel.h) — tree of root
entities, children drawn recursively (child lists are copied before iterating: re-parenting during
drag mutates the vector being walked). It is a `friend` of `Scene` and uses the immediate Entity
API — legal because panels run outside the system update.

- Select by click; click empty space to deselect.
- **Drag-drop re-parenting**: drag an entity onto another → `Scene::SetParent` (cycle-safe); onto
  empty space → `Unparent`.
- Right-click empty space → Create Empty Entity; right-click an entity → Delete (children survive
  as roots).
- ImGui IDs use the entt handle, not the UUID — old scene files could contain colliding UUIDs.

### Properties (drawn by the same panel)

Tag edit; **Add Component** popup (every component type not already present — camera, sprite,
lights, sky light, rigid body, colliders); one collapsible section per component
(`DrawComponent<T>` helper with a remove-component menu). Notable behaviors:

- Transform edits go through `DrawVec3Control` (the X/Y/Z colored reset buttons) and call
  `MarkChanged<TransformComponent>` only when a value actually changed.
- Camera: projection type combo, per-type parameters, Primary / FixedAspectRatio.
- Static mesh: shows the mesh asset (handle + path) — assign by dragging from the Content Browser.
- Sky light: environment asset, sky/ground colors, intensity, DrawSkybox.
- Colliders: dimensions, offset, friction/restitution.

Adding a component type means extending this panel's Add-Component popup and `DrawComponents` —
one of the two remaining hand-maintained per-component lists (the other is the serializer).

## Content Browser panel

[`ContentBrowserPanel`](../../GanymedEditor/source/Panels/ContentBrowserPanel.h) — a grid view of
`assets/` (the `.assets/` mesh-cache directory is hidden):

- Directory/file icons, tinted by asset type (mesh blue, environment orange, scene green, texture
  pink, material purple). Double-click enters directories; the `<-` button goes up but can never
  escape the asset root (path-normalized check).
- Every item is a drag source (`CONTENT_BROWSER_ITEM`, relative path payload) — the viewport and
  the properties panel accept the relevant types.
- Right-click on an importable file (mesh/environment/texture/material) → **Import**, registering
  it with the `AssetManager` (idempotent; persists `AssetRegistry.gr` immediately).

## Adding an editor feature — where things hook

| Want to… | Touch |
|---|---|
| New panel | Create under `Panels/`, own it in `EditorLayer`, call `OnImGuiRender`, dock it in the DockBuilder block |
| New component UI | `SceneHierarchyPanel::DrawComponents` (+ Add-Component popup) |
| New asset type in the browser | `AssetTypeFromExtension`, icon tint map, `IsImportableAsset`, drag-drop handling at the consumer |
| New shortcut | `EditorLayer::OnKeyPressed` |
| New scene-wide toggle | Prefer a singleton in `SceneSingletons.h`, edit it from the Stats panel like `PhysicsSettings` |

Remember the two editor-code rules: panels may use the immediate Entity API (they run outside the
update loop — the asserts in `Entity` enforce this), and any direct write to a tracked component
must be followed by `Scene::MarkChanged<T>`.

# Editor Visual Parity — the Cold War editor look

Goal: make GanymedEditor read as the same *class* of tool as the Cold War (WoT HEAT 1.3 / Engine
0.9.26) editor. Not a pixel-for-pixel clone — the palette's neutral ramp, density, geometry and
structural furniture are copied; the accent colour stays Ganymed's.

This is a **visual + layout** milestone. It adds no engine subsystems and no new editing features.
Where a piece of Cold War furniture has no backing feature in Ganymed (a Profiler tab, a Validation
Log), it is **not** stubbed in — an empty panel is worse than an absent one.

## Decisions already taken

| Question | Decision |
|---|---|
| Scope | **Full visual parity**: theme, fonts, icons, density, dock chrome, per-panel toolbars/search/column headers, status bar, viewport overlay bars, asset-browser furniture, custom window chrome. No new panels. |
| Icons | **Icon font merged into the ImGui atlas** (one TTF + a codepoint header). Icons become text: they scale with DPI, tint with `ImGuiCol_Text`, and need no texture bookkeeping. |
| Window chrome | **Custom** — undecorated window, ImGui-drawn title bar with menu, document tab and window buttons. Scheduled **last** (phase 9); see the cost warning there. |
| Palette | **Cold War's system, Ganymed's accent.** Same neutral ramp, same zero-rounding geometry, same semantic roles; `#F7A356` orange is replaced by a violet derived from the app icon. |

## Reference data

Everything below was **sampled from `coldwar_editor.png`**, not eyeballed. The screenshot is 1:1
native (2560 px wide; the viewport measures 1362 px against the editor's own `Free Aspect: 1360x878`
readout), so the pixel measurements are unscaled and directly comparable to a 100 %-scaling Ganymed
window.

### The neutral ramp

Six values carry the entire UI. That is the first thing to copy — Ganymed's current theme uses a
dozen near-identical greys (`0.1/0.105/0.11`, `0.2/0.205/0.21`, `0.15/0.1505/0.151`) that read as
noise rather than as a system.

| Token | Hex | Where Cold War uses it |
|---|---|---|
| `ChromeBg` | `#1A1A1A` | Window title bar, dock tab-bar strips, status bar, **inspector component-header rows**, panel gutters |
| `SurfaceSunken` | `#272727` | Column-header rows, search fields, the asset-browser breadcrumb bar |
| `SurfaceBg` | `#313131` | Panel content background, toolbar strips, active dock tab, asset grid |
| `Border` | `#1A1A1A` | Row separators, panel-to-panel gutters (1 px). Note: same value as `ChromeBg` — Cold War separates panels with *chrome-coloured gaps*, not with lighter borders |
| `GrabBg` | `#4D4D4D` | Scrollbar grab, slider grab |
| `TextPrimary` | `#CCCCCC` | Body text (8.1:1 on `SurfaceBg`, 10.8:1 on `ChromeBg`) |
| `TextDim` | `#878787` | Secondary/hint text (3.6:1) |
| `TextDisabled` | `#717171` | Disabled items, unselected tab labels (2.7:1 — deliberately below AA; it is "off", not "readable") |

### Semantic colours

| Token | Cold War | Role |
|---|---|---|
| Accent (fill) | `#F7A356` | Selected outliner row (**solid fill, near-black glyphs** — 10.3:1), active toolbar toggles (`Icons`, `Visualizers`, the active tool) |
| Link / entity name | `#589FFD` | Entity names in the outliner (4.8:1). A **separate role** from the accent |
| Success | `#65CC6B` | `Play (Custom)` label, the +Y gizmo axis |
| Axis X / Y / Z | red / green / blue | The viewport transform readout, and Ganymed's existing `DrawVec3Control` reset buttons |

### Metrics

| Metric | Cold War | Ganymed today |
|---|---|---|
| Corner rounding | **0 px everywhere** — windows, frames, tabs, grabs, popups | ImGui dark defaults (frames/grabs rounded); the curve editor even hard-codes `3.0f` |
| Tree row pitch | 26 px | ~23 px |
| Selection row height | 24 px of the 26 px pitch (⇒ `FramePadding.y ≈ 3`) | ImGui default 4 |
| Title bar | 40 px, `#1A1A1A` | OS-drawn, **white** (`#F3F3F3`) |
| Main toolbar strip | 41 px, `#313131`, dense icon-only, grouped by separators | A **docked, resizable** window taking 6 % of window height with one centred play button |
| Dock tab-bar strip | 35 px, `#1A1A1A`; active tab = `#313131`, active label `#FFFFFF`, inactive label `#717171` | ImGui defaults + a **collapse arrow** (`▼`) on every panel |
| Per-panel toolbar row | 44 px, `#313131`, ends in a 1 px `#1A1A1A` rule | none |
| Column-header row | 26 px, `#272727` | none |
| Status bar | 41 px, `#1A1A1A` | none |
| Font | neutral grotesque, ~18 px, ~26 px line | **Inter Regular** 18 px (phase 0) |

### The accent, worked out rather than guessed

The app icon's violet is `#7B43C2` (hue 266°, relative luminance **0.121**). Cold War's orange sits
at **0.466** — that gap is not a preference, it is physics: at hue 266° only the red and blue
channels contribute, so a saturated violet cannot reach an orange's luminance. Dropping `#7B43C2`
into the selected-row role with dark glyphs would be unreadable.

Two ways out, and they are not equivalent:

1. **Lighten the fill to a lilac and keep Cold War's dark glyph.** `#B182ED` fill with `#1A1A1A`
   text is 6.0:1; `#C39BFF` with `#101010` is 8.6:1, which is closest to the orange's feel.
   Preserves the *structure* of the treatment (bright fill, dark glyph) — a loud, unmistakable
   selection.
2. Keep the fill dark (`#7B43C2`) and use light glyphs (6.1:1 against white). Readable, but it
   inverts the treatment: the selected row becomes *darker* than the panel, which reads as
   "collapsed/disabled" in every other editor.

**Recommendation: option 1.** Proposed ramp:

| Token | Hex | Role |
|---|---|---|
| `Accent` | `#B182ED` | Selection fill, active toggle fill, checkmarks, drag/slider grabs |
| `AccentHover` | `#C39BFF` | Hover over an accented control |
| `AccentActive` | `#9152E0` | Pressed |
| `AccentText` | `#B07BF4` | Accent-coloured *text* on a panel background (4.3:1) — an active label with no fill |
| `TextOnAccent` | `#1A1A1A` | Glyphs on top of `Accent` |

Keep `#589FFD` for entity-name links. Collapsing links into the accent would merge two independent
signals ("this is selected" and "this is an entity reference") into one colour.

## Gap analysis, ranked by visible change per unit of work

| # | Gap | Impact | Effort | Phase |
|---|---|---|---|---|
| 1 | White OS title bar against a dark app | very high | high | 9 |
| 2 | Icon font in the atlas; most panels still unlabeled | very high | medium | 0 **done** (play/stop uses `ICON_LC_*`; remaining chrome in 4–6) |
| 3 | Montserrat (geometric display face) instead of a UI grotesque | high | low | 0 **done** (Inter; Montserrat remains for RmlUi HUD) |
| 4 | Noisy near-identical greys; rounding on | high | low | 1 **done** |
| 5 | Panels have no toolbar row, no search, no column headers | high | medium | 3 |
| 6 | Outliner rows are bare `TreeNodeEx` labels — no type icon, no per-row actions, no link colour | high | medium | 4 |
| 7 | Toolbar is a docked window with one centred button | high | low | 2 |
| 8 | Collapse arrow (`▼`) on every dock tab; no `•••` overflow | medium | trivial | 1 **done** (`WindowMenuButtonPosition = None`; `•••` is phase 3) |
| 9 | No status bar | medium | low | 7 |
| 10 | Inspector component headers are ImGui `CollapsingHeader`s, not full-width `#1A1A1A` rows with icon + eye + `•••` | medium | medium | 5 |
| 11 | Content Browser: no breadcrumb, no folder sidebar, no search, no item count, no view toggle | medium | high | 6 |
| 12 | Viewport has no header row (camera/quality/visualizers) and no transform readout | medium | medium | 8 |
| 13 | Hard-coded colours scattered across 5 files (see below) | low visually, high for maintenance | low | 1 **done** |
| 14 | Dock tab label colour cannot differ selected vs unselected | low | — | accepted deviation |

**The scattered colours that phase 1 absorbed** — these were the reason a token layer was
worth the file:

| Site | Value | Became |
|---|---|---|
| `EditorInspector.cpp` mixed/override tints | amber / override blue | `Theme.FieldMixed` / `Theme.FieldOverride` |
| `EditorWidgets.cpp` vec3 reset buttons | red/green/blue | `Theme.AxisX/Y/Z` |
| `EditorWidgets.cpp` curve/gradient editors | eight `IM_COL32` literals + `3.0f` rounding | `Theme.*` + rounding 0 |
| `ContentBrowserPanel.cpp` per-asset tints | eight tints | `Theme.AssetTint[AssetType]` |
| `EditorLayer.cpp` toolbar transparent-button push | hover/active from `ImGuiCol_Button*` | accent tokens at 35%/55% (`IconButton` is still phase 3) |

`SceneHierarchyPanel.cpp` had no colour literals; it was listed as a consumer and did not need
edits.

Note the collision phase 1 resolved: the prefab-override blue `#73B8FF` and the entity-link blue
`#589FFD` are the same colour to the eye, in the same window, meaning different things. Overrides
keep blue, links keep blue, and they never appear in the same panel — the outliner has no property
rows and the inspector has no entity links — recorded in `editor.md`.

## The token layer

New files, editor-side:

```
GanymedEditor/source/EditorTheme.h     // EditorTheme struct + accessor
GanymedEditor/source/EditorTheme.cpp   // the two presets + Apply()
GanymedEditor/source/EditorIcons.h     // ICON_* codepoint defines (vendored header)
GanymedEditor/source/EditorFonts.h/.cpp// atlas construction
```

```cpp
namespace GanymedE::EditorUI {

    struct EditorTheme
    {
        // Neutral ramp
        ImU32 ChromeBg, SurfaceSunken, SurfaceBg, Border, GrabBg;
        ImU32 TextPrimary, TextDim, TextDisabled;
        // Semantic
        ImU32 Accent, AccentHover, AccentActive, AccentText, TextOnAccent;
        ImU32 Link, Success, Warning, Error;
        ImU32 FieldMixed, FieldOverride;
        ImU32 AxisX, AxisY, AxisZ;
        // Metrics
        float RowHeight, ToolbarHeight, TabBarHeight, StatusBarHeight, ColumnHeaderHeight;
    };

    const EditorTheme& Theme();
    void ApplyTheme(const EditorTheme&);   // writes ImGuiStyle colours + metrics
}
```

Design rules, and why:

- **Flat struct of `ImU32`, no inheritance, no registry, no runtime theme editor.** One `Apply()`
  with one call site. A theming *framework* is the classic editor-tooling over-build; Unity and
  Unreal both ship exactly this — a serialized style asset consumed by one applier — and neither
  makes it extensible from the outside.
- `ImU32` rather than `ImVec4` because most consumers are `ImDrawList` calls. Convert at the
  `PushStyleColor` boundary.
- **Two presets ship**: `Ganymed` (the default — Cold War ramp + violet accent) and `Coldwar`
  (literal sampled values). The second exists to make side-by-side screenshot verification cheap,
  and costs one extra function.
- Semantic names, never colour names. `Accent`, not `Violet`; the whole point is that re-branding is
  a data change.

### Where theming lives

`ImGuiLayer::OnAttach` keeps `ImGui::StyleColorsDark()` and the built-in font. Nothing else.
`EditorLayer::OnAttach` calls `EditorFonts::Load()` then `ApplyTheme(MakeGanymedTheme())`.
`SetDarkThemeColors` is gone from the engine.

**No engine API is needed for the font swap.** `ImGuiRendererBgfx::NewFrame()` already rebuilds the
atlas whenever `!io.Fonts->IsBuilt()` (`ImGuiRendererBgfx.cpp:89`), so the editor can
`io.Fonts->Clear()`, add its own fonts, and the bgfx texture is recreated on the next frame with no
backend change. The one trap to document: **`ImFont*` values held across a `Clear()` dangle**, so
`io.FontDefault` must be reassigned in the same call — which is exactly why the engine should not be
holding font pointers the editor then invalidates.

## Phases

Each phase is independently shippable and independently visible. Build and screenshot after each;
per `AGENTS.md`, the matching doc update lands in the same change.

MSBuild fast path:

```
"C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" \
  GanymedEditor\GanymedEditor.vcxproj /p:Configuration=Debug /p:Platform=x64
```

Phases that add files need `scripts\Win_GenerateProjects.bat` re-run first.

---

### Phase 0 — Typography and icons — **done**

Inter Regular/Medium + Lucide, FreeType `LightHinting`, `EditorFonts::Load` from the editor.
Sandbox no longer warns about missing Montserrat. Play/Stop is `ICON_LC_PLAY` /
`ICON_LC_SQUARE_STOP`. Montserrat Regular/Bold/Italic stay under `assets/fonts/montserrat/`
because RmlUi still loads them for the Play-mode HUD — deleting the folder as originally
planned would empty the HUD. Live description: [editor.md](../editor/editor.md#look-and-feel).

---

### Phase 1 — The token layer and the style application — **done**

`EditorTheme` + two presets (`Ganymed` / `Coldwar`), `ApplyTheme` writes the full `ImGuiStyle`
(0 rounding, `FramePadding (6,3)`, `WindowMenuButtonPosition = None`, 1 px dock gutters).
Scattered literals absorbed; `SetDarkThemeColors` removed from the engine. **View → Reset Layout**
rebuilds the DockBuilder tree; **View → Theme** switches the accent. Live description:
[editor.md](../editor/editor.md#look-and-feel).

The spec that was executed (kept so later phases can cite the mapping):

**1.1 Write `EditorTheme.h/.cpp`** with the two presets from the tables above.

**1.2 `ApplyTheme` writes the full `ImGuiStyle`.** The metrics matter as much as the colours:

```cpp
style.WindowRounding = style.ChildRounding = style.FrameRounding =
style.PopupRounding  = style.TabRounding   = style.GrabRounding  =
style.ScrollbarRounding = 0.0f;                  // Cold War is square everywhere

style.WindowBorderSize = style.ChildBorderSize = 0.0f;
style.FrameBorderSize  = 0.0f;                   // separation comes from fills, not outlines
style.PopupBorderSize  = 1.0f;

style.FramePadding  = ImVec2(6, 3);              // 18px text + 6 = the measured 24px row
style.ItemSpacing   = ImVec2(6, 4);
style.ItemInnerSpacing = ImVec2(4, 4);
style.CellPadding   = ImVec2(6, 2);
style.IndentSpacing = 18.0f;
style.ScrollbarSize = 10.0f;
style.GrabMinSize   = 10.0f;

style.WindowMenuButtonPosition = ImGuiDir_None;  // kills the `▼` on every dock tab
style.TabBarBorderSize   = 0.0f;
style.TabBarOverlineSize = 0.0f;                 // Cold War's active tab is a bg change only
style.DockingSeparatorSize = 1.0f;               // 1px chrome-coloured gutters
style.SeparatorTextBorderSize = 1.0f;
```

Colour mapping worth calling out, because two of them invert ImGui's defaults:

| ImGui colour | Token | Note |
|---|---|---|
| `WindowBg`, `ChildBg` | `SurfaceBg` | |
| `MenuBarBg`, `TitleBg*` | `ChromeBg` | |
| `FrameBg` | `SurfaceSunken` | **Darker** than the window, not lighter. Inputs are recessed |
| `Header`, `HeaderHovered`, `HeaderActive` | `ChromeBg` / lighter / `Accent` | **Also darker** than the window — Cold War's inspector component headers are `#1A1A1A` on `#313131`. ImGui's default is the opposite |
| `Tab`, `TabDimmed` | `ChromeBg` | |
| `TabSelected`, `TabDimmedSelected` | `SurfaceBg` | Active tab = the panel it belongs to |
| `TabSelectedOverline`, `TabDimmedSelectedOverline` | fully transparent | |
| `Separator`, `Border`, `TableBorder*` | `Border` | |
| `ScrollbarBg` | `SurfaceBg` (opaque) | Cold War's track is invisible; only the grab shows |
| `ScrollbarGrab*` | `GrabBg` → lighter → `Accent` | |
| `CheckMark`, `SliderGrab*`, `DragDropTarget`, `ResizeGrip*`, `NavHighlight` | accent ramp | |
| `Text`, `TextDisabled`, `TextSelectedBg` | `TextPrimary`, `TextDisabled`, `Accent` @ 35 % | |
| `DockingEmptyBg` | `ChromeBg` | The void behind an empty dockspace |

**1.3 Retire the scattered literals** listed in the gap table, including the `3.0f` rounding inside
`EditorWidgets.cpp`'s curve canvas.

**1.4 Add `View → Reset Layout`.** This is not cosmetic and it will save hours: the DockBuilder
default layout in `EditorLayer::OnImGuiRender:236` only runs when there is no `imgui.ini` node, so
every layout change in phases 2–8 is **invisible** on a machine with an existing
`GanymedEditor/imgui.ini`. Either add the menu item (calls `DockBuilderRemoveNode` and rebuilds) or
bump a layout-version key in the ini and rebuild when it mismatches. Do this before phase 2, not
after debugging a change that "did not apply".

- **Files:** `EditorTheme.h/.cpp`; `EditorLayer.cpp/.h`, `EditorInspector.cpp`,
  `EditorWidgets.cpp`, `ContentBrowserPanel.cpp`; `ImGuiLayer.cpp/.h` (dropped
  `SetDarkThemeColors`). `SceneHierarchyPanel.cpp` had no literals.
- **Verify:** side-by-side against the reference screenshot with the `Coldwar` preset active — the
  ramp should match exactly. Then switch to `Ganymed` and confirm only the accent moved.
- **Docs:** `editor/editor.md` "Look and feel" — the token table, the two presets, the
  `Header`/`FrameBg` inversion and **why** (so nobody "fixes" it back toward ImGui defaults).

---

### Phase 2 — Chrome geometry: the toolbar stops being a dockable window

`EditorLayer::UI_Toolbar` currently opens a window docked into a DockBuilder node with
`NoTabBar | NoDockingOverMe` (`EditorLayer.cpp:242-252, 630-663`). That is why it is ~90 px tall,
user-resizable, and draggable out of the frame. No production editor lets you undock the main
toolbar.

Replace it with a **fixed-height child region inside the dockspace host window**, drawn between the
menu bar and `DockSpace()`:

```
Begin("DockSpace Demo", MenuBar | NoDocking | ...)
    BeginMenuBar / EndMenuBar          // becomes the title-bar menu in phase 9
    UI_Toolbar()                       // BeginChild, fixed Theme().ToolbarHeight (41px)
    DockSpace(id, ImVec2(0, -Theme().StatusBarHeight))   // reserve the strip for phase 7
    UI_StatusBar()                     // phase 7
End()
```

Then remove the `dockToolbar` split and the `##toolbar` `DockBuilderDockWindow` call from the default
layout. Also drop the `style.WindowMinSize.x = 430.0f` override around the dockspace
(`EditorLayer.cpp:229-264`) — it forces every panel to be at least 430 px wide, which fights the
narrow left dock Cold War uses.

**Toolbar content**, matching Cold War's grouping: mode tools (select / translate / rotate / scale,
bound to the existing Q/W/E/R state, **accent-filled when active**), 1 px `Border` separators between
groups, a centred `ICON_LC_PLAY + "Play"` in `Success` colour, and a right-aligned cluster
(settings, layout, screenshot). Use `EditorUI::IconButton(icon, tooltip, bool active)` from phase 3's
helper set — 24×24, transparent by default, `Accent` fill with `TextOnAccent` glyph when active.

Wiring the mode buttons to `m_GizmoType` is the one place this phase touches behaviour, and it
*fixes* something: the gizmo mode is currently invisible unless you remember which key you pressed.

- **Files:** `EditorLayer.cpp/.h`.
- **Verify:** toolbar is 41 px, cannot be dragged or resized; active tool shows an accent fill;
  Q/W/E/R and the buttons stay in sync.
- **Docs:** `editor/editor.md` — the Layout section's toolbar description is now wrong.

---

### Phase 3 — Panel furniture helpers

The reusable vocabulary every remaining phase consumes. Goes in `EditorWidgets.h/.cpp`, next to
`CurveEditor` / `DrawVec3Control`, and follows the same rule those already document: a custom widget
that participates in an edit gesture owns `ActiveId` for the whole gesture via one
`InvisibleButton`. The widgets here are chrome, not property editors, so most are exempt — but
`SearchField` is a text input and must not break `HandleShortcuts`'s `WantTextInput` gate.

| Helper | Draws |
|---|---|
| `PanelToolbarRow(float height)` / `EndPanelToolbarRow()` | `SurfaceBg` child of fixed height, 1 px `Border` rule along the bottom |
| `IconButton(icon, tooltip, bool active = false)` | 24×24 square, transparent → `AccentHover` on hover → `Accent` fill + `TextOnAccent` glyph when active |
| `ToolbarSeparator()` | 1 px vertical `Border` line with 4 px margins |
| `SearchField(id, char* buf, size_t n, const char* hint)` | `SurfaceSunken` frame, `ICON_LC_SEARCH` prefix in `TextDim`, hint text, `ICON_LC_X` clear button when non-empty. Returns true when the filter changed |
| `ColumnHeaderRow(std::initializer_list<ColumnSpec>)` | `SurfaceSunken` strip, `TextDim` 16 px labels, right-aligned icon columns |
| `OverflowMenuButton(id)` | The `•••` button — **only** call it where there is a real menu |
| `RowActionIcons(...)` | Right-aligned per-row icon cluster (eye / lock), `TextDim`, brightening on row hover |
| `StatusBarItem(icon, text, colour = TextPrimary)` | Icon + label pair with consistent spacing |

Also add `BeginPanel(name)` / `EndPanel()` wrapping `Begin`/`End` with `WindowPadding = (0,0)`, so
toolbar rows and column headers can reach the panel edges the way Cold War's do. The content region
inside then re-pushes normal padding.

- **Files:** `EditorWidgets.h/.cpp`.
- **Verify:** a throwaway call site per helper; `SearchField` focus does not eat Ctrl+Z (it should —
  `HandleShortcuts` returns early on `WantTextInput`; confirm that still holds).
- **Docs:** `editor/editor.md` — the "Adding an editor feature" table gains a "Panel furniture" row.

---

### Phase 4 — Scene Hierarchy → World Outliner

Cold War's outliner is the panel that most defines the look. Five changes, in order of impact:

1. **Toolbar row**: `+` create-entity dropdown, sort mode, filter dropdown, `SearchField`, view
   options. The search field is the only one with real work behind it (filter the tree by tag
   substring; a parent whose descendant matches must stay visible).
2. **Column-header row**: `Name` | eye | lock | link. Cold War's is a `#272727` strip with three
   right-aligned icon columns. The eye and lock columns need somewhere to store their state — either
   a new pair of scene singletons keyed by UUID, or two new tag components. **Recommendation:** an
   editor-side `std::unordered_set<UUID>` per flag, *not* a component, for v1. Visibility that does
   not serialize is honest about being an editor filter; a `HiddenComponent` implies it survives to
   the runtime and immediately raises "does the runtime respect it?".
3. **Per-row type icon** by dominant component (mesh / camera / light / audio / particle / prefab /
   empty), tinted like the content browser's asset tints.
4. **Link-coloured labels.** Cold War draws entity names in `#589FFD`. Reserve `TextPrimary` for
   plain entities and use `Link` for prefab instances — that maps the colour onto a fact Ganymed
   actually has (`PrefabInstanceComponent`) rather than colouring everything blue for decoration.
5. **Full-width selection fill.** `ImGuiTreeNodeFlags_SpanAvailWidth` plus `Accent` fill and
   `TextOnAccent` text for the selected row. The multi-select highlight documented in
   `editor.md` should use `Accent` at ~40 % alpha for non-primary selections, so primary vs.
   secondary stays readable — currently they are indistinguishable.

The hierarchy panel is 1642 lines and also hosts the inspector. Consider splitting the Properties
half into `PropertiesPanel.cpp` **as part of this phase or not at all** — it is a mechanical move,
but doing it later means doing phase 5's edits twice.

- **Files:** `Panels/SceneHierarchyPanel.cpp/.h` (+ possible split).
- **Verify:** filter a 100-entity scene; confirm ancestors of matches stay visible; selection fill
  spans the row; eye/lock toggles survive play/stop (they are editor state, so they must not be
  cleared by `RetargetPanels`).
- **Docs:** `editor/editor.md` Scene Hierarchy section.

---

### Phase 5 — Inspector component headers

Cold War's inspector is a stack of **full-width `#1A1A1A` rows** on a `#313131` panel: chevron, type
icon, name, then right-aligned eye + `•••`. Ganymed uses `ImGui::CollapsingHeader`, whose default is
a *lighter* fill.

The colour half is already done by phase 1's `Header` mapping. What remains:

- Draw the header manually inside `DrawComponent<T>` rather than via `CollapsingHeader`, so the icon
  and the right-aligned cluster fit. The chevron is `ICON_LC_CHEVRON_RIGHT` / `_DOWN` on the stored
  open state.
- The `•••` menu absorbs the existing remove-component menu, plus Copy/Paste Component if cheap.
- The eye toggle is a **per-component enable**, which Ganymed has no concept of. **Do not invent
  one.** Either omit the eye here, or draw it only for component types where a natural disable
  exists. An eye that does nothing is worse than a missing eye.
- `Add Component` becomes a full-width accent-outlined button at the bottom of the stack, as in
  Cold War, instead of the current inline popup trigger.

**The one thing not to break:** `DrawComponent<T>`'s commit boundary reads ImGui's active item before
and after the section body to turn a multi-frame drag into one undo command
(`editor.md` → "The commit boundary"). A hand-drawn header adds *items* to that section. The header's
chevron and `•••` must be submitted **outside** the before/after window, or clicking the chevron
mints a phantom `ComponentEditCommand`. Verify with `UndoDepth` after collapsing a section: it must
stay unchanged.

- **Files:** `Panels/SceneHierarchyPanel.cpp` (or the new `PropertiesPanel.cpp`),
  `EditorInspector.cpp`.
- **Verify:** collapse/expand every section with a selection and assert the undo depth does not
  move; a 3-entity multi-select drag still produces exactly one undo entry.
- **Docs:** `editor/editor.md` Properties section, and the commit-boundary paragraph gains the
  "header items sit outside the window" rule.

---

### Phase 6 — Content Browser → Asset Browser

The largest single panel job. Cold War's has four regions Ganymed lacks:

1. **Toolbar row**: `+` add, sort, import.
2. **Breadcrumb bar** (`#272727`): `ICON_LC_HOME` + clickable path segments, `←` / `→` history, `↑`
   parent. History is two `std::vector<path>` stacks; the existing root-escape check
   (`ContentBrowserPanel.cpp:66-70`) must gate every new navigation path, not just `<-`.
3. **Folder tree sidebar**, split from the grid. Cold War's is a fixed-width resizable pane. Use a
   two-column `BeginTable` with `ImGuiTableFlags_Resizable | BordersInnerV` — the table column
   sizing persists in `imgui.ini` for free, which a hand-rolled splitter does not.
4. **Footer**: `24 items` count on the left, grid/list view toggle on the right.

Also: `SearchField` in the toolbar, and **replace the legacy `ImGui::Columns` grid**
(`ContentBrowserPanel.cpp:83`) with `BeginTable`. `Columns` is deprecated, has no per-cell clipping,
and cannot do the hover/selection cell fill Cold War's grid shows.

A real risk worth naming: the panel calls `std::filesystem::directory_iterator` **every frame** on
the current directory. With the folder sidebar that becomes a recursive walk, and on a large
`assets/` tree it will show up in the frame. Cache the listing and invalidate it from
`AssetWatcher` (which already reports change events) rather than re-walking. This is the one place in
the milestone where a cosmetic change has a real performance consequence.

- **Files:** `Panels/ContentBrowserPanel.cpp/.h`.
- **Verify:** breadcrumb, `←`/`→` and the sidebar can none of them escape `assets/`; drag-drop to
  the viewport and to inspector slots still works (the `CONTENT_BROWSER_ITEM` payload must remain
  the relative path); item count matches the visible grid after the `.`/`.meta` filters.
- **Docs:** `editor/editor.md` Content Browser section, renamed if the panel is renamed.

---

### Phase 7 — Status bar

A 41 px `ChromeBg` strip below the dockspace. Left-to-centre: project/version chips with icons;
right: live counters. Cold War shows `WoT HEAT 1.3 · Engine 0.9.26.WIP · Release · Default ·
DirectX12 · mainline · game` then entity/draw counts and `FPS 120`.

Ganymed can fill nearly all of these from data it already has:

| Chip | Source |
|---|---|
| Project / scene name + dirty `*` | `m_EditorScenePath`, `m_UndoStack.IsDirtySinceSave()` — currently crammed into the menu bar (`EditorLayer.cpp:329-332`); move it here |
| Configuration | `GE_DEBUG` / `GE_RELEASE` / `GE_DIST` |
| Backend | `bgfx::getRendererName(bgfx::getRendererType())` |
| Entity count | `m_ActiveScene` registry size |
| Draw calls | `Renderer3D::GetStats().DrawCalls` |
| FPS | frame timestep — needs a smoothed average, not `1/ts`, or it is unreadable |
| Play state | `m_SceneState`, in `Success` when playing |

Engine version has no source today. Either add one constant or leave the chip out — do not print a
hard-coded string that will silently rot.

- **Files:** `EditorLayer.cpp/.h`.
- **Verify:** strip is a fixed 41 px and does not shrink when panels resize; FPS is stable; the
  dirty asterisk tracks undo position across an undo back to the saved state.
- **Docs:** `editor/editor.md`.

---

### Phase 8 — Viewport overlay bars

Two pieces:

**8.1 A viewport header row** (`PanelToolbarRow` inside the `Viewport` window, above the image) with
Cold War's dropdown set. Map to what exists; omit the rest:

| Cold War control | Ganymed |
|---|---|
| `Editor Camera` | exists (`EditorCamera`); the dropdown can switch to a scene camera preview |
| `Quality: Max` | no quality tiers — **omit** |
| `Selection: all` | no selection filters — omit |
| `Icons` toggle | no billboard gizmos yet — omit |
| `Lit` | a render-mode dropdown: Lit / Unlit / Wireframe. Wireframe is one bgfx state flag; the others need shader variants. Ship the dropdown only with modes that work |
| `Free Aspect: WxH` | trivially available from `m_ViewportSize` |
| `Visualizers` | the physics-debug toggles currently buried in Stats belong here |
| Gizmo-space toggle | `ImGuizmo::LOCAL` is hard-coded at `EditorLayer.cpp:517` — a Local/World toggle is a one-line behaviour win |

**8.2 A bottom-left readout** over the image: `X`/`Y`/`Z` of the selection in `AxisX/Y/Z` colours,
plus the counters strip. Drawn with `ImDrawList` after `ImGui::Image` so it costs no layout.

**The one correctness note:** inserting a header row moves the image's origin. Picking, the ImGuizmo
rect and `UIEngine::SetViewportOrigin` all derive from `m_ViewportBounds[0]`, which is read from
`GetCursorScreenPos()` *after* the row would be drawn (`EditorLayer.cpp:447-453`) — so they stay
correct automatically. That is worth verifying rather than assuming: click-select an entity near the
top edge of the viewport after the change.

- **Files:** `EditorLayer.cpp/.h`.
- **Verify:** hover-pick and click-select land on the right entity at the top and bottom edges;
  gizmo handles sit under the cursor; the RmlUi HUD still lines up during Play.
- **Docs:** `editor/editor.md` Viewport section.

---

### Phase 9 — Custom window chrome

Last, deliberately. This is roughly a quarter of the milestone's effort for one 40 px strip, and it
is where every platform bug in this plan lives. It is also the single loudest mismatch, so it is
worth doing — just not first, and not while anything else is in flight.

**9.1 Undecorated window.** `ApplicationSpecification` gains `CustomTitleBar` (default **false**, so
Sandbox and the runtime are unaffected); `WindowsWindow::Init` passes
`glfwWindowHint(GLFW_DECORATED, GLFW_FALSE)`. The borderless-fullscreen path already does exactly
this (`WindowsWindow.cpp:79`), so the pattern is established.

**9.2 Windows: keep a real non-client frame.** Two approaches, and the difference is not cosmetic:

- **Manual drag** — on a title-bar drag, `glfwSetWindowPos` by the mouse delta. ~40 lines, works
  everywhere. Loses Aero Snap, snap layouts, double-click-to-maximize, edge resize, and the drop
  shadow; and an undecorated maximized window **covers the taskbar**, the classic symptom.
- **Win32 subclass** (recommended) — `glfwGetWin32Window` (needs `GLFW_EXPOSE_NATIVE_WIN32`; bgfx
  already takes native handles so this is available), `SetWindowLongPtr(GWLP_WNDPROC)`, and handle
  `WM_NCCALCSIZE` (return a client area covering the frame, minus 1 px at the top so the maximized
  case does not eat the taskbar) plus `WM_NCHITTEST` (return `HTCAPTION` over the title strip where
  no ImGui item is hovered, `HTLEFT`/`HTTOPLEFT`/… in the 6 px border zone). The OS then provides
  snap, resize, shadow and double-click-maximize for free.

Ganymed is cross-platform, so the subclass lives in `Platform/Windows/` behind the same interface
Linux/macOS implement with the manual path. Linux under Wayland cannot do this at all — feature-flag
it and fall back to OS decoration rather than shipping a window that cannot be moved.

**9.3 The title bar itself**, drawn as the first thing in the dockspace host window: app icon; a
`ICON_LC_MENU + "Menu"` button opening the current File/Edit/View menus as a popup; the document tab;
flexible spacer; `ICON_LC_MINUS` / `ICON_LC_SQUARE` / `ICON_LC_X` window buttons (close hovers red).

**The document-tab question.** Cold War's title bar tabs are *open documents* —
`01_demo_conquest`, `player_controller_state_machine`, `network_player`. Ganymed has exactly one
scene open at a time and no other document types, so a tab strip there would be a strip of one.
**Recommendation:** draw a single tab showing the scene name + dirty asterisk, styled identically to
Cold War's active tab. It looks right, it is honest, and it becomes a real tab bar the day
multi-document editing exists. Do not build fake tabs.

**Hit-test exclusion** is the fiddly part: the strip is draggable *except* where an ImGui item is
hovered. Track the title-bar rect and test `ImGui::IsAnyItemHovered()` within it; feed the result to
`WM_NCHITTEST`. Get this wrong in either direction and you get a title bar that cannot be dragged, or
menu buttons that move the window instead of opening.

- **Files:** `Platform/Windows/WindowsWindow.cpp/.h`, `Linux/`, `macOS/`, `main/Application.h`,
  `EditorLayer.cpp`, new `GanymedEditor/source/EditorTitleBar.cpp`.
- **Verify:** drag, double-click-maximize, Win+Arrow snap, edge resize on all four sides and
  corners, **maximize does not cover the taskbar**, restore/minimize round-trip, moving between
  monitors with different DPI, alt-tab. Then confirm Sandbox and GanymedRuntime still get a normal
  decorated window.
- **Docs:** `engine/platform.md` (the undecorated-window path and the Win32 hit-test),
  `editor/editor.md` (the title bar and its menu).

---

## Not doing, and why

| | |
|---|---|
| Log / Profiler / Statistics / Systems / Command History / Validation Log panels | Scope decision. Several need engine-side data sources that do not exist; the rest would be empty windows that make the editor look *less* finished, not more |
| Per-component visibility (the eye in the inspector) | No engine concept. Phase 5 omits it rather than inventing one |
| Per-tab dock label colours (dim when unselected) | Not themable in ImGui 1.91.9b without patching `TabItemEx`. Accepted deviation — the background change alone carries the signal |
| A runtime theme editor / user-editable style file | One consumer, no request. `EditorTheme` is a struct, not a system |
| Multi-document tabs | Phase 9 draws one honest tab. Real multi-document editing is a separate milestone |
| Multi-viewport (ImGui platform windows) | Already deferred for a real reason — it needs one bgfx framebuffer per OS window (`ImGuiLayer.cpp:28-31`). Unrelated to this milestone |

## Cost, honestly

| Phase | Estimate |
|---|---|
| 0 — fonts + icons | **done** |
| 1 — token layer | **done** |
| 2 — toolbar geometry | 0.5 day |
| 3 — furniture helpers | 1 day |
| 4 — outliner | 1.5 days |
| 5 — inspector headers | 1 day |
| 6 — asset browser | 1.5 days |
| 7 — status bar | 0.5 day |
| 8 — viewport bars | 1 day |
| 9 — custom chrome | 2–3 days |
| | **~11–13 days** |

**Phases 0–2 are ~2 days and close most of the perceived gap** — the neutral ramp, zero rounding, a
grotesque at the right density, icons, and a real toolbar. If the milestone has to be cut, cut from
the back: 9 is the most expensive, 6 is the second, and neither changes the first impression as much
as phase 1 does.

## Two things this plan cannot fix

- **Content density.** A good part of why the Cold War screenshot looks like a working tool is that
  it *is* one: 3620 entities in the outliner, a real terrain in the viewport, 24 asset folders. An
  empty Ganymed scene will still look sparse after every phase here. Do not chase that with UI
  changes — the fix is a demo scene, not a theme.
- **`imgui.ini`.** Every layout change in phases 2–8 is invisible until the existing
  `GanymedEditor/imgui.ini` is discarded. Phase 1.4's Reset Layout item exists specifically so this
  does not get diagnosed as a broken change three times.

## Open questions

1. **Font family** — **Inter** (done, phase 0).
2. **Icon set** — **Lucide** (done, phase 0).
3. **Accent** — **`#B182ED`** (lilac fill, dark glyph, keeps Cold War's treatment). `#7B43C2` with
   light glyphs was rejected: it inverts the selected-row treatment.
4. **Outliner eye/lock storage** — editor-side `unordered_set<UUID>` (recommended: honest about
   being an editor filter) or real components that serialize?
5. **Properties split** — move the inspector out of `SceneHierarchyPanel.cpp` (1642 lines) during
   phase 4, or leave it and accept touching the same file in phases 4 and 5?

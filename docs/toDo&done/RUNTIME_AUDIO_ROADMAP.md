# GanymedEngine — Standalone Runtime + Audio Roadmap

Status: **Phases 1–4 executed; Phase 5 planned.** Written 2026-08-12, against the post-animation
engine (branch point: the skeletal-animation milestone, complete). Follows the format of
[`ANIMATION_ROADMAP.md`](ANIMATION_ROADMAP.md): each phase carries goal, steps, decisions with
rationale, risks, and a verification table; execution notes get appended as phases run. Read the
decision notes even if you skip the code sketches.

This is the plan of record for the sixth milestone: a **standalone game runtime application**
(`GanymedRuntime` — boots a `.ganymede` scene straight into play mode, renders fullscreen to the
backbuffer with RmlUi on top and no ImGui anywhere) and an **audio subsystem** (miniaudio-backed
engine core, `AudioSourceComponent`/`AudioListenerComponent`/`AudioSystem`, Lua bindings). After
this milestone the engine can ship a small game: something a person who is not the engine's
author can download, run, play, and hear.

## Decisions of record

Settled up front, before any code:

1. **The plan lives here**, indexed from `docs/README.md`, matching the previous milestones.
2. **Audio backend is miniaudio**, vendored as a committed single header at
   `GanymedEngine/extern/miniaudio/miniaudio.h` (cgltf precedent — no submodule). Its types
   never appear in a public engine header (design principle 6, "engine types firewalled";
   the `bgfx`-types-in-`Buffer.h` leak is the cautionary precedent, `PhysicsScene`'s pimpl is
   the model).
3. **Runtime ships before audio.** Neither depends on the other — audio's full lifecycle
   (play/stop, scene copy, voices) is exercised by editor play mode alone, and the runtime is
   audio-free until Phase 5. Runtime goes first anyway: (a) the render-to-backbuffer path is
   this milestone's highest-risk unknown (two never-exercised code paths meet: retargeted post
   passes and RmlUi's null-target branch) — fail fast while the tree is quiet; (b) the finished
   runtime is the milestone's integration proof — the exit criterion is *the standalone app
   playing sound under a HUD*, so it must exist before Phase 5; (c) the engine-side runtime
   enablers (Phase 1) are small and independent, so audio work can start the moment they land.
4. **Fullscreen means backbuffer-filling, borderless-optional.** The scene renders to the
   backbuffer at window size; a config flag gives borderless fullscreen via GLFW video-mode
   sizing. Exclusive fullscreen is out (see "Not doing").
5. **Two new docs are proposed by this plan**: `docs/engine/audio.md` and
   `docs/runtime/runtime.md`, both indexed from `docs/README.md`, both added to `AGENTS.md`'s
   doc-mapping table (which requires proposing new docs — this plan is that proposal).
   `runtime.md` gets its own `docs/runtime/` folder mirroring `docs/editor/`, because the
   `AGENTS.md` table maps *directories* to docs and `GanymedRuntime/` is a new top-level app
   directory, same shape as `GanymedEditor/`.

## Where the engine is today (facts this plan is built on)

Verified against the tree at planning time; re-verify anything load-bearing before executing a
later phase.

- **Boot**: `EntryPoint.h`'s `main` → `Log::Init` → `CreateApplication()` → `Run()`. Apps
  subclass `Application`; `GanymedEditorApp.cpp` and `SandboxApp.cpp` are ~15-line shells. The
  `Application` ctor (`GanymedEngine/source/GanymedE/main/Application.cpp:20-44`) creates the
  window (`WindowProps` parameterizes **title only**; 1600×900 hard default on Windows), then
  `Renderer::Init` → `ScriptEngine::Init` → `UIEngine::Init(w, h)`, then **unconditionally**
  news an `ImGuiLayer` and pushes it as an overlay. `Run()` (~L161-168) unconditionally
  brackets every frame with `ImGuiLayer::Begin()/End()`. There is no opt-out; ImGui installs
  its own GLFW callbacks and blocks events (`m_BlockEvents = true` default). The engine has
  **zero editor `#ifdef`s** — `ImGuiLayer` is referenced only from `Application`, so the
  opt-out is a two-site change, not surgery.
- **Shutdown order** (Application dtor): `UIEngine::Shutdown` → `ScriptEngine::Shutdown` →
  `Renderer::Shutdown` in the dtor **body**, then members destroy in reverse — LayerStack
  (per-layer `OnDetach`) → Window → `~BgfxContext` (`SetGpuAlive(false)` + `bgfx::shutdown`).
  Layers release GPU resources while bgfx is alive; anything a layer calls during `OnDetach`
  against an already-shut-down subsystem must no-op. **This ordering shapes the AudioEngine
  API** (see 3.2).
- **What a runtime layer must replicate from EditorLayer**: `AssetManager::Init()` *before any
  scene deserialize* (the serializer resolves handles through it); `SceneRenderer(w, h)`;
  `UIEngine::SetTarget`; scene load; `OnViewportResize` → `OnRuntimeStart` (physics scene
  created, Lua scripts instantiated) → `UIEngine::LoadDocument` (path hard-coded to
  `assets/ui/hud.rml` in the editor). `OnDetach`: `AssetManager::Shutdown()` — which **saves
  the registry** and clears GPU caches while the GPU is alive.
- **Camera**: `Scene::OnUpdateRuntime(ts, EditorCamera*)` — CameraSystem resolves the first
  `Primary` `CameraComponent` into `RenderContext::MainCamera`; RenderSystem falls back to
  `RenderContext::EditorViewCamera`; with neither it renders **nothing, silently black**.
  `Scene::GetPrimaryCameraEntity()` exists.
- **Render output**: `SceneRenderer` runs entirely offscreen — HDR FB (RGBA16F + RED_INTEGER
  picking + depth) on views 5/6, bloom 7–22, tonemap 24 (into the tonemap FB when FXAA is on,
  else the composite FB), FXAA 25 into the composite FB. The editor displays the composite FB
  via `ImGui::Image`. `RenderPass::Composite = 26` is **declared but never used**. The
  backbuffer receives only the BgfxContext view-0 touch, ImGui (200), and RmlUi (28) **iff**
  its target is null — a branch already implemented in `RmlUiRendererBgfx::BeginFrame`
  (~L101-104) but never exercised.
- **Editor-ism in play mode**: `RenderSystem::DrawPhysicsDebugOrGizmos`
  (`Scene/Systems/RenderSystem.cpp` ~L159-173) falls through to `DrawColliderGizmos()`
  **unconditionally** whenever Jolt debug draw is off — a shipped game draws authored collider
  wireframes over everything.
- **Events/input**: `Input::` polls GLFW directly, window-relative, no gating —
  fullscreen-ready as-is. `UIEngine::SetViewportOrigin` defaults to (0,0) — correct for
  fullscreen without ever being called. The window layer already `bgfx::reset`s the swapchain
  on size change.
- **Build**: root `premake5.lua` — app include list (~L94-96), `IncludeDir` table (~L61-80).
  `Sandbox/premake5.lua` is the minimal app template, including the three per-OS filters whose
  link lists **must** be reproduced in dependency order (static libs don't propagate links off
  MSVC). `debugdir "%{prj.location}"`; every app resolves `assets/` from CWD. Shader
  distribution: `scripts/compile_shaders.bat` (~L26) and `.sh` (~L46-48) carry hard-coded
  `TARGETS` lists. The engine already compiles with `/bigobj`; pch `gepch.h` is a mandatory
  first include; the engine project globs `source/**`, so new files need only a premake regen.
- **ECS checklists** (established by the animation milestone — reused, not re-derived): new
  component = `Components.h` + `ComponentList` (`ECS/ComponentTraits.h`) + both
  `SceneSerializer` sides + inspector `DrawComponent` + Add Component menu; untracked unless
  change-tracking buys something; runtime-only state on a component needs a `Scene::Copy`
  fixup sweep. New system = CRTP `ECS::System<Impl>` with declared views + a registration slot
  in the `Scene` ctor (**registration order is execution order**; currently Physics →
  NativeScript → LuaScript → Animation → Transform → Camera → Render — seven systems). Lua =
  single `ScriptBindings.cpp`, by-value, methods on the Entity usertype, no-op on missing
  component (`HasRigidBody`/`HasAnimator` precedent), hand-mirrored in
  `GanymedEditor/scripts-src/types/ganymed.d.ts`.
- **Asset layer**: `AssetType` enum is ordinal-persisted — **append only**.
  `AssetTypeFromExtension`/`AssetTypeToString` are the single extension registry. `GetAsset<T>`
  has header specialization declarations + a static_assert primary. **Script deliberately has
  no `GetAsset`** — the manager resolves handle→path and the consumer loads itself; that is
  the template for audio.

---

## Phase 1 — Engine enablers for a second front-end

**Goal:** the engine can host an ImGui-free application that renders the scene post stack and
RmlUi straight to the backbuffer — proven from Sandbox with a temporary probe, with the editor
pixel-unchanged. Everything here is engine-side; no new project yet.

### 1.1 `Application` learns to run without ImGui

**Decision: a specification struct, not a subclass hook.** Extend the `Application` ctor to
take an `ApplicationSpecification { std::string Name; uint32_t Width = 1600, Height = 900;
bool EnableImGui = true; }`, keeping the old `(const std::string&)` ctor delegating so the
editor and Sandbox compile untouched. Production norm: engines separate "host services" from
"front-end chrome" via configuration, not inheritance; a virtual `UsesImGui()` cannot work
here anyway, because the base ctor pushes the overlay before the derived vtable exists.

- Gate `new ImGuiLayer` + `PushOverlay` on the spec; `m_ImGuiLayer = nullptr` otherwise.
- Guard the `Begin()/OnImGuiRender/End()` bracket in `Run()` on `m_ImGuiLayer`. Layers'
  `OnImGuiRender` simply never runs — no layer-side changes needed. **Grep every use of
  `m_ImGuiLayer`, not just `Run()`** — an unguarded deref is the classic slip here.
- `WindowProps` gains width/height (today title-only); the ctor passes the spec's through.
  Add `bool Fullscreen` to `WindowProps` now (borderless: `glfwGetVideoMode` sizing +
  undecorated), implemented on Windows; Linux/macOS honor it best-effort, verified when
  someone runs there (the established per-platform posture).

Side effect worth stating: with no ImGuiLayer there is no event-blocking overlay — every
event reaches the game layers raw, which is exactly what the runtime wants and what `Input::`
already assumes.

### 1.2 `SceneRenderer` backbuffer output — retarget, don't blit

**Decision: Option A, retarget the final post pass.** `SceneRenderer::SetOutputToBackbuffer(bool)`
(default false). When set, `EndFrame`'s **final** pass — FXAA at view 25 when FXAA is enabled,
tonemap at view 24 when not; the branch must handle both — does
`bgfx::setViewFrameBuffer(view, BGFX_INVALID_HANDLE)` + `setViewRect(view, 0, 0, windowW,
windowH)` instead of binding the tonemap/composite FBs. `UIEngine::SetTarget(nullptr)` then
routes RmlUi to the backbuffer at view 28 (the already-implemented, never-exercised branch).
View order stays monotonic: 0 (context touch) < 24/25 (final post) < 28 (UI).

Why not Option B (keep the composite FB, add a present/blit pass): the production norm *is*
Option B — Unity/Unreal always end on a dedicated present/upscale pass, because they need
resolution scaling, HDR-display output, and platform present semantics. Ganymed diverges
deliberately: v1 needs none of those, and Option B costs real work for nothing —
`RenderPass::UI = 28` sorts after `Composite = 26`, so presenting a UI-composited image needs
a *new* view at 29–31 (`EnvironmentBake` starts at 32); and the existing `vs_Blit.sc` expects
`a_texcoord0` while the PostProcess fullscreen quad supplies only `a_Position` Float2, so a
present shader would have to derive UV from position like `vs_FXAA`/`vs_Tonemap` — a new
shader, a new program, a new flip-parity surface. Option A: zero new shaders, one bool, and it
exercises the UI backbuffer path we ship anyway. The escape hatch (a real present pass, when
resolution scaling arrives) is named and deferred.

Consequences to encode, not discover:

- `GetFinalImageRendererID()` is meaningless in backbuffer mode — assert or warn if called.
- Resize in backbuffer mode: `SetViewportSize` still recreates the intermediate FBs (HDR,
  bloom chain, tonemap) at the new size; the final rect tracks the window. The editor's
  composite-FB-is-a-new-object-after-resize hazard **disappears** for the runtime because the
  UI target is null — one fewer re-set to forget.
- **Flip-parity risk, noted and deferred**: `vs_Tonemap`/`vs_FXAA` carry the GLSL V-flip
  branch written for offscreen targets (flip-parity rule in `rendering.md`); on GL backends
  bgfx flips offscreen vs backbuffer, so the same pass retargeted at the backbuffer may render
  upside-down there. Primary platform is D3D (unaffected). Treat exactly like the
  MaxBones/minimal-GL caveat in the animation milestone: a documented compile-away, not
  engineering. Record the expected failure mode in `rendering.md` so whoever hits it on GL
  knows it's known.
- The final pass must read the **current window size**, not a cached FB size — the window
  layer `bgfx::reset`s per-frame on size change.

### 1.3 Gate the collider-gizmo fall-through

Runtime-visible, so it's in this milestone: the `else DrawColliderGizmos()` branch in
`RenderSystem::DrawPhysicsDebugOrGizmos` gains a gate — `bool ShowColliderGizmos = false` on
the physics debug-draw settings already read by that function (the `PhysicsSettings` scene
singleton). The **editor** sets it true (on scene attach/new/open), preserving today's editor
behavior exactly; default-false means any non-editor host is clean. In-phase check: confirm
scene singletons survive `Scene::Copy` so the editor's play-mode copy keeps drawing gizmos as
today — if they don't copy, the editor re-sets it on play, and that finding goes in the
execution notes.

### 1.4 No-camera policy

The runtime path passes `nullptr` for the fallback camera (the parameter stays — it's the
editor's). When `RenderContext::MainCamera` is null and no fallback exists: clear color plus a
**throttled** error log (once per ~5 s, not per frame — loud, not log-flooding), living in
RenderSystem's else-branch. `RenderContext::EditorViewCamera` is now a misnomer ("fallback
view camera") — **flagged as debt, not renamed** in this milestone; the rename ripples through
docs and editor for zero behavior.

### 1.5 AssetManager registry-save toggle

`AssetManager::Init(bool saveRegistryOnShutdown = true)`; `Shutdown()` skips the registry
write when false. A shipped game must not write into its install directory on exit (fails
outright under Program Files), and the fix is a two-line toggle now versus a support mystery
later. Editor/Sandbox behavior unchanged; the runtime passes false in Phase 2.

### Phase 1 risks

- The ImGui opt-out touches the one file every app runs through — the `m_ImGuiLayer` grep is
  mandatory, not advisory.
- The RmlUi null-target branch is implemented-but-untested code; budget for it being subtly
  wrong (scissor/viewport state, view rect).

### Phase 1 verification

Temporary probe in Sandbox (it exists for exactly this; probe removed afterwards): a spec with
`EnableImGui = false`, a minimal layer that loads a scene, starts runtime, calls
`SetOutputToBackbuffer(true)` + `SetTarget(nullptr)`, loads an `.rml` document.

| Check | Evidence |
|---|---|
| Editor unchanged | Build + full editor smoke: viewport image, play/stop, gizmos in edit mode, F1 stats — all as before; `SetOutputToBackbuffer` never called there |
| Backbuffer scene | Sandbox probe shows the tonemapped scene filling the window, with and without FXAA (both final-view branches) |
| RmlUi on backbuffer | `.rml` doc visible over the scene; log the renderer's target-null branch being taken |
| No ImGui | bgfx stats (F1 — bgfx debug text, works without ImGui) show no view-200 activity; no ImGui GLFW callbacks installed |
| Resize | Drag-resize the Sandbox window: scene + UI track, intermediate FBs recreated at the new size (log dimensions), no stretching |
| Gizmo gate | Sandbox play mode with a collider: no wireframes; editor play mode: wireframes as today |
| No camera | A scene without a primary camera: clear color + throttled error (count log lines over 30 s — expect ~6, not ~1800) |
| Registry toggle | Sandbox probe with `saveRegistryOnShutdown = false`: `AssetRegistry.gr` mtime unchanged after exit |

### Phase 1 execution notes

**`WindowProps` already carried `Width`/`Height`.** The plan's "title only, 1600×900 hard default"
was stale — it parameterizes both, defaulted from `DEFAULT_WINDOW_WIDTH/HEIGHT` in `Core.h` (1600×900
on Windows, 640×480 elsewhere). Only `Fullscreen` was new, so 1.1 was smaller than budgeted.

**`GetImGuiLayer()` was left unguarded at its one call site, deliberately.** The grep found exactly
one use outside `Application`: `EditorLayer::OnUpdate`'s `BlockEvents` call. Guarding it would be
defending against a configuration the editor cannot have — it *is* the ImGui front-end. The null
possibility is documented on the accessor instead.

**The retargeted final pass takes its rect from `SceneRenderer`'s own size, not a window query.**
The plan said "read the current window size". In backbuffer mode the host feeds `SetViewportSize`
the window size on every `WindowResizeEvent`, so the two are identical, and reading the member avoids
pulling `Application`/`Window` into a renderer TU. The contract ("in backbuffer mode, feed it the
window size") is stated on `SetOutputToBackbuffer`. The one-frame mismatch the plan worried about is
real but pre-existing and shared with ImGui: `bgfx::reset` runs in `Window::OnUpdate` *after* the
frame was submitted, so the frame built at the old size is presented into the new-size backbuffer.
Transient, self-correcting, not worth engineering around.

**Latent bug found in the code 1.2 touches, and fixed.** The tonemap *target* was chosen on
`m_Settings.FXAAEnabled` while the FXAA *pass* ran on `FXAAEnabled && m_FXAAShader`. With FXAA
enabled and its shader failing to load, tonemap wrote the FXAA input and nothing ever wrote the
composite target — a black viewport with no error. Both decisions now read one `fxaaActive`
predicate. This mattered here because "which pass is final" is exactly what backbuffer mode keys off.

**In-phase finding the plan asked for: `Scene::Copy` does *not* copy singletons.** It constructs a
fresh `Scene`, whose constructor `SetSingleton`s defaults, then copies entities and components only
(`Scene.cpp`, `Copy`). So the play-mode scene starts every run with engine defaults. This is already
load-bearing today — it is why `EditorLayer` re-copies `DebugDraw` onto the active scene *every play
frame* rather than once on play. `ShowColliderGizmos = true` was therefore added to that same
per-frame write, needing no new call site. Recorded in `scene.md` because it is the kind of fact that
silently breaks the next person's "set it once on scene open".

**`ShowColliderGizmos` went on `PhysicsSettings`, not on `PhysicsDebugDrawSettings`.** The latter is
the parameter type handed to `PhysicsScene::DebugDraw` — Jolt draw options. Authored-collider
wireframes are not a Jolt concept, and putting a flag there that the callee ignores would be a small
lie for the sake of riding the editor's existing struct copy. One extra line in the editor was the
better trade. No Stats-panel checkbox was added: it would be a knob nobody turns, and edit mode draws
gizmos unconditionally regardless.

**The no-camera throttle is timestep-driven, not clock-driven.** `RenderSystem::OnUpdate` already
receives `Timestep` (it was being discarded with `(void)ts`), so the throttle accumulates it rather
than reaching for `std::chrono` or `glfwGetTime`. The counter is primed *above* the interval so the
first cameraless frame reports immediately instead of after five seconds of silence.

**One permanent logging addition beyond the plan:** `RmlUiRendererBgfx::BeginFrame` logs once per
change of target kind ("compositing into the backbuffer" / "an offscreen target"). The plan asked for
this as probe instrumentation; it is worth keeping, because a UI composited into the wrong target is
*invisible* rather than visibly wrong, which makes "which branch ran" the first thing you want from
a log. Tri-state member so it never repeats and never spams on editor resizes.

#### Verification evidence

Probe: a temporary `RuntimeProbe` layer in Sandbox with `EnableImGui = false`, a procedurally built
scene (sky light, directional light, an HDR sprite at colour 2.4, a box collider, a primary
perspective camera — no asset-registry dependency), `SetOutputToBackbuffer(true)` +
`SetTarget(nullptr)`, and `assets/ui/hud.rml`. A wall-clock script drove the run and
`bgfx::requestScreenShot(BGFX_INVALID_HANDLE, ...)` captured the **backbuffer** to TGA at each step,
so the visual claims below are pixels, not inference. Probe and its copied `ui/`+`fonts/` assets were
removed afterwards; x64 Debug, D3D11.

| Check | Evidence |
|---|---|
| Editor unchanged | Whole solution builds; editor boots clean (log: ImGui shader loaded, 7 registry assets, `save-on-shutdown on`, IBL baked, **zero** warnings/errors), panels dock, viewport shows the studio-HDRI scene. `SetOutputToBackbuffer` never called there |
| Backbuffer scene | Screenshot 1 (1600×900, FXAA on, view 25 final) and screenshot 3 (1024×640, FXAA off, **view 24** final) both show the tonemapped, bloomed scene filling the window. Both final-view branches exercised |
| RmlUi on backbuffer | HUD health bar + score visible over the scene in all four screenshots; log: `RmlUi: compositing into the backbuffer (1600x900)` — the null-target branch, taken for the first time ever |
| No ImGui | Log: `ImGuiLayer absent`, and no `Loaded shader 'ImGui'` line at all — the ImGui bgfx backend never initialized, so nothing could reach view 200 |
| Resize | Scripted `glfwSetWindowSize(1024, 640)` → log `resize event 1024x640; SceneRenderer now 1024x640`; bgfx recreated the HDR/ID/depth/tonemap/composite targets at 1024×640 and the bloom chain at 512×320 → 16×10. Screenshot 2 is 1024×640 with the quad still square — no stretching |
| Gizmo gate | Screenshots 1–3: no wireframe on the collider'd entity. Screenshot 4, after writing `ShowColliderGizmos = true` — the *same singleton write the editor makes each play frame* — shows the green wire box. Gate verified in both directions |
| No camera | `--no-camera` run: **7** error lines over 31 s across **7228** frames (plan predicted ~6 vs ~1800), clear colour only, clean exit, no crash |
| Registry toggle | Default run leaves no `Sandbox/assets/AssetRegistry.gr`; the same binary with save-on-shutdown enabled creates it. Editor log still reports `save-on-shutdown on` |
| Fullscreen (1.1 extra) | `--fullscreen`: log `Borderless fullscreen: 1920x1080 at (0, 0)`, window reports 1920×1080, screenshot 1 is 1920×1080 with correct aspect |

**Not verified by me, needs an interactive pass:** editor play/stop cycling, F1 stats overlay, and
collider gizmos inside editor play mode. The GUI cannot be driven from here; the gizmo path was
verified through the identical singleton write instead, and play/stop touches no code changed in this
phase. Worth ten seconds of clicking before Phase 2.

**Build/tooling note:** the probe added source files, so `premake5 vs2022` was re-run; the tree was
restored and regenerated afterwards. No premake *script* changes were needed in Phase 1.

---

## Phase 2 — `GanymedRuntime`: the standalone app

**Goal:** `GanymedRuntime.exe` boots, loads a config, deserializes a `.ganymede` scene, enters
play mode, renders fullscreen-to-backbuffer with the HUD on top, resizes, and exits cleanly.
No audio yet.

### 2.1 Project + build plumbing

- `GanymedRuntime/premake5.lua`: copy `Sandbox/premake5.lua` wholesale — **including all three
  per-OS filters with their full ordered link lists** (GNU ld walks archives once; the lists
  are load-bearing off MSVC; copy, don't retype) — then add the editor's windows-filter
  pattern for `WindowedApp`/`entrypoint "mainCRTStartup"` (no console window in Dist; keep the
  console in Debug the way the editor does) and, optionally, a `resources/*.rc` for an icon.
- Root `premake5.lua`: `include "GanymedRuntime"`.
- `scripts/compile_shaders.bat` and `scripts/compile_shaders.sh`: third `TARGETS` path.
- `.gitignore`: two explicit per-path lines for `GanymedRuntime/assets/AssetRegistry.gr` and
  `GanymedRuntime/assets/.assets` (the existing entries are per-path, not globs; compiled
  shaders are already glob-covered).
- `GanymedRuntime/assets/` tree: compiled shaders (via the script), `fonts/montserrat/*.ttf`
  (RmlUi hard-requires them; missing fonts = silently empty UI), `ui/`, `scenes/`, plus the
  models/environments the demo scene references. **v1 posture, stated**: runtime content is a
  copied snapshot of editor-authored assets; sharing/packing is a non-goal (see "Not doing").
  Risk to answer in-phase with evidence: a fresh clone has no `AssetRegistry.gr` (gitignored)
  — confirm scene deserialization rebuilds registry entries from the paths scenes carry; if
  handles-without-registry fail, the fallback is committing the runtime's registry, and that
  finding goes in the execution notes.
- `docs/README.md` applications table gains the fourth project row.

### 2.2 Runtime config

**Decision: a small YAML config, `GanymedRuntime/assets/runtime.yaml`, with a command-line
override for the scene.** Fields: `StartScene`, `UIDocument` (empty = no HUD load), `Title`,
`Width`, `Height`, `Fullscreen`. Production norm: a project-settings asset with a build scene
list (Unity Build Settings, Unreal `DefaultGame.ini`) — Ganymed's divergence is scale-honest:
one scene, one document, six keys, parsed with the yaml-cpp already linked everywhere.
Hardcoding `assets/ui/hud.rml` (the editor's habit) is exactly the editor-ism this app exists
to shed; a CLI-only approach leaves window/title/fullscreen homeless. Missing file = defaults
+ warning, so the exe still boots to *something* diagnosable.

### 2.3 `RuntimeLayer`

In `GanymedRuntime/source/`, beside a ~15-line `RuntimeApp.cpp` shell. `OnAttach`, in order —
each line is a lesson EditorLayer already learned:

1. `AssetManager::Init(/*writableRegistry=*/false)` — before any deserialize. (Renamed in
   execution from the plan's `saveRegistryOnShutdown`; see the Phase 2 notes.)
2. `SceneRenderer(windowW, windowH)`; `SetOutputToBackbuffer(true)`;
   `UIEngine::SetTarget(nullptr)`; `UIEngine::SetViewport(w, h)` (origin stays (0,0) —
   fullscreen).
3. Deserialize `StartScene` **directly into the play scene** — no `Scene::Copy`. The copy
   exists to preserve the editor's edit-time scene across play/stop; the runtime has no
   edit-time scene and no stop. `OnViewportResize(w, h)` → `OnRuntimeStart()`.
4. `UIEngine::LoadDocument(config.UIDocument)` if set.

`OnUpdate`: `Scene::OnUpdateRuntime(ts, nullptr)` — no editor-camera fallback (Phase 1
policy). `OnEvent`: forward to `UIEngine::OnEvent` first, bail if handled (the editor pattern
minus the hover/focus gating); handle `WindowResizeEvent` → `SceneRenderer::SetViewportSize`,
`Scene::OnViewportResize`, `UIEngine::SetViewport` (no `SetTarget` re-set needed — the target
is null). Minimize-to-zero sizes are already swallowed by `Application::OnWindowResize`.
Esc-to-quit in v1: yes (`Application::Close()`), because a chromeless fullscreen window
otherwise requires Alt+F4 knowledge; documented as a placeholder for a real pause menu (out
of scope). `OnDetach`: `Scene::OnRuntimeStop()` → scene reset → `AssetManager::Shutdown()` —
GPU caches clear while bgfx is alive (LayerStack destroys before Window; the existing order
guarantees it).

### 2.4 Docs

`docs/runtime/runtime.md` (new — proposed in Decisions of record): boot sequence, config
schema, the backbuffer render mode and its divergences from the editor path (no picking
consumer, no composite-FB display, the flip-parity caveat), the assets-snapshot posture.
`docs/README.md`: doc map + applications table. `AGENTS.md`: `GanymedRuntime/` →
`docs/runtime/runtime.md` row. **In the same docs pass: fix the stale `architecture.md`
limitations section** — it misstates scripting (says C++-only; Lua landed two milestones ago)
and carries the bgfx-migration shadow-regression warning the animation milestone's cascade
measurements contradict. This is pre-existing debt, and this milestone changes
architecture.md's app-hosting story anyway, so the section gets rewritten once, not twice.

### Phase 2 risks

- Order-of-init bugs (AssetManager after deserialize, UIEngine target after first frame) fail
  as silent black screens — the boot-log table below is the antidote; make each step log.
- The premake per-OS link lists rot silently on MSVC — copy, don't retype.
- RmlUi fonts missing = silent empty UI; the assets checklist covers it.

### Phase 2 verification

| Check | Evidence |
|---|---|
| Boot log table | One line per boot step: window WxH/fullscreen, config parsed (echo values), registry entries loaded, scene path + entity count, primary camera found, OnRuntimeStart OK, UI doc loaded. Screenshot of the first frame |
| Play mode is real | A scene with physics (falling body) + a Lua script (logs OnCreate/OnUpdate) — both run standalone, evidence in the log |
| HUD | The RmlUi doc renders over the scene; a data binding or button reacts to input (proves the event-forwarding chain) |
| Resize + fullscreen | Windowed drag-resize: no stretch, log new FB sizes. `Fullscreen: true`: fills the monitor, correct aspect |
| No-camera scene | Boots to clear color + throttled error, does not crash |
| Clean exit | Esc and window-close both: no asserts, no bgfx leak report in the shutdown log, `AssetRegistry.gr` untouched |
| Fresh-clone drill | Delete `GanymedRuntime/assets/AssetRegistry.gr` + `.assets` + compiled shaders → run scripts → boot: works (the registry-rebuild question answered with evidence) |
| Editor regression | Full editor smoke after all premake/build changes |

### Phase 2 execution notes

**The registry question the plan asked to answer in-phase: scenes are *not* self-describing, and the
runtime's `AssetRegistry.gr` is committed.** `.ganymede` stores bare handles; `AssetManager` resolves
handle→path through the registry and nowhere else. So the plan's named fallback is the answer, and
the `.gitignore` line it asked for is inverted: only `GanymedRuntime/assets/.assets` (the derived
mesh cache) is ignored. The framing that makes this coherent rather than grubby is that the same file
plays two roles — a machine-local database the editor grows as you import, and *authored content* a
game ships with. `.gitignore` is already per-path, so it says exactly that.

The failure mode was measured, not assumed, because it is worse than "the scene fails to load": with
the registry moved aside the runtime booted, loaded the scene, reported the right entity count, and
rendered nothing but the procedural sky gradient. Every mesh and the HDR environment were **silently**
absent; only `ScriptEngine` complained, because it alone logged the unresolved handle. Screenshot in
the evidence table.

The path-based `MeshPath`/`ScriptPath`/`EnvironmentPath` fallbacks in the deserializer looked like a
way to dodge the whole problem — author the runtime scene path-based and let `ImportAsset` mint
handles at load. Rejected: the editor rewrites to handles on save, so the first time anyone opens the
demo scene and saves it the portability quietly disappears. A property that breaks on a round-trip
nobody thinks about is worse than no property.

**A hole in Phase 1's 1.5, found by 2.1 and fixed.** `ImportAsset` calls `SaveRegistry()`
unconditionally, and it runs *during deserialization* for path-based components — so a "shipped game
must not write its install directory" flag that only guarded `Shutdown` would have been bypassed
before the first frame. The guard moved inside `SaveRegistry()` itself, which covers every present
and future caller, and the parameter was renamed `saveRegistryOnShutdown` → **`writableRegistry`**
because that is what it actually means. Two call sites, both intentional.

**A real bug in the code Phase 2 passes through: a malformed scene took the process down.**
Hand-authoring the demo scene produced 20-digit entity UUIDs, which overflow `uint64_t`, and
yaml-cpp's `as<uint64_t>()` throws on that. `SceneSerializer::Deserialize` had no handler, so the
exception escaped `OnAttach` → `PushLayer` → `main` and terminated the process: no window, no
message, exit code −1. For a shipped game that is the worst available failure mode, and the plan's
own Phase 2 risk list demands the opposite ("make each step log"). `Deserialize` now checks existence
and wraps the parse in one try/catch, returning false with the file and reason logged; the throwing
body moved to a private `DeserializeUnchecked` purely so the try block did not re-indent 260 lines of
component branches. Note this also makes the existing `bool` return meaningful — before, it was false
only for a missing `Scene:` key, so every caller's error handling was decorative.

**One diagnostic added beyond the plan, and it is the antidote to the plan's own stated risk.**
"Order-of-init bugs fail as silent black screens" — the no-registry experiment showed the asset layer
was a source of exactly that: `GetAsset<Mesh>`/`<Environment>`/`<Texture2D>` returned null for an
unknown handle without a word. They now warn, naming the handle and the expected type, **once per
handle** (a `WarnedUnknownHandles` set) because `RenderSystem` re-fetches by handle every frame per
entity and an unguarded warning would arrive at frame rate. Verified: 2 lines for 2 bad handles
across an 8-second run. This is the `AnimationSystem` unknown-clip posture — loud, not broken.

**`WindowedApp` is scoped to Dist, and the plan's parenthetical about the editor was wrong.**
`GanymedEditor` is `WindowedApp` under `filter "system:windows"` for *every* configuration, so it has
no console in Debug either. The runtime keeps `ConsoleApp` outside Dist — but for a duller reason than
first written: on Windows `Log`'s non-file sink is spdlog's `msvc_sink` (OutputDebugString), so the
console window is *empty*. The boot log lives in `GanymedE.log` and the debugger's Output pane either
way. The comment in `GanymedRuntime/premake5.lua` says so rather than implying the console is useful.

**No `.rc`/icon.** The plan offered it as optional; there is no runtime icon asset and inventing one
is not this phase's work. Named, not done.

**Fonts: three faces, not twenty.** `UIEngine::LoadDefaultFonts` loads Regular, Bold and Italic. The
snapshot ships those plus the OFL licence rather than the editor's full Montserrat family — the
"copied snapshot" posture is about *provenance*, not about copying indiscriminately.

**Demo scene content.** No existing scene had a primary camera *and* physics *and* a script —
`BoxesPhysicsExample` has bodies but no camera and no renderable component at all, so in the editor it
is visible only as collider gizmos. The demo scene is hand-authored: sky light, sun, primary
perspective camera, a static floor and two dynamic boxes (all `BoxTextured.glb`, one mesh asset), and
a Player entity carrying the existing `Player.lua`. Reusing that script rather than writing one was
deliberate — it is known-good, and it drives `UI.SetHealth`/`UI.SetScore` and reads WASD, so a single
entity exercises Lua lifecycle, the HUD data model and input forwarding at once. It carries no rigid
body, because it writes its own translation every frame and physics would fight it.

**Observation, not a Phase 2 bug: the demo looks blown out.** The studio HDRI at exposure 1.0 with
bloom washes the sky to near-white. The editor's default new scene does the same thing (see the
Phase 1 notes and the regression screenshot), so this is content/exposure tuning, not the backbuffer
path. Left alone; Phase 5's demo scene is where it is worth authoring properly.

**Pre-existing noise, flagged not fixed:** every solution build prints `'pwsh.exe' is not recognized`
from `vendor/premake/premake5.lua`'s postbuild step, and the editor's registry carries two stale
entries (`assets/models/Fox.glb` with a doubled prefix, and `scripts/_P5Probe.lua` pointing at a
deleted probe). Neither is in this phase's path.

#### Verification evidence

x64 Debug, D3D11. Screenshots are GDI captures of the real window's client area — what a user would
see, not a bgfx readback. Runs were driven by a script that starts the exe, resizes or keys it, grabs
the client rect, and closes it.

| Check | Evidence |
|---|---|
| Boot log table | One line per step, in order: `Window: 1600x900`; config echoed (`scene=… ui=… title=… 1600x900 fullscreen=false`); `AssetManager initialized (3 registered assets, registry read-only)`; `Deserializing scene 'Runtime Demo'` + 7 per-entity lines; `Scene … loaded (7 entities)`; `Primary camera: 'Main Camera'`; `Player created: Player (speed=3.0)`; `Runtime started`; `UI document … loaded`; `--- Boot complete ---`; then `Baked IBL environment` and `RmlUi: compositing into the backbuffer (1600x900)`. **Zero** warnings or errors |
| Play mode is real | Physics: two boxes fall, tumble and settle on the floor with shadows, across screenshots 1→2. Lua: `Player created` is `OnCreate`; the HUD health draining 52 → 4 and score climbing 39 → 80 is `OnUpdate` running every frame; `Player destroyed` on exit is `OnDestroy` |
| HUD | The health bar and score render over the scene in every screenshot, and the data model is being written by Lua rather than static. Event forwarding proven at the other end by Escape reaching `RuntimeLayer::OnKeyPressed` through `UIEngine::OnEvent` (RmlUi declined it) |
| Resize | Scripted `MoveWindow` to 1024×640 outer → log `Resized to 1008x601` (client area), screenshot 2 is 1008×601 with the cubes still cubic — no stretch. `RmlUi: compositing into the backbuffer` did **not** repeat, correct for a target-kind that did not change, and no `SetTarget` re-set was needed |
| Fullscreen | `Fullscreen: true` → `Borderless fullscreen: 1920x1080 at (0, 0)`, `Window: 1920x1080 (borderless fullscreen)`, `RmlUi: … (1920x1080)`, screenshot 3 captured at 1920×1080 with correct aspect and the HUD anchored to the screen edges. Note the config still echoes `1600x900` — Width/Height are ignored under Fullscreen, as documented |
| No-camera scene | Passed on the command line (so this doubles as the CLI-override test): `Scene overridden on the command line: …NoCamera.ganymede`, 1 entity, `RuntimeLayer` reports the missing camera once at boot, then `RenderSystem` logs at 18:09:19 / :24 / :29 — exactly the 5 s throttle. Clear colour only, exit code 0, no crash |
| Clean exit | Escape → `Escape pressed - closing`, `Player destroyed`, **exit code 0**, no asserts, no bgfx leak report. `AssetRegistry.gr` mtime unchanged across four runs and still in hand-authored key order (a `SaveRegistry` would have reordered it — `Registry` is an `unordered_map`) |
| Fresh-clone drill | Deleted both gitignored derived trees (`assets/.assets`, `assets/shaders/compiled`), re-ran `compile_shaders.bat` (378 binaries), booted: mesh re-imported from the `.glb`, IBL rebaked, zero errors. The registry-rebuild question is answered the other way — it is not rebuilt, it is shipped |
| No-registry (extra) | Registry moved aside: boots, 0 registered assets, scene "loads" with 7 entities, renders **only** the procedural sky/ground gradient, HUD frozen at 100/100. After the new diagnostic: exactly 2 warnings, one per unknown handle |
| Editor regression | Whole solution builds (one pre-existing `strncpy` C4996 in `SceneHierarchyPanel`). Editor boots clean: panels docked, hierarchy shows the default Sky Light + Sun, Stats panel live, `AssetManager initialized (7 registered assets, registry **writable**)`, registry rewritten on exit, exit code 0, zero warnings or errors |

**Not verified by me:** Linux and macOS builds (the per-OS link lists were copied verbatim from
`GanymedEditor/premake5.lua`, which is the established posture for those platforms), and the Dist
configuration's `WindowedApp` path.

**Build/tooling note:** `GanymedRuntime` is a new project, so `premake5 vs2022` was re-run;
`compile_shaders.bat` now has three targets. Root `premake5.lua` gained one `include` line.

---

## Phase 3 — miniaudio + `AudioEngine` core + the Audio asset type

**Goal:** the engine can load and play sounds through a pimpl'd, engine-scope `AudioEngine`
with groups and 3D spatialization, with `.wav`/`.mp3`/`.flac` registered as assets — before
any ECS surface exists. Verified from a temporary probe.

### 3.1 Vendoring

- Commit `GanymedEngine/extern/miniaudio/miniaudio.h` (single header, no `.gitmodules` —
  cgltf precedent). `IncludeDir["miniaudio"]` in the root `premake5.lua`; an engine
  `includedirs` entry.
- Implementation TU `GanymedEngine/source/GanymedE/Audio/miniaudio_impl.cpp`:
  `#include "gepch.h"` first (mandatory), `#define MINIAUDIO_IMPLEMENTATION`, include the
  header, wrapped in warning push/disable if the /W level complains (single-header libs
  usually do). The `source/**` glob picks it up; **regenerate premake projects**.
- Platform link surface: Windows/WASAPI — nothing to add; Linux — miniaudio dlopens
  ALSA/PulseAudio (`dl` + `pthread` already in every app's list; add `m` only if the linker
  asks); macOS — **`CoreAudio.framework` + `AudioToolbox.framework` appended to the macosx
  links block of *every* app** (Sandbox, Editor, Runtime) — static-lib links don't propagate.
  Record in `build-and-tooling.md`.

### 3.2 `AudioEngine` (`GanymedE/Audio/AudioEngine.h/.cpp`)

Engine-scope static class, `Init()`/`Shutdown()` from `Application` — Init after
`Renderer::Init` (no ordering dependency; say so in a comment — the ctor's existing comments
set the tone), Shutdown between `ScriptEngine::Shutdown` and `Renderer::Shutdown`.

**The load-bearing decision: every public API call is guarded by an initialized flag, because
layers outlive the dtor body.** `AudioEngine::Shutdown` runs in the Application dtor body;
LayerStack (and the scenes in it, and `AudioSystem::OnRuntimeStop`) destroys *after*. Any
stop-all a layer issues during `OnDetach` must hit a no-op, not a dead `ma_engine`. This is
the `Renderer::IsGpuAlive` pattern applied to audio — same problem shape, same solution — and
it goes in the header comment, or someone will "fix" it.

Shape (all miniaudio types behind the .cpp — pimpl, `PhysicsScene`-style):

```cpp
using VoiceId = uint32_t;                       // 0 = invalid; monotonically issued
enum class AudioGroup : uint8_t { Master, Music, SFX };

class AudioEngine
{
public:
	static void Init();
	static void Shutdown();

	static VoiceId CreateVoice(const std::filesystem::path& fullPath,
	                           AudioGroup group, bool spatial, bool stream, bool loop);
	static void DestroyVoice(VoiceId);          // stops + frees; invalid id = no-op
	static void Play(VoiceId);
	static void Stop(VoiceId);                  // pause-at-position semantics
	static bool IsPlaying(VoiceId);
	static void SetVolume(VoiceId, float);
	static void SetPitch(VoiceId, float);
	static void SetPosition(VoiceId, const glm::vec3&);
	static void SetListener(const glm::vec3& pos, const glm::vec3& fwd, const glm::vec3& up);
	static void PlayOneShot(const std::filesystem::path&, AudioGroup,
	                        const glm::vec3* position /*null = 2D*/, float volume);
	static void SetGroupVolume(AudioGroup, float);
	static void StopAll();                      // scene-scoped callers use this on runtime stop
};
```

Internals: one `ma_engine` + two `ma_sound_group`s (Music, SFX; Master is the engine
endpoint); a `map<VoiceId, ma_sound>` voice table; one-shots in an internal list reaped when
finished (checked from a cheap update tick or lazily in `PlayOneShot` — decide in-phase, log
the choice in the execution notes). Device-init failure (headless box, no output device)
degrades to a logged, alive-flag-false state where every call no-ops — the engine runs
silent, never crashes.

**Decision: lean on miniaudio's resource manager; don't build AssetManager caching.**
`ma_engine` ref-counts decoded data by file path and decodes async; a `ma_sound` per voice is
the intended usage. So **`AssetType::Audio` follows the Script precedent, not the Texture
one**: the registry maps handle→path, `AudioEngine` consumes paths, and there is deliberately
**no `GetAsset<AudioClip>`** — no `Ref<AudioClip>`, no cache map, no specialization.
Production norm: big engines put audio behind the asset system because they cook and stream
banks; Ganymed has no cooking (see "Not doing") and miniaudio already *is* the cache —
duplicating it in AssetManager would be two ref-counting caches disagreeing about lifetime.
The upgrade path (an AudioClip asset when cooking arrives) stays open because handles are
already the currency in components.

**Decision: streaming is an explicit authored bool, not a size heuristic.**
`MA_SOUND_FLAG_STREAM` from a component flag (Phase 4) — music streams, SFX decode. A size
heuristic guesses wrong exactly at the boundary a human never mis-authors (a 4 MB ambience
loop), and it makes play-mode behavior depend on bytes on disk, invisible in the inspector.

### 3.3 Asset type

- `AssetType::Audio` **appended** to the enum (ordinal-persisted — never reorder).
- `AssetTypeFromExtension`: `.wav`, `.mp3`, `.flac`. **Not `.ogg`** — miniaudio's built-in
  decoders are wav/flac/mp3; Vorbis needs stb_vorbis wired in. Named follow-up, not scope.
- `AssetTypeToString`: `"Audio"`. No `GetAsset` specialization — the static_assert message in
  `AssetManager.h` gets a one-line comment noting Audio is path-resolved by design.

### Phase 3 risks

- Device init can fail — the no-op degradation must be real, not theoretical (probe it).
- miniaudio's async decode means "play" logs before sound is audible — verification checks
  sound state, not wall-clock ears alone.
- The impl TU is a very large preprocessed unit — measure build-time impact once, note it.

### Phase 3 verification

Temporary probe (Sandbox layer or editor `OnAttach`, removed afterwards):

| Check | Evidence |
|---|---|
| Init | Log backend, device name, sample rate, channels on startup; clean Shutdown log; re-Init in the same process works |
| Decode + play | One `.wav`, one `.mp3`, one `.flac` each play to completion; `IsPlaying` transitions true→false; table of the three |
| Resource-manager dedup | Two voices from the same path: second `CreateVoice` measurably faster / no second decode (time both) |
| Streaming | A long file with `stream = true`: working-set delta vs decoded mode (log both) |
| Spatial | A looping voice, scripted listener sweep left→right: pan audibly follows; log positions per second |
| Groups | `SetGroupVolume(Music, 0)` silences a Music voice, SFX unaffected |
| One-shot reaping | Fire 20 `PlayOneShot`s, wait, log the internal voice-table size back at baseline |
| Post-shutdown guard | `Play`/`StopAll` after `Shutdown` — no-op, no crash (simulates the layer-detach ordering) |
| Registry | Drop a `.wav` under `assets/` → `ImportAsset` → registry gains an `Audio` entry; survives a save/load roundtrip |

### Phase 3 execution notes

**miniaudio 0.11.25, committed as one header.** `extern/miniaudio/miniaudio.h`, byte-identical to the
`0.11.25` tag and to `master` at the time of vendoring. No licence file alongside it — the licence
(public domain / MIT-0) is in the header's own trailer, and the cgltf precedent is a bare `.h`.

**The plan's stated risk — "the impl TU is a very large preprocessed unit" — was measured and is not
a problem, but not for the expected reason.** x64 Debug, full engine build 26.1 s. Touching only
`miniaudio_impl.cpp` costs 3.5 s; touching `AudioEngine.cpp` costs 3.6 s; touching a trivial TU
(`AssetTypes.cpp`) costs 2.6 s, which is the msbuild+link floor. So the ~84k-line *implementation* is
worth about **1 s**, and `AudioEngine.cpp` — which includes the header for declarations only — costs
exactly the same. The 11.5k-line declaration half dominates, so the two-TU split still earns its
keep, and the `MA_NO_ENCODING`/`MA_NO_GENERATION` trims were dropped: they would have bought a
fraction of that 1 s in exchange for a cross-TU consistency obligation (both TUs must agree on the
option macros or struct layouts diverge silently), which is a bad trade at that price.

**Loading is synchronous, and the plan implied otherwise.** The Phase 3 risk list says "miniaudio's
async decode means 'play' logs before sound is audible", which anticipated `MA_SOUND_FLAG_ASYNC`.
Rejected, and the reason is Phase 4's: `OnRuntimeStart` has to "warn once per entity **with the
path**" when a clip fails to load, and with async the load happens on a job thread with no caller
context, so the error arrives detached from the entity that caused it. Synchronous also matches the
rest of the asset layer, which has no async anywhere. The cost is a first-use decode hitch, which is
precisely what the authored `stream` flag exists for, and which is charged once per path — measured
at 289.8 ms for a 60 s file, then 0.07 ms for the second voice on the same path.

**One-shot reaping: a per-frame tick, which the plan left open.** `AudioEngine::OnUpdate()` is called
from `Application::Run`, outside the minimised gate. Lazy reaping inside `PlayOneShot` was the
alternative and it fails on exactly the workload one-shots are for: a game firing footsteps for ten
minutes and then going quiet would pin every clip it ever played until the next one happened to fire.
The leak is time-shaped, so the reap has to be too. Three lines in the run loop, one call site.

**miniaudio's own fire-and-forget helper is deliberately unused.** `ma_engine_play_sound` looks like
exactly what `PlayOneShot` wants — it even recycles its inlined sounds. It forces
`MA_SOUND_FLAG_NO_SPATIALIZATION` and `MA_SOUND_FLAG_NO_PITCH` and takes no volume, so it cannot
serve a positioned SFX at all. Recorded because it is the obvious thing to reach for.

**`Play` needed no idempotency code, and Phase 5's requirement is already satisfied.**
`ma_sound_start` returns early if the sound is already playing, and seeks back to 0 if it has reached
its end. So a script calling `PlaySound` every frame cannot restart the voice, which is the exact
failure the `PlayAnimation` execution note found the hard way. There is a comment at the call site
telling the next reader not to "improve" it into an unconditional seek.

**Five additions to the plan's API sketch, all with a caller in this phase or the next.**
`SetLooping` (Phase 4's `OnUpdate` pushes Volume/Pitch/**Loop** every frame; the sketch had no way to
push the third); `IsInitialized` (the UIEngine precedent, and the probe's first assertion); `OnUpdate`
(the reap tick above); `GetVoiceCount`/`GetOneShotCount` (the phase's own verification table asks for
"the internal voice-table size back at baseline", which is unobservable without them). Nothing else
was added.

**`VoiceId` is never reset, including across a re-`Init`** — found by writing the re-init test. The
first draft reset the counter in `Init`, which meant a `VoiceId` held across a `Shutdown`/`Init` pair
could name a *different* sound afterwards instead of merely no-opping. One line deleted.

**Both voice containers are node-based, and that is load-bearing.** A `ma_sound` is a node in
miniaudio's graph and its neighbours hold its address, so it must never be relocated after init:
`unordered_map<VoiceId, ma_sound>` and `list<ma_sound>`, never a vector. Each sound is also
initialised *in place* after insertion, for the same reason.

**Spatial panning was measured, not listened to.** The plan's check reads "pan audibly follows",
which the author of these notes cannot verify. Instead, a throwaway harness initialised `ma_engine`
in `noDevice` mode with the same listener convention `AudioEngine::SetListener` uses, read the mix
back with `ma_engine_read_pcm_frames`, and measured per-channel RMS across an emitter sweep. That is
strictly better evidence than ears: it confirms the handedness claim now in the header — right-handed,
−Z forward, **+X is the listener's right** — as a number rather than an impression. Table in the
evidence section and in [`audio.md`](../engine/audio.md).

**The alive guard proved itself in the ordinary shutdown path, not just the scripted one.** In every
run the log shows `AudioEngine shut down` *before* the probe layer's `OnDetach` calls `StopAll()` —
the Application destructor body running ahead of the LayerStack unwinding, exactly as decision 10
predicted. The scripted test (Shutdown mid-run, then call all sixteen public functions) is the
belt-and-braces version.

**Pre-existing noise, flagged not fixed:** Sandbox ships no `assets/fonts/`, so running anything in
it logs three RmlUi font-face errors and two ImGui font warnings; and the engine's static-lib link
prints `LNK4006: __NULL_IMPORT_DESCRIPTOR already defined in gdi32.lib` from `psapi.lib`. Neither is
in this phase's path. The `pwsh.exe is not recognized` postbuild noise from Phase 2 is still there.

**Not verified by me:** audible playback (everything below is device state, timing and measured
sample data — no one listened to it), the Linux and macOS builds, and the Dist configuration. The
macOS framework additions were placed by the same rule as the bgfx frameworks beside them; the Linux
`m` question is left as the plan wrote it — add it only if a linker asks, since bgfx and Jolt already
link without it.

#### Verification evidence

x64 Debug, D3D11, WASAPI. All log lines from a temporary `AudioProbe` layer hosted in Sandbox, since
deleted along with its four generated test files.

| Check | Evidence |
|---|---|
| Init | `AudioEngine initialized (WASAPI, 'Speakers (USB Audio Device)', 48000 Hz, 8 channels)`. Clean `AudioEngine shut down`. Re-`Init` in the same process reopens the device and the next voice is id 11, not id 1 |
| Decode + play | Each format played to completion and `IsPlaying` went true→false at the file's real length, which only happens if the device is genuinely pulling frames: `beep.wav` 1.016 s, `beep.mp3` 1.024 s (encoder padding), `beep.flac` 1.018 s, against a 1.000 s source. `CreateVoice` 6.3 / 8.3 / 9.8 ms. Voice count returned to 0 |
| Resource-manager dedup | Same 60 s path twice, both decoded: **289.80 ms** then **0.07 ms** (≈4000×), working set +11.0 MiB then +0.0 MiB. Destroying both returned the working set to its pre-decode value, so the resource manager frees at refcount zero |
| Streaming | Same file, `stream = true`: `CreateVoice` 11.6 ms, working set **+0.2 MiB** (+0.3 after 1.5 s of playback) versus **+11.0 MiB** decoded. 11.0 MiB is 60 s × 48 kHz × 1 ch × f32 — the decode is resampled to the device rate, which is worth knowing before authoring long clips as non-streamed |
| Spatial | Listener at (0,0,0) facing (0,0,−1), up (0,1,0); emitter swept x −8→+8 at z=−2. Measured balance (R−L)/(R+L): −0.66, −0.65, −0.62, **0.00 at x=0**, +0.62, +0.65, +0.66. Symmetric about the forward axis, +X on the right, and total energy falls with distance under the default inverse attenuation |
| Groups | A `Music` voice and an `SFX` voice playing together; `SetGroupVolume(Music, 0)` then back to 1.0, then `Master` to 0.25 and back. Every call routed and both voices kept playing throughout (`IsPlaying` true on both) — the audible half is unverified, see above |
| One-shot reaping | 20 positioned one-shots fired in 8.6 ms plus one 2D one → internal count **21**; after 3 s → **0**. No `DestroyVoice` calls involved |
| Post-shutdown guard | `Shutdown()` called with a voice still playing → voices 0, one-shots 0, `IsInitialized` false. Then all sixteen public functions called: `Play`, `Stop`, `SetVolume`, `SetPitch`, `SetLooping`, `SetPosition`, `SetListener`, `SetGroupVolume`, `PlayOneShot`, `DestroyVoice`, `StopAll`, `OnUpdate`, `IsPlaying`, `GetVoiceCount`, `GetOneShotCount`, `CreateVoice`. No crash; `CreateVoice` returned 0 and `IsPlaying` false. Then re-`Init` and a `.flac` played normally |
| Registry | Four files imported → all four typed `Audio` with fresh handles. `Shutdown()` (persists) → `Init()` → handle, type and path all identical: `handle=8651894436568738473 type=Audio path='audio/beep.wav'` |
| Regression | Whole solution builds (one pre-existing `strncpy` C4996, one pre-existing `LNK4006`). GanymedRuntime boots the Phase 2 demo scene with one added log line and exits 0; GanymedEditor boots, docks, and exits 0. Neither shows a new warning |

**Build/tooling note:** three new engine source files, so `premake5 vs2022` was re-run. Root
`premake5.lua` gained `IncludeDir["miniaudio"]`; the engine project one `includedirs` entry; the three
apps two macOS frameworks each. No shader-script change (audio has no shaders).

---

## Phase 4 — ECS: components, `AudioSystem`, serialization, inspector

**Goal:** audio is authorable — an entity with an `AudioSourceComponent` plays on start, moves
in 3D with its entity, obeys an `AudioListenerComponent`, and everything survives
save/load/play/stop. All verified in the **editor**, which fully exercises the play-mode
lifecycle without the runtime.

### 4.1 Components (`Components.h`)

```cpp
struct AudioSourceComponent
{
	AssetHandle Clip = InvalidAssetHandle;
	float Volume = 1.0f;
	float Pitch  = 1.0f;
	bool  Loop = false, PlayOnStart = false, Spatialize = true, Stream = false;
	AudioGroup Group = AudioGroup::SFX;
	// No runtime state. The live voice lives in AudioSystem's table (see 4.2).
};

struct AudioListenerComponent
{
	bool Primary = true;
};
```

**Decision: the live voice is system-owned, keyed by entity — the opposite of the animation
palette decision, and the difference is the point.** The palette went on `AnimatorComponent`
because it is *pure data* produced by one system and consumed by another through declared
access; `Scene::Copy` handles a `vector<mat4>` generically. A voice is a **live foreign
resource with a lifecycle** — exactly what a Jolt body is, and the engine already decided
where those live: owned by the system, existing only during play, keyed back to entities
(`PhysicsScene`). An opaque `VoiceId` on the component instead would (a) leak meaningless
runtime state into the serializer's field of view, (b) require a `Scene::Copy` fixup sweep
(the `NativeScriptComponent::Instance` precedent — avoidable here, so avoid it), and (c) let
a copied scene double-drive one voice. With zero runtime state on the component,
**`Scene::Copy` needs no audio fixup at all** — the copy is trivially correct, and the voice
map is built fresh by `OnRuntimeStart` against the runtime copy's entities.

**Decision: explicit `AudioListenerComponent`, primary-camera fallback.** Unity's norm
(AudioListener on the camera, exactly one) teaches the right mental model and keeps
listener-not-on-camera open (third-person games put the listener between camera and
character). Fallback when no listener exists: the primary camera's pose via the
`RenderContext` singleton — a scene with a camera and no listener still sounds right.
Multiple listeners: first wins, warn once (the unknown-clip-name precedent: loud, not broken).

Both components: **untracked** (the system polls every frame; tracking buys nothing, and
untracked means Lua setters skip `MarkChanged` — the `AnimatorComponent` precedent). Full
new-component checklist for both: `Components.h` → `ComponentList` → both `SceneSerializer`
sides (Clip as a guarded handle; **`AudioGroup` serialized as a string, not an ordinal** —
YAML readability, and this enum isn't registry-persisted so strings are free) →
`SceneHierarchyPanel` Add Component entries + `DrawComponent`s. The two hand-maintained lists
have no compile-time enforcement; forgetting one fails silently.

### 4.2 `AudioSystem` (`Scene/Systems/AudioSystem.h/.cpp`)

CRTP `ECS::System<AudioSystem>`; views: `RO<AudioSourceComponent>` +
`RO<WorldTransformComponent>` (emitters), `RO<AudioListenerComponent>` +
`RO<WorldTransformComponent>` (listener). No reactive views → no editor drain obligation.
State: `unordered_map<entt::entity, VoiceId> m_Voices` — runtime-scene entities; the map
lives and dies with the run.

**Registration: after CameraSystem, before RenderSystem** — the chain becomes Physics →
NativeScript → LuaScript → Animation → Transform → Camera → **Audio** → Render (**eight**
built-in systems). The argument: AudioSystem needs post-TransformSystem world transforms
(hard constraint), and the listener *fallback* reads what CameraSystem just resolved —
running after Camera means the fallback pose is this frame's, not last frame's. After Render
would be equally correct (Render reads nothing of audio) but breaks the "Render is last"
invariant readers of the chain rely on, for no gain. The honesty clause from the animation
execution notes applies verbatim: `ValidateOrdering` cannot enforce most of this (no shared
writer/reader pairs) — the slot is a documented intention; comment at the registration site.

Behavior:

- `OnRuntimeStart`: build voices for all `PlayOnStart` sources (resolve the `Clip` handle to
  a path via metadata — the Script-precedent lookup; invalid/missing handle: warn once per
  entity **with the path, not just the handle**, and skip).
- `OnUpdate` (play mode): lazily create voices requested since start (a Lua `PlaySound` on a
  source that never auto-played); push `Volume`/`Pitch`/`Loop` every frame (cheap;
  poll-don't-track); push emitter positions for `Spatialize` sources; resolve and push the
  listener pose. Finished non-looping voices: **keep paused rather than freed** (replay is
  then instant) — confirm in-phase and record.
- `OnRuntimeStop`: destroy every voice in the map, clear it. **Stopping play silences
  everything** — the `PhysicsScene` lifetime rule, applied to sound.
- `OnUpdateEditor`: **nothing — edit mode is silent.** This *follows* the "systems simulate
  only in play mode" norm that AnimationSystem explicitly diverged from; state both facts in
  the header comment so the asymmetry reads as two decisions, not an accident. An inspector
  preview-play button is deferred (see "Not doing").

### 4.3 Inspector (`SceneHierarchyPanel.cpp`)

`AudioSourceComponent`: clip field via `EditorUI::AcceptAssetDropHandle(AssetType::Audio)` —
the animation-milestone helper's first non-founding client, which is the pattern paying rent —
sliders Volume 0–1 / Pitch 0.25–4, checkboxes Loop/PlayOnStart/Spatialize/Stream, Group combo.
`AudioListenerComponent`: the Primary checkbox and a hint line ("falls back to the primary
camera when absent"). Content browser: `.wav`/`.mp3`/`.flac` show as typed assets (icon-tint
reuse is fine).

### Phase 4 risks

- Handle→path resolution at `OnRuntimeStart` is where a fresh clone's missing registry shows
  up first — the warning must carry the path.
- Poll-push of volume/pitch each frame into miniaudio: confirm no per-call allocation or lock
  contention at ~50 sources (probe once).
- Editor play/stop cycles are the leak factory: the voice map must reach zero on every stop.

### Phase 4 verification

Editor play mode, temporary probe logging voice-table size + listener pose per second
(removed afterwards):

| Check | Evidence |
|---|---|
| PlayOnStart | Enter play: source audible within one frame of the `OnRuntimeStart` log line |
| Spatial tracking | A looping emitter on a moving entity (Lua mover): pan/attenuation follow; logged emitter world position matches `WorldTransform` each second |
| Listener component | Listener on a non-camera entity: audio pans relative to *it*, not the camera; remove it → camera fallback, one info log |
| Multiple listeners | Two listeners: first wins + one warning, no repeat |
| Play/stop hygiene | 5 play/stop cycles: voice-table size logged 0 → N → 0 each cycle; the audio device stays open (no re-init) |
| Scene::Copy cleanliness | Runtime-copy writes (Lua volume changes) don't leak to the edit scene; verify by diffing serialized YAML pre/post play |
| Serialization | Save → load roundtrips every field including Group-as-string; a pre-milestone scene (no audio components) loads unchanged |
| Inspector | Drag `.wav` onto the clip field assigns; drag `.lua` is ignored (typed-drop negative test) |
| Stream flag | A music entity with Stream on: working-set delta vs off (the Phase 3 probe repeated through the component path) |

### Phase 4 execution notes

**`AudioTypes.h` split out of `AudioEngine.h`, which is a small correction to Phase 3.**
`Components.h` needs `AudioGroup` and nothing else; including the facade would have pulled
`<filesystem>` and a class full of static methods into every translation unit that touches a scene.
The split mirrors `AssetTypes.h` / `AssetManager.h` exactly — vocabulary in one header, the facade in
another. `AudioGroupFromString` was added alongside `ToString` for the serializer; an unknown name
warns and falls back rather than throwing, so one mistyped group does not cost the whole scene.

**The plan's "lazily create voices requested since start" is not a per-frame scan, and Phase 4 has no
caller for it at all.** `EnsureVoice(entity, source)` is idempotent and is what a request would call,
but the only requester in this phase is `OnRuntimeStart`. The lazy path belongs to Phase 5's
`PlaySound`, where creation happens *in the request* rather than in a poll that looks for work — a
scan over every source every frame asking "did anyone want you yet?" would be doing the request's job
badly. `OnUpdate` therefore only pushes state for voices that already exist.

**"Finished non-looping voices: keep paused rather than freed" needed no code, which the plan asked
to confirm in-phase.** `AudioEngine::Stop` is pause-with-cursor and `ma_sound_start` rewinds a sound
at its end, so a voice that finishes simply sits there and replays instantly. There is a comment in
`OnUpdate` saying what is deliberately absent, because "nothing here frees finished voices" is
exactly the kind of gap a later reader fixes.

**Every audio field is read guarded (`if (node["Volume"])`), diverging from `RigidBodyComponent`'s
bare `as<T>()` two blocks above it.** Phase 2 established that `Deserialize` wraps the whole parse in
one try/catch, so a single absent key in a bare read does not skip a field — it throws out and loses
the file. Audio components are new enough that hand-authoring a scene with one is still normal (the
Phase 5 demo will be), so the guarded form is worth its verbosity here. The older components were
left alone: changing them is a separate, wider decision.

**A NaN guard on the listener basis, which is not paranoia.** `UpdateListener` normalises the
transform's −Z and +Y. An entity scaled to 0 — a completely ordinary way to hide something —
produces a zero-length axis, `glm::normalize` returns NaN, and that NaN lands in miniaudio's
spatializer, which runs on the device thread. A poisoned mix surfacing from a background thread
three subsystems away from the scale field that caused it is a bug worth two lines to never have.

**The editor's play button cannot be driven, so verification used two probes.** It is an
`ImGui::ImageButton` with no keyboard path, and `SendKeys` reached the window but never the layer
(ImGui installs its own GLFW callbacks). So: (a) a Sandbox harness that drives a real `Scene` through
the identical `Copy` → `OnRuntimeStart` → `OnUpdateRuntime` → `OnRuntimeStop` sequence
`EditorLayer::OnScenePlay/OnSceneStop` run, which is where every programmatic check lives; and (b) a
temporary timer in `EditorLayer::OnUpdate` calling the real `OnScenePlay`/`OnSceneStop`, to confirm
the editor's own path agrees. Both removed.

**Measure in Release before optimising, and this phase is the reminder why.** The plan's risk item
asks to confirm the poll-push has no per-call allocation or lock contention at ~50 sources. Debug
said **17 µs per source per frame** — alarming. Release says **0.7 µs** (0.038 ms for 52 sources).
The Debug figure is almost entirely MSVC's checked-iterator `unordered_map` lookups: five per source
per frame, one in `AudioSystem::OnUpdate` plus one inside each of the four `AudioEngine` setters,
which each call `FindVoice`. A batched `SetVoiceState(id, volume, pitch, loop, position)` would
collapse that to one lookup; it is named here and deliberately **not** done, because 0.7 µs is not a
problem and the API bulge would be paid for a Debug-only number.

**A real, pre-existing bug found in passing: the RmlUi debugger document dies on the second play.**
`EditorLayer::OnSceneStop` calls `UIEngine::CloseAllDocuments()`, which destroys the document the
RmlUi `Debugger` plugin owns; the next play logs `RmlUi: A document owned by the Debugger plugin was
destroyed externally. This is not allowed.` Anyone who presses play twice hits it — it only went
unnoticed because nobody had cycled play/stop in a scripted loop before. Nothing to do with audio;
flagged, not fixed. The fix is for `CloseAllDocuments` to skip documents it does not own, or for the
debugger to be shut down and re-initialised around play.

**A pre-existing property, not a bug, that cost real time to rule out: scene YAML entity order is
entt's iteration order.** `save → load → save` produces byte-different files of identical length; the
entity blocks are identical, only their order differs. So a byte compare is the wrong roundtrip test
(the probe sorts blocks before comparing), and — more usefully — **re-saving an unmodified scene
produces a large meaningless diff** for anyone versioning scene files. The serializer already sorts
script `Fields` for exactly this reason; entities are the bigger case and are unsorted. Flagged, out
of scope.

**Not verified by me:** audible playback, as in Phase 3 — everything below is state, timing, YAML and
measured memory. The **drag-and-drop interaction and the Add Component menu entries were read, not
run**: both inspectors were captured on screen with real data (screenshots in the evidence table),
but dragging a `.wav` onto the clip field, and the negative case of dragging a `.lua`, need a hand on
a mouse. The type filter behind them is `AssetTypeFromExtension`, which the registry check does
exercise. Linux, macOS and Dist are unbuilt, as before.

#### Verification evidence

x64 Debug unless stated, D3D11, WASAPI. Programmatic checks from a temporary `AudioProbe` layer in
Sandbox driving the real play/stop lifecycle; editor checks from a temporary timer in `EditorLayer`.
Both removed, along with their four generated assets.

| Check | Evidence |
|---|---|
| PlayOnStart | Entering play built and started both `PlayOnStart` sources: `play -> engine voices = 2` on the same call as `OnRuntimeStart`, with no intervening frame |
| Spatial tracking | A looping spatial emitter on an entity moved every frame, listener at the origin: logged world position tracks the entity — `t=0s world=(-8.00, 0.00, -2.00)` through `t=4s world=(4.75, 0.00, -2.00)`. World lags local by exactly one frame (−4.80 local vs −4.85 world), because `TransformSystem` runs before `AudioSystem` and the probe logs before the update — i.e. what gets pushed is the cache as `AudioSystem` finds it, which is this frame's value |
| Listener component | A listener on a non-camera entity (origin) with the camera at (0,0,12): pushed listener pose is the component's, not the camera's, and no fallback line is logged |
| Camera fallback | Same scene with the component removed: `No AudioListenerComponent in the scene - listening from the primary camera`, exactly once. Also fired for real in `GanymedRuntime`, whose Phase 2 demo scene has no listener |
| Multiple listeners | Two primary listeners: `2 primary AudioListenerComponents in the scene - using the first.` — **one** warning for the whole run, first wins |
| Play/stop hygiene | Five cycles, engine voice count logged at each boundary: `0 → 2 → 0` every time, and `after 5 cycles: engine voices = 0`. The device was opened once at boot and never re-initialised |
| Scene::Copy cleanliness | Play-mode writes to the runtime copy (`Volume` 0.8→0.13, `Pitch` 1.0→1.77, `Loop` true→false, every frame for 30 frames), then the edit scene re-serialized: **YAML unchanged**, edit-scene values still 0.80 / 1.00 / true. No audio fixup exists in `Scene::Copy` and none is needed |
| Serialization | `save → load → save`: 1758 vs 1758 bytes, identical block-for-block (byte-different only in entity order, see above). Every field round-tripped including `Group: Music` as a string, `Stream`, `Spatialize`. A pre-milestone scene (`BoxesPhysicsExample.ganymede`, committed before this milestone) loads unchanged: 6 entities, 0 audio components, no warnings |
| Stream flag | Same scene, same play/stop cycle, one component flag flipped, sampled mid-cycle (a post-stop reading measures nothing — the voice is gone and the buffer freed): streamed +21.8 MiB, decoded +32.6 MiB, so **decoding costs +10.0 MiB** for a 60 s clip. Both figures include the Scene copy and Jolt world; only the difference is audio |
| Poll-push at scale | 52 sources: `OnUpdateRuntime` 0.087 ms/frame vs 0.049 ms with the audio components stripped → **0.038 ms, 0.7 µs per source** (Release). Debug: 1.445 vs 0.669 ms → 17 µs per source. No allocation, no lock on this path |
| Inspector | Screenshots of both components with real data: Audio Source showing `Clip: audio/beep.wav`, Clear, the drop hint, `SFX` group combo, Volume 0.800, Pitch 1.000, Loop ✓ / Play On Start ✓ / Spatialize ✓ / Stream ☐ and the apply-on-play hint; Audio Listener showing Primary ✓ and its fallback hint |
| Editor play/stop | The real `OnScenePlay`/`OnSceneStop`, three cycles: `0 → 2 → 0` each time, exit code 0. This is the path the Sandbox harness models, confirmed against the model |
| Regression | Whole solution builds (one pre-existing `strncpy` C4996). `GanymedRuntime` boots the Phase 2 demo, adds one listener-fallback line, exits 0. `GanymedEditor` boots and exits 0. No new warnings in either |

**Build/tooling note:** four new engine source files (`AudioTypes.h/.cpp`,
`Scene/Systems/AudioSystem.h/.cpp`), so `premake5 vs2022` was re-run. No premake edits — the engine
globs `source/**`. No new dependency, no shader change.

---

## Phase 5 — Lua, the runtime demo, docs sweep

**Goal:** the milestone is usable without touching C++, and the standalone runtime proves it:
`GanymedRuntime.exe` boots the demo scene with music, scripted SFX, and the HUD.

### 5.1 Lua bindings

`ScriptBindings.cpp` (single file on purpose; mirror every signature in
`GanymedEditor/scripts-src/types/ganymed.d.ts` by hand). Entity methods — by-value, no-op on
missing component (`HasRigidBody`/`HasAnimator` precedent), routed through
`scene->Systems().Get<AudioSystem>()` (the physics-binding shape) so the system's voice map
stays the single owner:

`PlaySound()`, `StopSound()`, `SetSoundVolume(f)`, `SetSoundPitch(f)`, `SetSoundLooping(b)`,
`IsSoundPlaying()`, `HasAudioSource()`.

**`PlaySound` on an already-playing source is idempotent** — the `PlayAnimation` execution
note found the per-frame-call idiom the hard way, and the same ordering applies here
(`LuaScriptSystem` runs before `AudioSystem`): a per-frame `PlaySound` must not restart the
voice every tick. Restart = `StopSound()` + `PlaySound()`, the documented, less common
operation.

Globals, minimal: `Audio.PlayOneShot(path, x, y, z)` / `Audio.PlayOneShot(path)` (2D),
`Audio.SetMasterVolume(f)`, `Audio.SetGroupVolume("Music"|"SFX", f)`. One-shots are
fire-and-forget AudioEngine-owned voices — footsteps and impacts without authoring an entity
per sound. Not more than this: no clip-swapping from Lua (author it on the component), no
per-voice handles in Lua (`VoiceId` is engine-internal currency). `/bigobj` is already
engine-wide — the usertype growth is pre-paid.

### 5.2 Runtime demo + polish

- A demo scene in `GanymedRuntime/assets/scenes/`: primary camera, a listener, a `Music`
  looping streamed source, a physics interaction that fires a Lua one-shot, and the HUD
  document. This is the **milestone exit artifact**.
- Sweep the boot path once more against the Phase 2 log table with audio in — specifically
  that an `AudioEngine::Init` failure (headless box) still boots the game, silent.

### 5.3 Docs sweep

Each item belongs to the phase that caused it; this is the audit:

- **New**: `docs/engine/audio.md` — AudioEngine API + the alive-guard contract, the
  no-`GetAsset` decision, voice ownership, groups, the system's chain position.
  `docs/runtime/runtime.md` landed in Phase 2 — audit it against final behavior.
- **Updated**: `scene.md` (component catalog + "eight built-in systems" + the
  silent-edit-mode note contrasted with AnimationSystem); `ecs.md` (the named system chain);
  `assets.md` (`AssetType::Audio`, path-resolved-by-design); `scripting.md` (bindings + the
  idempotent `PlaySound` note); `build-and-tooling.md` (miniaudio vendoring, macOS
  frameworks, the third shader-script target); `editor.md` (audio inspectors);
  `architecture.md` (module list; the limitations section was fixed in Phase 2 — re-audit);
  `AGENTS.md` (+2 table rows); `docs/README.md` (doc map + applications table).
- `ganymed.d.ts` diffed against `ScriptBindings.cpp` signature by signature.

### Phase 5 verification

| Check | Evidence |
|---|---|
| Lua drives audio | A script toggles a source on a key press in play mode; `PlaySound` called every frame does **not** stutter/restart (log `IsSoundPlaying` + frame count — the idempotency test) |
| One-shots | 20 scripted `Audio.PlayOneShot`s: audible, engine voice count returns to baseline (logged) |
| Group volumes from Lua | `SetGroupVolume("Music", 0)` mutes music; SFX one-shots still audible |
| **Milestone exit** | Fresh clone → scripts → build → `GanymedRuntime.exe`: the demo scene boots to play mode fullscreen, music streams, physics-triggered SFX fires, HUD renders, Esc exits clean. Boot-log table captured in the execution notes |
| d.ts parity | Every binding present in `ganymed.d.ts`, spot-checked by a probe script using each once |
| Docs audit | Each doc claim spot-checked against code; the "eight systems" count grep'd |

---

## Explicitly not doing (v1)

Named so nobody half-builds them in passing:

- **Mixing beyond Master + Music/SFX groups** — the two fixed groups are in (they cost
  nothing and Lua wants `SetGroupVolume`); arbitrary bus graphs, sends, ducking, and
  snapshots are out. First acceptable follow-up: a data-driven group list.
- **DSP effects** (reverb, filters, EQ) — out.
- **Doppler** — out; velocities stay zero. Upgrade path noted: feed rigid-body velocity into
  the voice when it matters.
- **Audio occlusion/obstruction** — out.
- **`.ogg`/Vorbis** — out (needs stb_vorbis vendored); wav/mp3/flac cover v1 content.
- **Inspector audio preview** (edit-mode play button) — out; edit mode stays silent.
- **Asset packing/cooking, an AudioClip asset with `GetAsset`** — out; the runtime reads a
  loose `assets/` snapshot and miniaudio's resource manager is the cache. Cooking is its own
  milestone and the one that revisits this.
- **Installer/distribution, exclusive fullscreen, multiple windows** — out. Borderless
  fullscreen via config is the v1 ceiling.
- **Runtime hot-reload** (scripts, assets, shaders in GanymedRuntime) — out; that's editor
  tooling.
- **A pause/menu system** — out; Esc quits, documented as a placeholder.
- **Off-mouse picking readback fix** (the animation Phase 4 loose end) — not this milestone,
  re-flagged here so it stays visible. Likewise the picking `RED_INTEGER` attachment still
  allocated in the runtime: dead cost, accepted v1, noted in `runtime.md`.
- **`RenderContext::EditorViewCamera` rename** — debt, flagged, untouched.

## Design tensions, recorded

1. **Retarget the final pass vs a dedicated present pass** → retarget. The present pass is
   the production norm because it carries scaling/HDR-display duties Ganymed doesn't have
   yet; retargeting is one bool and zero shaders, and it exercises the shipping RmlUi
   null-target path. Escape hatch (a real present pass) named for a resolution-scaling
   milestone.
2. **GL flip parity on the retargeted pass** → note-and-defer, D3D-primary — the
   MaxBones/minimal-GL posture, applied again.
3. **Voice on the component vs a system-owned map** → system map. A voice is a live foreign
   resource (Jolt-body-shaped), not pure data (palette-shaped); zero runtime state on the
   component makes `Scene::Copy` trivially correct with no fixup sweep.
4. **Explicit listener vs implicit camera** → explicit component, camera fallback, warn on
   multiples. Teaches the Unity-norm model; keeps listener-off-camera open.
5. **AssetManager caching vs miniaudio's resource manager** → miniaudio's. Two ref-counting
   caches over one resource is a lifetime argument waiting to happen; Script's
   handle→path-only precedent fits exactly.
6. **Streaming: authored bool vs size heuristic** → authored bool. Intent over inference;
   behavior visible in the inspector, not dependent on bytes on disk.
7. **ImGui coupling: spec flag vs subclass hook** → spec flag; a virtual can't fire from the
   base ctor anyway.
8. **Runtime writes `AssetRegistry.gr` on exit** → no; the `Init(writableRegistry)` toggle,
   guarded inside `SaveRegistry()` so `ImportAsset` is covered too. Two lines now vs a
   Program-Files write failure later.
12. **Runtime asset references: committed registry vs path-based scenes** → committed registry
    (Phase 2). Path-based scenes would make the runtime self-describing, but the editor rewrites
    them to handles on save, so the property dies on the first round-trip. For a shipped game the
    registry is authored content; the real fix is per-asset committed metadata (Unity `.meta`),
    which is an asset-identity milestone of its own.
9. **Edit-mode audio: silent vs preview** → silent, the systems norm — explicitly contrasted
   with AnimationSystem's documented divergence so both read as decisions.
10. **AudioEngine shutdown vs layer teardown order** → alive-guard on every API call (the
    `Renderer::IsGpuAlive` pattern); the Application dtor body runs before LayerStack
    destruction and nothing can reorder that without touching every app.
11. **Runtime config: YAML file vs hardcode/CLI** → YAML + CLI scene override; the editor's
    hard-coded `hud.rml` is the anti-pattern this app exists to shed.

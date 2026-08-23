# GanymedRuntime

The standalone game player: `GanymedRuntime.exe` reads a config, deserializes one scene straight
into play mode, and renders it fullscreen-to-backbuffer with an RmlUi HUD on top. No ImGui, no edit
mode, no play/stop, no picking, no gizmos.

It exists to prove the engine can host a second front-end. Everything the editor does that a shipped
game must *not* do had to become opt-in for this app to be possible — see
[architecture.md](../engine/architecture.md) on why the engine has no editor `#ifdef`s.

| | |
|---|---|
| Sources | `GanymedRuntime/source/` — three files: `RuntimeApp.cpp`, `RuntimeLayer`, `RuntimeConfig` |
| Content | `GanymedRuntime/assets/` — a copied snapshot of editor-authored assets (see [Assets](#assets)) |
| Working directory | the project folder (`debugdir "%{prj.location}"`); all asset paths are relative to it |
| Windows subsystem | `ConsoleApp`, except Dist which is `WindowedApp` + `mainCRTStartup` so a shipped game has no console behind it |

## Boot sequence

`CreateApplication` runs before any window exists, so it does config first:

1. `RuntimeConfig::Load("assets/runtime.yaml")`, then `ApplyCommandLine` — the scene can be
   overridden with `GanymedRuntime <path/to/scene.ganymede>`.
2. Build the `ApplicationSpecification` from it: `Name`/`Width`/`Height`/`Fullscreen`, and
   **`EnableImGui = false`**. That single flag means no `ImGuiLayer` is constructed, so nothing
   installs ImGui's GLFW callbacks and nothing blocks events — every event reaches the game raw.
3. `PushLayer(new RuntimeLayer(config))`.

`RuntimeLayer::OnAttach`, in order — every step is a lesson `EditorLayer` already learned, and each
one logs, because the failure mode for getting this order wrong is a black window with no message:

| Step | Why here |
|---|---|
| `AssetManager::Init(/*writableRegistry=*/false)` | Must precede any deserialize: scenes store bare asset handles and the serializer resolves them through the registry. Read-only because a shipped game must not write into its install directory ([assets.md](../engine/assets.md)) |
| `SceneRenderer(windowW, windowH)` | Owns the HDR target and the post stack |
| `SetOutputToBackbuffer(true)` | Retargets the post stack's final pass (FXAA, or tonemap when FXAA is off) at the backbuffer ([rendering.md](../engine/rendering.md)) |
| `UIEngine::SetTarget(nullptr)` + `SetViewport(w, h)` | Sends the UI view to the backbuffer too. `SetViewportOrigin` stays at its (0,0) default — the game owns the whole window, so window-relative mouse positions are already UI-relative |
| `SceneSerializer::Deserialize` into the scene | **No `Scene::Copy`.** The copy exists to preserve the editor's edit-time scene across play/stop; there is no edit-time scene here and no stop |
| `Scene::OnViewportResize` → `OnRuntimeStart` | Creates the Jolt world and instantiates Lua scripts |
| `UIEngine::LoadDocument(config.UIDocument)` | After the scene starts, so a script's `OnCreate` has already written the HUD data model before first layout |

`OnDetach` unwinds it: close documents → `OnRuntimeStop` → drop the scene and renderer →
`AssetManager::Shutdown()`. That last call releases GPU caches, and it is safe because `LayerStack`
is destroyed before the `Window` — so bgfx is still alive. The same guarantee `EditorLayer` leans on.

## The frame

```
RuntimeLayer::OnUpdate(ts)
├─ Renderer2D/3D::ResetStats
├─ SceneRenderer::BeginFrame          bind + clear the HDR target
├─ Scene::OnUpdateRuntime(ts, nullptr)   all eight systems
├─ UIEngine::OnUpdate(ts) / OnRender()   layout, then submit to RenderPass::UI
└─ SceneRenderer::EndFrame            bloom → tonemap → FXAA, final pass to the backbuffer
```

The `nullptr` is the fallback camera, and it is deliberate: an editor camera is the editor's
business. A scene with no primary `CameraComponent` renders the clear colour and `RenderSystem` logs
an error every five seconds ([scene.md](../engine/scene.md)) — loud, throttled, and it does not
crash.

View order stays monotonic, which is what makes the whole thing work without a present pass:
0 (BgfxContext touch) < 5/6 (scene) < 7–22 (bloom) < 24/25 (final post) < 28 (UI).

## Events

`OnEvent` gives the HUD first refusal unconditionally — `UIEngine::OnEvent` marks the event handled
when RmlUi consumed it. The editor gates the same call on the viewport owning the pointer, because
there a click might belong to a panel; here there is nothing else on screen to own it.

Then two handlers:

- **`WindowResizeEvent`** → `SceneRenderer::SetViewportSize`, `Scene::OnViewportResize`,
  `UIEngine::SetViewport`. Note what is *absent*: the editor must re-call `UIEngine::SetTarget`
  after every resize because `SetViewportSize` rebuilds the composite framebuffer and the old
  pointer is dangling. With a null target there is nothing to re-point — one fewer thing to forget.
  Minimize-to-zero never arrives here; `Application::OnWindowResize` swallows it.
- **`KeyPressedEvent`** → Escape calls `Application::Close()`. A **placeholder for a pause menu**,
  which is out of scope for this milestone. A chromeless fullscreen window whose only exit is
  Alt+F4 is a worse default than a quit key a real menu will later take over.

F1 (bgfx debug stats) needs nothing here — `Application::OnKeyPressed` handles it, and bgfx's debug
text works without ImGui.

## Config

`assets/runtime.yaml`, parsed by `RuntimeConfig` with the yaml-cpp the engine already links. Six
keys, every one optional; a missing or malformed file logs and boots with all defaults, because an
exe that starts and tells you what is wrong beats one that refuses to start.

| Key | Default | Notes |
|---|---|---|
| `StartScene` | `assets/scenes/Demo.ganymede` | The only field the command line can override |
| `UIDocument` | `assets/ui/hud.rml` | Empty string = no HUD |
| `Title` | `GanymedEngine Runtime` | Window title |
| `Width` / `Height` | 1600 / 900 | Ignored when `Fullscreen`; a zero is rejected with a warning |
| `Fullscreen` | `false` | Borderless — an undecorated window at the primary monitor's video mode ([platform.md](../engine/platform.md)) |

Why a file at all, when there is exactly one scene? Because hard-coding `assets/ui/hud.rml` is
precisely the editor-ism this app exists to shed (`EditorLayer::OnScenePlay` still does it), and a
CLI-only design leaves title, geometry and fullscreen homeless. The production norm is a
project-settings asset with a scene list (Unity's Build Settings, Unreal's `DefaultGame.ini`); the
divergence here is scale-honest — six keys in a file the app parses, not an asset type.

## Assets

`GanymedRuntime/assets/` is a **copied snapshot** of editor-authored content. Sharing a tree with
the editor, or packing it, is a non-goal for this milestone: every app resolves `assets/` from its
working directory, and cooking is its own milestone.

Two consequences worth knowing before adding content:

- **`AssetRegistry.gr` is committed for this app**, unlike the editor's. Scenes store bare handles,
  so a runtime with no registry silently loses every mesh and environment — verified, and the
  reason `assets.md` now calls the registry *authored content* for a runtime rather than a cache.
  Add an asset by adding both the file and its registry entry, with the handle the scene uses.
- Only the three font faces `UIEngine` actually loads are shipped (`Montserrat-Regular`, `-Bold`,
  `-Italic`, plus the OFL licence). RmlUi hard-requires them: missing fonts render as a silently
  empty UI, not an error.

`assets/.assets/` (the mesh cache) and `assets/shaders/compiled/` are derived and gitignored.
`scripts/compile_shaders.bat` writes this app's copy alongside the editor's and Sandbox's.

## Divergences from the editor render path, on purpose

- **No picking consumer.** The HDR framebuffer still allocates its `RED_INTEGER` entity-ID
  attachment and `RenderSystem` still writes IDs into it. Dead cost in a shipped game, accepted for
  v1 and named here so it is not mistaken for something load-bearing.
- **`GetFinalImageRendererID()` asserts.** In backbuffer mode nothing writes the composite target,
  so asking for it is a bug at the call site.
- **GL flip parity is a known, deferred risk.** `vs_Tonemap`/`vs_FXAA` carry the V-flip branch
  written for offscreen targets, and bgfx flips offscreen versus backbuffer on GL. The primary
  platform is D3D and is unaffected; see the flip-parity rule in
  [rendering.md](../engine/rendering.md) for what to change if this bites on GL.

# Architecture Overview

## Module layout

Everything in the engine lives in `namespace GanymedE` (ECS machinery in `GanymedE::ECS`), under
`GanymedEngine/source/`:

```
GanymedE/
├── main/        Application, EntryPoint          — the run loop and program entry
├── Core/        Window, Layer(Stack), Input, Log, UUID, Random, Timestep, Core.h macros
├── events/      Event base + dispatcher, window/key/mouse events
├── ECS/         The view/access-wrapper layer over entt (see ecs.md)
├── Reflection/  Component MEMBER reflection over entt::meta (see scene.md)
├── Scene/       Scene, Entity, Components, SceneSerializer, built-in Systems/
├── Renderer/    bgfx-backed renderer: resources, Renderer2D/3D, SceneRenderer, IBL, cameras
├── Assets/      AssetManager (scan-derived index + manager registry), AssetMeta, CompiledCache + compilers
├── Physics/     PhysicsScene (Jolt, pimpl'd)
├── Audio/       AudioEngine (miniaudio, behind the .cpp — see audio.md)
├── Scripting/   ScriptEngine (the shared Lua VM) + the sol2 bindings (see scripting.md)
├── UI/          UIEngine (RmlUi game UI; the editor's own UI is ImGui — see ui.md)
├── Math/        Transform decomposition, AABB + Frustum, FloatCurve + ColorGradient
├── ImGui/       ImGuiLayer (docking UI host)
├── Debug/       Instrumentor (chrome://tracing profiler)
└── Utils/       PlatformUtils (file dialogs)

Platform/
├── Bgfx/        BgfxContext (bgfx lifetime + swapchain), ImGuiRendererBgfx
├── Bimg/        TextureEncode - block compression. Its OWN static lib, built as C++20
│                because bx does not compile below it (see GanymedEngine/TextureEncode.lua)
├── Windows/     WindowsWindow, WindowsInput, file dialogs
├── Linux/       LinuxWindow, LinuxInput, file dialogs
└── macOS/       macOSWindow, macOSInput, file dialogs
```

Client applications include the umbrella header [`GanymedE.h`](../../GanymedEngine/source/GanymedE.h),
implement `GanymedE::CreateApplication()`, and include
[`EntryPoint.h`](../../GanymedEngine/source/GanymedE/main/EntryPoint.h) exactly once — the engine
owns `main()`.

There are two real front-ends, and the engine has **zero editor `#ifdef`s** — the difference is
configuration, not compilation. `CreateApplication` returns an `Application` built from an
`ApplicationSpecification`, and `EnableImGui = false` is the whole opt-out that separates
[`GanymedEditor`](../editor/editor.md) (ImGui chrome, scene rendered into a viewport panel) from
[`GanymedRuntime`](../runtime/runtime.md) (no ImGui, scene rendered straight to the backbuffer).
Anything that reads as editor-only behaviour inside the engine is a bug; the collider-gizmo gate on
`PhysicsSettings::ShowColliderGizmos` is there because it *was* one.

## The frame, end to end

One iteration of [`Application::Run`](../../GanymedEngine/source/GanymedE/main/Application.cpp):

```
Application::Run loop
│
├─ compute Timestep from glfwGetTime()
│
├─ JobSystem::OnUpdate                     drain main-thread jobs (runs even while minimized)
├─ AssetManager::Update                    apply parses that finished on workers (the only
│                                          place the async asset path creates GPU resources)
│
├─ Layer::OnUpdate for each layer          (EditorLayer in the editor)
│   │
│   ├─ SceneRenderer::BeginFrame           binds + clears the HDR scene target
│   ├─ Scene::OnUpdateEditor / OnUpdateRuntime
│   │   ├─ FrameBegin                      epoch++, rotate change buffers, flush CommandQueue
│   │   ├─ SystemManager::OnUpdate[Editor] (m_IsUpdating = true while running)
│   │   │   ├─ PhysicsSystem               fixed-step Jolt, collision events, transform writeback
│   │   │   ├─ NativeScriptSystem          script lifecycle + OnUpdate
│   │   │   ├─ LuaScriptSystem             the same lifecycle for Lua ScriptComponents
│   │   │   ├─ AnimationSystem             sample clips → joint palette on AnimatorComponent
│   │   │   ├─ TransformSystem             recompute dirty world transforms (ChangeView)
│   │   │   ├─ CameraSystem                resolve primary camera → RenderContext singleton
│   │   │   ├─ AudioSystem                 push voice state + emitter/listener poses to AudioEngine
│   │   │   ├─ ParticleSystem              CPU emit/age/integrate (edit and play)
│   │   │   └─ RenderSystem                submit lights/sky/meshes/particles/sprites/gizmos
│   │   └─ FrameEnd                        clear init/fini buffers + graveyards
│   ├─ entity-ID pick request/poll         (editor)
│   └─ SceneRenderer::EndFrame             bloom → tonemap → FXAA → composite
│
├─ AudioEngine::OnUpdate                   reap finished one-shots (runs even while minimized)
│
├─ ImGuiLayer::Begin / Layer::OnImGuiRender / ImGuiLayer::End
│   └─ editor panels, viewport image, gizmos → ImGui draw data → bgfx view 200
│
└─ Window::OnUpdate
    ├─ glfwPollEvents                      → event callbacks → Application::OnEvent → layers (top-down)
    └─ BgfxContext::Frame                  bgfx::frame() — submits + presents
```

Two ordering facts worth internalizing:

- **bgfx executes the frame in view-ID order, not call order.** "Where a draw goes" is the current
  view ID ([`RenderPassIDs.h`](../../GanymedEngine/source/GanymedE/Renderer/RenderPassIDs.h)), and
  the whole frame's pass schedule is that table: shadows (1–4) → scene HDR (5) → 2D/transparent (6)
  → bloom (7–23) → tonemap (24) → FXAA (25) → composite (26) → picking blit (27) → ImGui (200).
- **Structural ECS changes made by systems are deferred.** They queue through
  `Scene::Commands()` and apply at the *next* `FrameBegin`, so nothing mutates the registry while
  views iterate it. Editor/tooling code outside the update loop uses the immediate `Entity` API.

## Ownership

- `Application` (singleton) owns the `Window` and the `LayerStack`. The window owns the
  `BgfxContext`, which owns bgfx itself — its destructor lowers `Renderer::IsGpuAlive()` *before*
  `bgfx::shutdown()`, and every GPU-resource destructor checks that flag (statics can outlive
  `main()`; C++ guarantees nothing about their order relative to bgfx teardown).
- The front-end layer — `EditorLayer` or `RuntimeLayer` — owns the `SceneRenderer` (render targets +
  post stack) and the active `Scene`. Neither the engine nor `Application` holds a scene.
- `Scene` owns the entt registry, the `SystemManager` (nine built-in systems), the `CommandQueue`,
  per-component-type change buffers / graveyards / init-fini buffers, and the UUID→entity map.
  Scene-wide state lives in singletons in `registry.ctx()` (`RenderContext`, `PhysicsSettings`).
- `PhysicsSystem` owns the `PhysicsScene` (Jolt world) — it exists only between play and stop.
- `AudioEngine` owns the miniaudio device and every live voice. It is static-lifetime and explicitly
  `Init()`/`Shutdown()` by `Application`; because that shutdown runs in the destructor *body*, before
  the LayerStack unwinds, every one of its calls no-ops once shut down — the `IsGpuAlive` pattern
  again (see [audio.md](audio.md)).
- Renderer subsystems (`Renderer2D`, `Renderer3D`, `ParticleRenderer`, `PostProcess`, `MeshShader`) are static-lifetime
  but explicitly `Init()`/`Shutdown()` by `Renderer`, releasing GPU handles while bgfx is alive.
- **Loaded assets are owned by whatever holds an `AssetRef<T>`, which in practice is a scene.**
  `TypedAssetManager<T>` tracks them in a `weak_ptr` cache, one manager per managed type in a flat
  slot array indexed by a dense type id ([assets.md](assets.md#managers-and-caching)), but the cache
  keeps nothing alive: a component field is the owner, so closing a scene collects its meshes,
  materials, textures and environments. `AssetManager::Shutdown` destroys the managers while
  `Renderer::IsGpuAlive()` is still true, the same discipline as the renderer subsystems above.

## Design principles the code actually follows

1. **Declared access is the single source of truth.** A system's view declarations
   (`RO<T>`, `RW<T>`, `React*<T>`…) simultaneously define what its result tuples look like, which
   change buffers it consumes, and its scheduling metadata (`ViewDesc`). The `Scene` constructor
   asserts that system registration order is consistent with those declarations
   (`SystemManager::ValidateOrdering`).
2. **Writes to tracked components are explicit.** A tracked+writeable slot is never a bare `T&`;
   the only mutable path is `Modify()`, which logs the change. Code that writes a tracked component
   outside a view (editor panels, gizmos, physics writeback) must call
   `Scene::MarkChanged<T>()` — several call sites carry comments to that effect.
3. **One list per concept.** `ComponentList` in
   [`ComponentTraits.h`](../../GanymedEngine/source/GanymedE/ECS/ComponentTraits.h) is the single
   registry of components; `Scene::Copy`, signal hookup, and the ViewDesc bitmask index all iterate
   it instead of hand-maintained parallel lists. `Reflection` is the member-level counterpart, and
   `Reflection::Validate()` asserts the two lists agree at boot
   ([scene.md](scene.md#member-reflection)).
4. **Derived data is cached and invalidated, not recomputed.** `WorldTransformComponent` is the
   flagship: an idle scene recomputes zero world matrices; moving one entity recomputes exactly its
   subtree (TransformSystem's ChangeView).
5. **Concrete over virtual where there is one implementation.** The old
   `RendererAPI`/`GraphicsContext` virtual layers are gone; `Shader`, `Texture2D`, `Framebuffer`
   etc. are concrete wrappers over bgfx handles. `RendererAPI` survives only as a backend enum.
6. **Plain-data components, engine types firewalled.** Components are plain structs; Jolt types
   never appear in headers (`PhysicsScene` is pimpl'd); miniaudio types appear in exactly two `.cpp`s
   and never in a header, not even as a forward declaration; enkiTS is reduced to a `void*` in
   `JobSystem.h`'s shared state, so only the engine project compiles against it; bgfx types appear
   only in renderer headers.

## Current limitations / known state

- **The frame is still single-threaded**, and a scheduler existing does not change that. One scene
  update per frame on the main thread; bgfx runs in single-threaded mode (`renderFrame()` before
  `init`). [`Core/JobSystem`](core.md#job-system) has two consumers and neither is in the frame: the
  texture compiler's block encode
  ([`THREADING_ROADMAP.md`](../toDo&done/THREADING_ROADMAP.md) T3, 1.8× on a 2560×1664 texture)
  and asset parsing (T4). No ECS system runs on it. Jolt still runs its own pool,
  so there are currently two pools of `hardware_concurrency() - 1` threads. The ViewDesc machinery
  exists so ECS parallelism can be added without redesign.
- **Asset loading is asynchronous.** Identity is per-asset `.meta` sidecars, loading goes through a
  manager registry with a Parse/Apply split, `AssetRef<T>` is the reference type components hold,
  sources are compiled into `assets/.compiled/` with content-hash invalidation, and the parse half
  runs on `JobSystem` workers while `AssetManager::Update` applies the results on the main thread.
  Measured: 845 ms of cold-import work that used to block the frame loop now costs it nothing
  (see [assets.md](assets.md#asynchronous-loading)).
  [`ASSET_PIPELINE_ROADMAP.md`](../toDo&done/ASSET_PIPELINE_ROADMAP.md) Phases 1–5 have landed. What
  is left is **hot reload** (Phase 6): editing an asset on disk still needs a manual Reload.
- **Component members are reflected, but nothing consumes it yet.**
  [`Reflection/`](scene.md#member-reflection) registers all 23 components over `entt::meta` and
  validates itself at boot; the serializer, the inspector and the Lua bindings still hand-list every
  field, and behaviour is unchanged. Collapsing them is R2–R4 of
  [`REFLECTION_ROADMAP.md`](../toDo&done/REFLECTION_ROADMAP.md).
- Scripting is Lua 5.4 + sol2 (`ScriptComponent`, hot-reloadable, TypeScript-authored via
  TypeScriptToLua) *and* C++ `NativeScriptComponent`. See [scripting.md](scripting.md).
  `Scripting-And-UI-Integration.md` is the plan that delivered it, not a plan for the future.
- Shadows work. `BGFX_MIGRATION.md` §8.8 records an empty-shadow-map regression from the end of
  that migration; it was fixed, and the animation milestone measured the cascades in use. Read
  that section as history, not as current state.
- `GLM_FORCE_DEPTH_ZERO_TO_ONE` is a compile-time, workspace-wide choice. A backend reporting
  `homogeneousDepth == true` (OpenGL) logs an error rather than adapting; the caps-driven
  projection helper is still open (migration §9.3).

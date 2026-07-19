# Architecture Overview

## Module layout

Everything in the engine lives in `namespace GanymedE` (ECS machinery in `GanymedE::ECS`), under
`GanymedEngine/source/`:

```
GanymedE/
├── main/        Application, EntryPoint          — the run loop and program entry
├── Core/        Window, Layer(Stack), Input, Log, UUID, Timestep, Core.h macros
├── events/      Event base + dispatcher, window/key/mouse events
├── ECS/         The view/access-wrapper layer over entt (see ecs.md)
├── Scene/       Scene, Entity, Components, SceneSerializer, built-in Systems/
├── Renderer/    bgfx-backed renderer: resources, Renderer2D/3D, SceneRenderer, IBL, cameras
├── Assets/      AssetManager (handle registry), MeshCache
├── Physics/     PhysicsScene (Jolt, pimpl'd)
├── Scripting/   ScriptEngine (the shared Lua VM) + the sol2 bindings (see scripting.md)
├── Math/        Transform decomposition, AABB + Frustum
├── ImGui/       ImGuiLayer (docking UI host)
├── Debug/       Instrumentor (chrome://tracing profiler)
└── Utils/       PlatformUtils (file dialogs)

Platform/
├── Bgfx/        BgfxContext (bgfx lifetime + swapchain), ImGuiRendererBgfx
├── Windows/     WindowsWindow, WindowsInput, file dialogs
├── Linux/       LinuxWindow, LinuxInput, file dialogs
└── macOS/       macOSWindow, macOSInput, file dialogs
```

Client applications include the umbrella header [`GanymedE.h`](../../GanymedEngine/source/GanymedE.h),
implement `GanymedE::CreateApplication()`, and include
[`EntryPoint.h`](../../GanymedEngine/source/GanymedE/main/EntryPoint.h) exactly once — the engine
owns `main()`.

## The frame, end to end

One iteration of [`Application::Run`](../../GanymedEngine/source/GanymedE/main/Application.cpp):

```
Application::Run loop
│
├─ compute Timestep from glfwGetTime()
│
├─ Layer::OnUpdate for each layer          (EditorLayer in the editor)
│   │
│   ├─ SceneRenderer::BeginFrame           binds + clears the HDR scene target
│   ├─ Scene::OnUpdateEditor / OnUpdateRuntime
│   │   ├─ FrameBegin                      epoch++, rotate change buffers, flush CommandQueue
│   │   ├─ SystemManager::OnUpdate[Editor] (m_IsUpdating = true while running)
│   │   │   ├─ PhysicsSystem               fixed-step Jolt, collision events, transform writeback
│   │   │   ├─ NativeScriptSystem          script lifecycle + OnUpdate
│   │   │   ├─ TransformSystem             recompute dirty world transforms (ChangeView)
│   │   │   ├─ CameraSystem                resolve primary camera → RenderContext singleton
│   │   │   └─ RenderSystem                submit lights/sky/meshes/sprites/gizmos to Renderer2D/3D
│   │   └─ FrameEnd                        clear init/fini buffers + graveyards
│   ├─ entity-ID pick request/poll         (editor)
│   └─ SceneRenderer::EndFrame             bloom → tonemap → FXAA → composite
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
- `EditorLayer` owns the `SceneRenderer` (render targets + post stack) and the active `Scene`.
- `Scene` owns the entt registry, the `SystemManager` (five built-in systems), the `CommandQueue`,
  per-component-type change buffers / graveyards / init-fini buffers, and the UUID→entity map.
  Scene-wide state lives in singletons in `registry.ctx()` (`RenderContext`, `PhysicsSettings`).
- `PhysicsSystem` owns the `PhysicsScene` (Jolt world) — it exists only between play and stop.
- Renderer subsystems (`Renderer2D`, `Renderer3D`, `PostProcess`, `MeshShader`) are static-lifetime
  but explicitly `Init()`/`Shutdown()` by `Renderer`, releasing GPU handles while bgfx is alive.

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
   it instead of hand-maintained parallel lists.
4. **Derived data is cached and invalidated, not recomputed.** `WorldTransformComponent` is the
   flagship: an idle scene recomputes zero world matrices; moving one entity recomputes exactly its
   subtree (TransformSystem's ChangeView).
5. **Concrete over virtual where there is one implementation.** The old
   `RendererAPI`/`GraphicsContext` virtual layers are gone; `Shader`, `Texture2D`, `Framebuffer`
   etc. are concrete wrappers over bgfx handles. `RendererAPI` survives only as a backend enum.
6. **Plain-data components, engine types firewalled.** Components are plain structs; Jolt types
   never appear in headers (`PhysicsScene` is pimpl'd); bgfx types appear only in renderer headers.

## Current limitations / known state

- Single-threaded: one scene update per frame on the main thread; bgfx runs in single-threaded
  mode (`renderFrame()` before `init`). The ViewDesc machinery exists so parallelism can be added
  without redesign.
- Shadows had a known regression at the end of the bgfx migration (empty shadow map — see
  [`BGFX_MIGRATION.md` §8.8](../toDo&done/BGFX_MIGRATION.md)); verify against current state before
  relying on the doc.
- Scripting is C++ `NativeScriptComponent` only (recompile to change behavior);
  [`Scripting-And-UI-Integration.md`](../toDo&done/Scripting-And-UI-Integration.md) is the planned
  next phase.
- `GLM_FORCE_DEPTH_ZERO_TO_ONE` is a compile-time, workspace-wide choice. A backend reporting
  `homogeneousDepth == true` (OpenGL) logs an error rather than adapting; the caps-driven
  projection helper is still open (migration §9.3).

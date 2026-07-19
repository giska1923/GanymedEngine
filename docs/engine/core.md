# Core

The application skeleton: program entry, the run loop, layers, events, input, and the small
utilities everything else builds on. Files under `GanymedEngine/source/GanymedE/main/`,
`Core/` and `events/`.

## Entry point & Application

The engine owns `main()`. A client app implements one function and includes one header:

```cpp
// MyApp.cpp
#include <GanymedE.h>
#include <GanymedE/main/EntryPoint.h>   // exactly once, in one TU

namespace GanymedE {
    class MyApp : public Application {
    public:
        MyApp() : Application("MyApp") { PushLayer(new MyLayer()); }
    };
    Application* CreateApplication() { return new MyApp(); }
}
```

[`EntryPoint.h`](../../GanymedEngine/source/GanymedE/main/EntryPoint.h) initializes logging, stores
command-line args (`Application::GetCommandLineArgs()` — the editor uses them to open a scene passed
as `argv[1]`), then wraps startup/run/shutdown in three profiling sessions
(`GanymedEProfile-*.json`).

[`Application`](../../GanymedEngine/source/GanymedE/main/Application.h) is a singleton
(`Application::Get()`) that owns:

- the platform `Window` (created via `Window::Create`, event callback bound to
  `Application::OnEvent`),
- the `LayerStack` and the always-present `ImGuiLayer` overlay,
- the run loop (`Run()`): timestep from `glfwGetTime()`, `OnUpdate` for every layer (skipped while
  minimized), then the ImGui begin/render/end bracket, then `Window::OnUpdate` (poll events +
  present).

Application-level event handling: window close stops the loop; resize forwards to
`Renderer::OnWindowResize` (0×0 → minimized, updates are skipped); **F1** toggles the bgfx
stats/debug-text overlay.

## Layers

[`Layer`](../../GanymedEngine/source/GanymedE/Core/Layer.h) is the unit of app logic:
`OnAttach/OnDetach/OnUpdate/OnImGuiRender/OnEvent`. The
[`LayerStack`](../../GanymedEngine/source/GanymedE/Core/LayerStack.h) keeps ordinary layers in the
front half (insertion index) and overlays at the back, so overlays always update last and receive
events first.

- **Update order**: front → back. **Event order**: back → front (overlays first), stopping when a
  handler marks the event handled.
- `Layer::IsAttached()` exists because attachment can legitimately be skipped (it was gated during
  the bgfx migration): events are not delivered to unattached layers, and `~LayerStack` only calls
  `OnDetach` on layers that actually attached. `Application::Push*` sets the flag after `OnAttach`.

## Events

[`events/Event.h`](../../GanymedEngine/source/GanymedE/events/Event.h) defines a small
class-per-event hierarchy (window close/resize/focus/move, app tick/update/render, key
pressed/released/typed, mouse button/move/scroll — see `ApplicationEvent.h`, `KeyEvent.h`,
`MouseEvent.h`). Events are:

- **Blocking** — dispatched synchronously the moment GLFW reports them, not queued.
- **Typed** via the `EVENT_CLASS_TYPE`/`EVENT_CLASS_CATEGORY` macros, which give each class a static
  type tag and category bitmask.
- **Consumed** via `EventDispatcher`: `dispatcher.Dispatch<KeyPressedEvent>(fn)` calls `fn` only if
  the runtime type matches, and ORs the handler's `bool` return into the event's handled flag.
  Handled events stop propagating down the layer stack.

Handlers are bound with `GE_BIND_EVENT_FN(fn)` (lambda-based, preferred) or the older
`BIND_CALLBACK_FN(fn, this)` (`std::bind`).

The `ImGuiLayer` "blocks" events when the mouse/keyboard is captured by UI —
`EditorLayer` relaxes this while the viewport is hovered or focused so camera/gizmo input works.

## Input

[`Input`](../../GanymedEngine/source/GanymedE/Core/Input.h) is a static polling API:
`IsKeyPressed(Key::W)`, `IsMouseButtonPressed(Mouse::ButtonLeft)`, `GetMousePosition()`. Each
platform implements it over GLFW (`Platform/<OS>/<OS>Input.cpp`). Key and mouse codes
(`Core/KeyCodes.h`, `Core/MouseButtonCodes.h`) mirror GLFW's values in the `Key::` / `Mouse::`
namespaces, so no translation is needed.

Use events for edges (a key going down), polling for state (a key being held) — the editor camera
and gizmo shortcuts show both in use.

## Window

[`Window`](../../GanymedEngine/source/GanymedE/Core/Window.h) is the one abstraction that stayed
virtual (one implementation per OS): size, VSync, native handle, and the event callback.
`Window::Create` is implemented per platform and returns the platform window. See
[platform.md](platform.md) for the GLFW/bgfx wiring — notably `GLFW_NO_API` (bgfx owns the graphics
API) and the resize path that defers `bgfx::reset` to a frame boundary.

## Logging

[`Log`](../../GanymedEngine/source/GanymedE/Core/Log.h) wraps two spdlog loggers, both writing
color-coded to stdout and to `GanymedE.log`:

- `GE_CORE_*` — engine internals ("GANYMED" logger).
- `GE_*` — client/application code ("APP" logger).

Levels: `TRACE`, `INFO`, `WARN`, `ERROR`, `FATAL`. Format is `[HH:MM:SS] name: message`; messages
use fmt syntax (`GE_CORE_INFO("Loaded {0}", path)`).

## Asserts

From [`Core.h`](../../GanymedEngine/source/GanymedE/Core/Core.h): `GE_ASSERT(cond, msg)` (client)
and `GE_CORE_ASSERT(cond, msg)` (engine) log an error and `__debugbreak()`/`SIGTRAP` when the
condition fails. **They compile out entirely outside Debug** (`GE_ENABLE_ASSERTS` is tied to
`GE_DEBUG`), so never put side effects in an assert condition. The ECS leans on asserts heavily —
skipped reactive views, immediate structural changes during update, double-queued component adds
are all assert-time failures.

## Small types

- [`Timestep`](../../GanymedEngine/source/GanymedE/Core/Timestep.h) — a float of seconds with an
  implicit `float` conversion plus `GetSeconds()`/`GetMilliseconds()`.
- [`UUID`](../../GanymedEngine/source/GanymedE/Core/UUID.h) — a random `uint64_t`
  (`std::mt19937_64` seeded from `random_device`), hashable, with an *explicit* `uint64_t`
  conversion. Entity identity (`IDComponent`), asset handles (`AssetHandle`), and the physics
  body↔entity map all key off it. `UUID{0}` conventionally means "none" (no parent, invalid asset).
- `Ref<T>` / `Scope<T>` — aliases for `std::shared_ptr` / `std::unique_ptr` with
  `CreateRef`/`CreateScope` factories. Engine convention: resources shared across systems are
  `Ref`, uniquely-owned internals are `Scope`.
- `BIT(x)` — bit flags (event categories).
- `PlatformDetection.h` — defines exactly one of `GE_PLATFORM_WINDOWS/LINUX/MACOS` (and errors on
  everything else).

## Profiling

[`Debug/Instrumentor.h`](../../GanymedEngine/source/GanymedE/Debug/Instrumentor.h) writes
chrome://tracing-compatible JSON. `GE_PROFILE_FUNCTION()` / `GE_PROFILE_SCOPE("name")` place scoped
timers; `GE_PROFILE_BEGIN_SESSION`/`END_SESSION` bracket output files (the entry point produces
Startup/Runtime/Shutdown captures). Enabled with `GE_PROFILE`; when off, all macros compile to
nothing. Open the JSON in `chrome://tracing` or [Perfetto](https://ui.perfetto.dev).

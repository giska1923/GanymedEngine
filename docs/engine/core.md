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

### ApplicationSpecification

The name-only constructor above delegates to the spec constructor, which is what an app uses when
it wants anything other than the defaults:

| Field | Default | Meaning |
|---|---|---|
| `Name` | `"GanymedEngine"` | Window title |
| `Width` / `Height` | `DEFAULT_WINDOW_WIDTH/HEIGHT` (1600×900 on Windows, 640×480 elsewhere) | Windowed size; ignored when `Fullscreen` |
| `Fullscreen` | `false` | Borderless fullscreen — see [platform.md](platform.md#windows) |
| `EnableImGui` | `true` | `false` builds no `ImGuiLayer` at all |

`EnableImGui = false` is how a non-editor front-end (the standalone runtime) is hosted. It is a spec
field rather than a virtual hook because the `Application` constructor decides whether to push the
overlay, and a virtual dispatched from a base constructor sees the base vtable — a `UsesImGui()`
override could never fire. Two consequences worth knowing:

- `GetImGuiLayer()` returns null, and layers' `OnImGuiRender` simply never runs. **Check the pointer
  before dereferencing it**; the editor is the only call site that assumes it exists, and it does so
  legitimately because it *is* the ImGui front-end.
- Nothing installs ImGui's GLFW callbacks and nothing blocks events, so every event reaches the game
  layers raw. That is exactly what a shipped game wants and what `Input::` already assumes.

[`EntryPoint.h`](../../GanymedEngine/source/GanymedE/main/EntryPoint.h) initializes logging, stores
command-line args (`Application::GetCommandLineArgs()` — the editor uses them to open a scene passed
as `argv[1]`), then wraps startup/run/shutdown in three profiling sessions
(`GanymedEProfile-*.json`).

[`Application`](../../GanymedEngine/source/GanymedE/main/Application.h) is a singleton
(`Application::Get()`) that owns:

- the platform `Window` (created via `Window::Create` from a `WindowProps` built out of the spec,
  event callback bound to `Application::OnEvent`),
- the `LayerStack` and — when `ApplicationSpecification::EnableImGui` — the `ImGuiLayer` overlay,
- the run loop (`Run()`): timestep from `glfwGetTime()`, [`JobSystem::OnUpdate`](#job-system) (the
  main-thread job drain), `OnUpdate` for every layer (skipped while minimized),
  `AudioEngine::OnUpdate` (*not* skipped — a minimized window has not stopped making noise), then
  the ImGui begin/render/end bracket **if an `ImGuiLayer` exists**, then `Window::OnUpdate` (poll
  events + present).

The constructor brings up the engine's global subsystems in a fixed order — [`JobSystem::Init`](#job-system)
→ [`Reflection::Init`](scene.md#member-reflection) → `Renderer::Init` → [`AudioEngine::Init`](audio.md)
→ `ScriptEngine::Init` → `UIEngine::Init` — and
the destructor body tears them down in the reverse-ish order the dependencies actually require:
`UIEngine::Shutdown` (its Lua plugin holds references into the VM, and it releases GPU textures) →
`ScriptEngine::Shutdown` → `AudioEngine::Shutdown` → `JobSystem::Shutdown` → `Renderer::Shutdown`.
Audio has no dependency in either direction; it is placed where it is so the boot log reads in a
stable order.

`Reflection::Init` sits second because it depends on nothing — it only fills entt's meta context — so
everything constructed after it may assume every component is reflected. It has no matching
`Shutdown`: the meta context is owned by entt's locator and dies with the process, and there is no
foreign resource behind it. In Debug it self-validates and asserts, so a registration mistake
surfaces in the boot log rather than the first time a panel draws.

The two ends of `JobSystem`'s lifetime are **not** mirror images, and both positions are load-bearing.
It initializes *first*, before the window exists, because enkiTS numbers the thread that initializes
it as thread 0 — doing it here is what makes "thread 0" and "the thread that owns bgfx submission"
the same thread by construction, which is the only reason `JobSystem::IsMainThread()` can be trusted.
It shuts down *before* the renderer, not last, because `Shutdown` drains the main-thread queue one
final time and that queue is where the bgfx half of a background load lives; joining after
`Renderer::Shutdown` would hand a live `bgfx::create*` call a dead context.

**The destructor body runs before the members unwind**, so the `LayerStack` — and every layer's
`OnDetach` — happens *after* those shutdowns. That is why `AudioEngine` guards every public call
with an initialized flag and the GPU-resource destructors check `Renderer::IsGpuAlive()`. Anything a
layer touches during teardown must be safe to call dead.

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

## Job system

[`JobSystem`](../../GanymedEngine/source/GanymedE/Core/JobSystem.h) is a static facade over
[enkiTS](build-and-tooling.md#third-party-dependencies), and it exposes **exactly two shapes**:

```cpp
// Data-parallel, blocking. 4096 is the grain size, and it is not optional.
JobSystem::ParallelFor(mipCount, 4096, [&](uint32_t begin, uint32_t end, uint32_t threadIndex) {
    for (uint32_t i = begin; i < end; ++i)
        buckets[threadIndex] += Encode(i);
});

// Independent background work, returns a cancellable handle.
Future<MeshData> job = JobSystem::Submit(JobPriority::Normal, [path] { return ParseGltf(path); });
// ... later, on the main thread:
if (auto data = job.Get())
    Upload(*data);
```

There is no task graph and no per-frame system scheduling; see
[`THREADING_ROADMAP.md`](../toDo&done/THREADING_ROADMAP.md) for why that was ruled out rather than
deferred, and for the survey of the alternatives (cpp-taskflow, Marl, in-house) that landed on enkiTS.

**`ParallelFor(count, minRange, fn)`** splits `[0, count)` across workers and does not return until
every range has run, so it needs no handle and its task object lives on the stack. `minRange` is the
grain size and has **no default on purpose**: enkiTS's guidance is a range worth at least ~10k cycles,
and `minRange = 1` over cheap per-item work spends more on scheduling than on the work — the classic
way to make a system slower and conclude that threading does not help. `threadIndex` is
`0..ThreadCount()-1` and exists for indexing per-thread output buckets, not for branching on which
thread you are.

**`Submit(priority, fn) -> Future<T>`** returns a move-only handle whose **destructor cancels and
waits**. That is the load-bearing property of the whole subsystem rather than a convenience: a
dropped `Future` must never leave a worker writing into state its caller has already destroyed. The
cost is stated plainly — *destroying a `Future` can block*. If that matters at a call site, move the
handle somewhere that outlives the work; there is no detach.

**Cancellation is cooperative, and "cancel" does not mean "dequeued".** enkiTS has no cancellation
API at all, so `Future::Cancel()` sets a flag: a job that has not started yet still runs, and simply
returns without touching its result slot (`Get()` then yields `std::nullopt`). A job already past
that entry check only stops if it asks, via `JobSystem::IsCurrentJobCancelled()` at coarse loop
boundaries. A reader who assumes cancel unqueues will get resource lifetimes wrong.

**`SubmitToMainThread(fn)` + `OnUpdate()` is the bgfx handoff.** A worker parses bytes; the "turn
those bytes into a GPU object" half is queued back to the thread that owns submission.
`OnUpdate` swaps the queue under a lock and runs it outside the lock, so work queued *from* a
main-thread job lands next frame by construction instead of extending the current drain
indefinitely. This is a plain queue rather than enkiTS's `IPinnedTask`/`RunPinnedTasks`: pinned tasks
are heap objects whose lifetime must be managed until they execute, and they give no clean "drain
exactly what was queued at frame start" boundary. The queue is ~15 lines and its semantics are ours.

`JobPriority::High / Normal / Low` map 1:1 onto enkiTS's three default tiers (frame work /
import-compile / speculative and background I/O), asserted with a `static_assert` on
`ENKITS_TASK_PRIORITIES_NUM` rather than translated. Waiting is bounded by the waited-on job's tier,
so a thread blocked on frame work pumps other frame work and is never dragged into background I/O.

Two behaviours worth knowing:

- **Every entry point works uninitialized**, by running the work inline on the calling thread. The
  asset compiler is meant to run from tools that never construct an `Application`, and a scheduler
  that *must* exist is one that gets a null check at every call site instead.
- **Jolt still owns its own pool** of `hardware_concurrency() - 1` threads
  ([physics.md](physics.md)), so today there are two pools of that size and the oversubscription is
  real. Consolidating Jolt onto `JobSystem` via `JPH::JobSystemWithBarrier` is the fix; shrinking
  this pool on a guess is not.

`RunSelfTest()` runs from `Init` in Debug builds and checks the three things worth catching at boot
rather than during a scene swap: a `ParallelFor` sum matches the serial result, a `Future` dropped
mid-flight really did wait, and a running body observes `IsCurrentJobCancelled()`. It sleeps ~20ms
deliberately — without that the second check would be a race it wins by luck.

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
- [`Random`](../../GanymedEngine/source/GanymedE/Core/Random.h) — a seedable PCG32. State is two
  `uint64_t`s (16 bytes), copyable: a copied `Random` continues the same sequence from the copy
  point without aliasing the source's stream, which is what `Scene::Copy` needs for per-emitter
  particle RNG. The `uint32_t` seed is expanded with splitmix64, then the PCG seed dance (odd
  increment, two dummy draws). `Float01()` is `[0, 1)` via division by 2^32 — no `std::`
  distribution object, whose sequence the standard does not pin across implementations.
  Direction-in-cone and other domain mapping stay at the call site. This is a different generator
  from `UUID`'s file-static `mt19937_64`; that one wants uncorrelated 64-bit ids, not a small
  replayable stream. Float determinism is best-effort IEEE-754, not a cross-platform promise.
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

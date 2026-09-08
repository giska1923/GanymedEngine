# Threading Milestone — Scope Sketch (`Core/JobSystem` on enkiTS)

Status: **scope sketch, not a phase plan.** Written 2026-09-08 against `master` at `ee4148f`. Like
[`REFLECTION_ROADMAP.md`](REFLECTION_ROADMAP.md) this fixes the decisions that are expensive to change
later — the library, the scope boundary, the API shape, the risks — and deliberately leaves the phases
coarse, because the first real consumer arrives in
[`ASSET_PIPELINE_ROADMAP.md`](ASSET_PIPELINE_ROADMAP.md) Phase 4 and the second in Phase 5. Detailing
steps now would be designing against requirements that do not exist.

The milestone thesis: Ganymed is single-threaded except for Jolt, which runs *its own* pool. What is
missing is not "threads" — `bx` already ships every primitive — but a **scheduler**: something that
turns "encode 400 texture mips" into parallel work, and "load this mesh" into a cancellable background
task whose GPU-side completion runs on the bgfx submit thread. Those two shapes, and nothing else.

---

## What BlankEngine does, and why most of it is not the interesting part

The investigation started from `D:\game\src\engine\engine\core\thread`. That directory is **738 lines
and contains no job system**:

| File | Lines | What it is |
| --- | --- | --- |
| `threadpool.hpp/.cpp` | 181 | Textbook pool: one mutex, `eastl::queue<std::function<void()>>`, `condition_variable_any`, `packaged_task`→`std::future`, a `m_busyThreads` counter. An `IObserver{onEnqueueTask, onBeforeExecute, onAfterExecute}` hook compiled only under `ENGINE_TRACK_ALLOCS`. |
| `trackable_mutex.hpp` | 166 | Tracy `Lockable`/`SharedLockable` wrappers behind `#define TRACK_MUTEXES 0` — compiled **off**, degrading to a name-discarding passthrough. Plus a hand-rolled `SharedRecursiveMutex` (recursive `std::shared_mutex` is UB) and PlayStation variants, because default-constructing condvars there consumes limited named kernel objects. |
| `thread_utils.hpp/.cpp` | 213 | `setThreadName`, `setThreadAffinity`, `threadPriority`/`setThreadPriority`/`setThreadRealtimePriority`, `currentThreadNativeHandle`. |
| `spin_lock.hpp` | 54 | rigtorp's spinlock — `exchange` acquire, relaxed spin, `_mm_pause`/`yield`. |
| `playstation_mutex.hpp` | 97 | Console mutex/condvar construction. |
| `movable_atomic.hpp` | 27 | `MovableAtomic<T>`, so atomics can live in vectors. |

The engine is `services/thread/thread_service.hpp` (576) + `.cpp` (921), and its content is a wrapper
around **cpp-taskflow**. BlankEngine did not write a scheduler; it bought one and spent ~1,500 lines making
it fit an engine. Its own `ThreadPool` survives for exactly one purpose: backing `CustomThread`, where
each dedicated thread is a `core::ThreadPool(1, initFn)`.

`ThreadService` runs **two** `tf::Executor`s. Foreground (frame work) is sized
`hardware_concurrency()/2` floored at 4. Background (IO and long operations, priority-tiered via
`tf::Executor::priority_async`) is **2 in Debug / 4 in Release**, with editor, unit-test and validation
builds getting `max(4, hc/2)`, per-platform constants for Xbox, PS5 and Steam Deck, and overrides from
engine cfg plus per-domain project `ThreadSettings`. Note that `fg + bg + main` deliberately exceeds
the core count: background threads are blocked on IO, so oversubscribing them costs almost nothing and
hiding IO latency is worth more than avoiding context switches. `executorConfig(foregroundWorkers)`
tunes taskflow's `max_steals_before_yield` and `max_yields_explore`; a `WorkerInterface` hook names
each thread and registers it with the profiler under a
`ThreadGroupId{Main=0, Foreground=1, Background=2, Custom=3}` that doubles as a profiler sort key.

### The five mechanisms worth lifting

These are the things a first attempt at a job system always gets wrong, and they are the actual value
in `thread_service.hpp`:

1. **`Future<T>` whose destructor cancels *and waits*.** It wraps `tf::Future<T>` adding
   `cancelIfPending()`, `ready()`, and a `reset()` in the destructor that cancels then blocks until the
   task is provably not running. A dropped future can therefore never leave a task writing into freed
   state. Without this you get a bug that reproduces on one machine, in Release, once a week.
2. **`addMainThreadTask` drained once per frame** — swap-under-lock, then run outside the lock, so
   main-thread tasks spawned *from* a main-thread task land next frame by construction:
   ```cpp
   MainThreadTasks tasks;
   {
       const std::scoped_lock lock(m_mainThreadTasksMutex);
       eastl::swap(tasks, m_mainThreadTasks);
   }
   for (auto& [_, task] : tasks) { task(); }
   ```
   This is not optional for Ganymed — it is the only correct shape for "parse bytes on a worker, call
   `bgfx::createTexture2D` on the submit thread".
3. **`loopUntil(predicate, callIntervalMs)`** — a waiting worker pumps other queued tasks instead of
   blocking, so a task waiting on a task cannot deadlock the pool. It ships with a wart:
   `maxRecommendedTasksSizeWithLoopUntil()` = `ObserverInterface::C_STACK_SIZE * numThreads / 4`,
   because nesting these overflows the observer stack. A heuristic guarding a structural problem.
4. **`thisThreadTaskIsCancelled()`** — cooperative cancellation polled inside a long body.
5. **Task wrappers as the cross-cutting seam.** `wrapWithLoggerContext` propagates per-thread logger
   context across the enqueue boundary; without it every log line from a worker loses which asset or
   system it belonged to. `wrapEditorCallback` adds telemetry timers and the background-activity
   progress UI.

### What is deliberately not being copied

- **Two executors.** Justified there by different steal tuning per tier. One scheduler with priority
  tiers is enough at Ganymed's scale; a second pool is oversubscription that would need measurements
  to defend, and there are none.
- **`TrackableMutex`.** 166 lines switched off by its own `#define`. Add lock instrumentation when
  there is a measured contention problem.
- **`SharedRecursiveMutex`.** A recursive reader-writer lock is a design smell that papers over
  unclear ownership. Fix the ownership instead.
- **The eight-overload `wrapEditorCallback` SFINAE set** (const/mutable × void/non-void ×
  accepts-`Progress`/not). One `std::function<void(float)> progress` parameter does this.
- **`MovableAtomic`, `SpinLock`, `thread_utils`, the whole primitives drawer.** Already covered by
  `bx` — see below.

---

## Decision 1 — the primitives are already vendored; only the scheduler is missing

`bx` is a submodule and provides `thread.h`
(`Thread::init(ThreadFn, void*, uint32_t stackSize, const StringView& name)` at
[`thread.h:42`](../../GanymedEngine/extern/bx/include/bx/thread.h#L42), i.e. named threads for free),
`mutex.h`, `semaphore.h`, `mpscqueue.h`, `spscqueue.h`, `ringbuffer.h`, `cpu.h`, `os.h`, `timer.h`,
`handlealloc.h`. So BlankEngine's entire `core/thread/` layer is already available without writing a line.

What `bx` does **not** have is a scheduler or a task graph — exactly the piece BlankEngine also did not
write. That is the whole scope of this milestone.

---

## Decision 2 — enkiTS, not cpp-taskflow, and not in-house

The consumer list below is either a flat parallel-for or an independent background task. **No consumer
wants a task dependency graph.** BlankEngine's `tf::Taskflow` usage exists to drive its per-frame *service
execution graph* — precisely the thing
[`ASSET_PIPELINE_ROADMAP.md`](ASSET_PIPELINE_ROADMAP.md) already decided Ganymed does not want. So
taskflow's differentiating feature is the one that has been ruled out, leaving only its costs: heavy
header-only template code in every TU that touches threading, plus the deprecation-warning workaround
BlankEngine needed (`core/integration/taskflow_integration.hpp`, 29 lines suppressing warnings from
taskflow's `std::stringstream` use, with an include-order guard on `TF_CACHELINE_SIZE`).

**enkiTS** (Doug Binks, zlib, ~2 files + an optional C API, C++11 with C++14 only feature-tested for
`[[deprecated]]`) maps onto the consumer list unusually well. Verified against
`src/TaskScheduler.h` on `master`:

| Need | enkiTS |
| --- | --- |
| Parallel-for | `ITaskSet::ExecuteRange(TaskSetPartition range_, uint32_t threadnum_)` with `m_SetSize` and `m_MinRange` (grain size; the header advises ≥10k cycles of work per range) |
| bgfx submit-thread handoff | `IPinnedTask` + `AddPinnedTask` + **`RunPinnedTasks()`** as the per-frame drain — a first-class concept, where BlankEngine hand-rolled it |
| Priority tiers | `enum TaskPriority { TASK_PRIORITY_HIGH = 0, … TASK_PRIORITY_LOW }`, count set by `ENKITS_TASK_PRIORITIES_NUM` (default 3, settable 1–5) |
| Deadlock-free waiting | `WaitforTask(const ICompletable*, TaskPriority priorityOfLowestToRun_)` pumps other tasks while waiting — and the priority bound means waiting on a high-priority task will not drag the waiter into running low-priority IO. Better than `loopUntil`, which has no such bound |
| Enqueue / completion | `AddTaskSetToPipe(ITaskSet*)`, `ICompletable::GetIsComplete()` |
| Light dependencies | `SetDependency` / `SetDependenciesArr` / `SetDependenciesVec` + `OnDependenciesComplete` — enough for "compile then upload" without a graph DSL |
| Lambdas without subclassing | `TaskSet` (over `TaskSetFunction`) and `LambdaPinnedTask` (over `PinnedTaskFunction`) |
| Config | `TaskSchedulerConfig{numTaskThreadsToCreate = GetNumHardwareThreads()-1, numExternalTaskThreads, profilerCallbacks, customAllocator}`, plus `Initialize()` / `Initialize(uint32_t)` / `Initialize(TaskSchedulerConfig)` |
| Shutdown | `WaitforAllAndShutdown()`. **`ShutdownNow()` leaves queued tasks "in an undefined state in which should not be re-launched"** — so it is the fast-exit path only, never normal shutdown |

**The in-house alternative, stated fairly:** a single-queue pool over `bx::Thread` + `Semaphore` +
`MpScUnboundedBlockingQueue` is genuinely ~250 lines and adds no dependency. It is defensible if async
asset IO is the *only* consumer, and it degrades the moment consumer #1 (BCn encode) arrives, because
then range splitting, work stealing and deadlock-free waiting get hand-written — which is exactly
where the bugs live. **Marl** (Google, Apache-2.0) was considered and rejected: fibers let a task block
without stalling a thread, which suits IO, but they wreck profilers and debuggers, need per-platform
context-switch assembly, and its current maintenance activity is something I could not confirm.

---

## Decision 3 — what we still write ourselves: `Future<T>` and lifetime

enkiTS has **no cancellation API**. Verified: no per-task `Cancel()`, no cancellation token, nothing
aborts a running `ExecuteRange`, and no way to dequeue a pending task. The only cooperative signals are
`GetIsShutdownRequested()` and `GetIsWaitforAllCalled()`, which long loops poll themselves. It also
holds **raw `ITaskSet*` / `IPinnedTask*` pointers it does not own**, so a task object destroyed before
its execution finishes is a use-after-free with no diagnostic.

Both facts point at the same ~150–250 lines, and they are the BlankEngine shape:

- `Job<T>` / `Future<T>` over a `shared_ptr<State>` holding the task object, an atomic status, the
  result, and a cancel flag. The shared state owning the task object solves the lifetime problem;
  `GetIsComplete()` is the retirement condition.
- **Cancel means "still runs, returns immediately"** — not "dequeued". The task body checks the flag at
  entry and at coarse loop boundaries. This is the mental model to write into the docs, because a
  reader who assumes cancel dequeues will get the resource lifetime wrong.
- **The destructor cancels and waits.** Mechanism 1 above, and the single most important line in the
  milestone. `AssetManager` will otherwise leak tasks writing into freed resources during scene swaps.
- `Assert(IsMainThread())` on every bgfx-touching path, per asset-roadmap decision 13.

---

## Decision 4 — one scheduler, priority-tiered, no second pool

`TaskSchedulerConfig{numTaskThreadsToCreate = hardware_concurrency() - 1}` and the three default
priorities. `HIGH` = frame-critical parallel-for, `MED` = import/compile, `LOW` = speculative and
background IO. BlankEngine's fg/bg split is a scale answer to a problem Ganymed does not have yet; if a
measurement later shows IO starving frame work, raise `ENKITS_TASK_PRIORITIES_NUM` before adding a
second scheduler.

**Jolt keeps its own pool until this milestone's second consumer lands, then gets consolidated.** Today
[`PhysicsScene.cpp:237`](../../GanymedEngine/source/GanymedE/Physics/PhysicsScene.cpp#L237) constructs
`JPH::JobSystemThreadPool` with `hardware_concurrency() - 1` threads, so adding a scheduler creates a
second pool of the same size and genuine oversubscription. Jolt exposes an abstract `JobSystem` with
`JobSystemWithBarrier` already implementing the barrier half, so subclassing it onto `Core/JobSystem`
is a well-trodden path and the right fix. It is a consolidation, not a speedup — schedule it for the
payoff of one pool, and expect no frame-time win.

---

## The consumers, ranked — and why the first one should be the boring one

1. **BCn encode at texture import** (asset Phase 4). `bimg::imageEncode` is CPU-bound and
   embarrassingly parallel over mips and blocks. Biggest wall-clock win available, and the **safest
   first consumer**: offline work, no frame budget, no lifetime hazards, and a failure costs import
   time rather than a corrupted frame.
2. **Async asset load** (asset Phase 5). Background queue with priorities, cancellable typed futures,
   pinned-task handoff for bgfx resource creation. This is where the hard bugs are.
3. **Mesh import** (cgltf → binary cache) and **shader compile** (today a batch script shelling to
   shaderc) — trivially parallel over files.
4. **Jolt consolidation** (decision 4).
5. **Particle sim** ([`ParticleSystem.cpp`](../../GanymedEngine/source/GanymedE/Scene/Systems/ParticleSystem.cpp),
   200 lines) and **animation**
   ([`AnimationSystem.cpp`](../../GanymedEngine/source/GanymedE/Scene/Systems/AnimationSystem.cpp),
   199) — parallel over emitters/animators is trivial with `ITaskSet`. Worth doing when counts are
   large enough to measure. **Not a reason to build anything**, and per-particle parallelism inside one
   emitter wants an SoA pool rework that is its own milestone.

Recommended ordering: prove the scheduler on consumer 1, then take consumer 2. That inverts asset
Phase 5's current step 1, which builds the job system as a prerequisite of async loading — the
scheduler should exist by then, exercised.

**The honest sequencing note: this milestone has no consumer until asset Phase 4.** `AssetRef<T>`'s
design already absorbs sync→async with no call-site churn (asset-roadmap Phase 3 → 5), so nothing is
blocked by waiting. Building it earlier means designing against imagined requirements — the same
argument used to defer the reflection milestone.

---

## Sketched phases

Deliberately coarse. T3 and T4 depend on asset Phases 4 and 5 respectively.

**T1 — Vendor and wrap.** enkiTS as a submodule under `GanymedEngine/extern/`, added to the premake
build (project regeneration required). `Core/JobSystem.h/.cpp`: scheduler lifetime tied to
`Application`, `Initialize(TaskSchedulerConfig)`, `WaitforAllAndShutdown()` on teardown,
`ParallelFor(count, minRange, fn)`, `Enqueue(priority, fn) -> Future<T>` with the decision-3 shared
state, `RunPinnedTasks()` drained once per frame from `Application::Run`, and `IsMainThread()`.
Verification is a test, not a feature: a `ParallelFor` summing a large array matches the serial
result; a `Future` dropped mid-flight does not use freed state under a debug allocator; shutdown with
work queued exits cleanly.

**T2 — Thread naming and profiler wiring, up front rather than retrofitted.** enkiTS's
`ProfilerCallbacks` hook worker start/stop/wait. Ganymed's profiler is `Debug/Instrumentor.h`, a
Chrome-trace writer currently **compiled off** (`GE_PROFILE 0` at
[`Instrumentor.h:204`](../../GanymedEngine/source/GanymedE/Debug/Instrumentor.h#L204)) whose global
mutex makes it a poor fit for per-task granularity. So: name the threads and wire the callbacks to
whatever `GE_PROFILE_*` resolves to, and treat "is a real frame profiler worth adopting" as a separate
question. Do not fold a Tracy adoption into this milestone.

**T3 — First consumer: parallel BCn encode in the asset compiler.** Lands with asset Phase 4.
Verification is a measured before/after import time on a real scene, reported as numbers.

**T4 — Second consumer: async load.** Lands with asset Phase 5, which owns the risks. This milestone
contributes the pinned-task drain, the cancel-and-wait `Future`, and the main-thread assert.

---

## Risks

- **enkiTS does not own task objects.** Decision 3. The single most likely way this milestone produces
  a use-after-free.
- **`ShutdownNow()` is not shutdown.** It leaves queued tasks undefined by its own documentation. Use
  `WaitforAllAndShutdown()`; reserve `ShutdownNow()` for a crash-exit path if one is ever wanted.
- **Thread numbering and external-thread registration need checking before use.** `GetThreadNum()`,
  `RegisterExternalTaskThread()`, `GetNumFirstExternalTaskThread()` (returns 1) and
  `NO_THREAD_NUM = 0xFFFFFFFF` interact in a way that determines *which* thread number `IPinnedTask`
  must target to hit the bgfx submit thread. Verify against the header at T1 rather than assuming
  thread 0.
- **`m_MinRange` chosen badly makes `ParallelFor` slower than serial.** The header's guidance is
  ≥10k cycles of work per range. A per-particle `ParallelFor` with `m_MinRange = 1` is the classic way
  to make a system slower and conclude threading does not help.
- **Oversubscription from two pools** until Jolt is consolidated (decision 4). Measure before assuming
  it is harmless; BlankEngine gets away with it because its second tier is IO-blocked, and Jolt's is not.
- **The scheduler grows.** If work stealing tuning, a per-frame graph, or a second executor starts
  being discussed without a profile that demands it, stop — that is the failure mode the asset
  roadmap's Phase 5 risk section already names.

---

## Explicitly out of scope

- **A per-frame system execution graph.** BlankEngine's `ExecutionGraph`/`ServiceManager` model. Ganymed's
  ECS scheduling is explicit and readable; making it a graph is a large change with no measured
  problem behind it.
- **Fibers.** Decision 2.
- **Porting BlankEngine's primitives drawer.** Decision 1 — `bx` covers it.
- **Lock instrumentation.** BlankEngine's own is compiled off.
- **Adopting a new frame profiler.** T2 names the mismatch; resolving it is a separate decision.
- **Multithreaded bgfx submission.** bgfx has its own render-thread model; using it is a rendering
  milestone question, not this one, and asset-roadmap decision 13 assumes the single-submit-thread
  model throughout.

---

## Open questions to settle before T1

1. Which thread number is the bgfx submit thread from enkiTS's point of view, and does the main thread
   need `RegisterExternalTaskThread()` to participate in `WaitforTask` pumping? Determines the
   `IPinnedTask` contract. Header check, not a design decision.
2. `numTaskThreadsToCreate`: `hardware_concurrency() - 1` while Jolt still owns its own pool, or a
   smaller count until decision 4's consolidation lands? Wants a measurement on a real scene, not a
   guess.
3. Does `Core/JobSystem` expose enkiTS types (`TaskPriority`, `ICompletable`) or fully wrap them?
   Wrapping costs a translation layer and buys the ability to change scheduler later; exposing is
   honest about the dependency. Leaning toward a thin wrapper that exposes `TaskPriority` directly and
   hides everything else, but this is worth deciding once rather than drifting.

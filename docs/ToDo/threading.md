# ToDo — Threading

T1–T4 of [`THREADING_ROADMAP.md`](../history/THREADING_ROADMAP.md) are done: the enkiTS-backed
scheduler, thread naming and profiler wiring, parallel BCn encode, and async asset loading. See
[core.md](../engine/core.md#job-system) for what exists today.

One item is left, and it is the roadmap's own Decision 4.

---

## Consolidate Jolt onto `JobSystem` (Decision 4)

**The engine runs two thread pools of `hardware_concurrency() - 1` each.** `JobSystem::Init` creates
one; `PhysicsScene` constructs Jolt's own `JPH::JobSystemThreadPool` with the same count. On a
16-thread machine that is 30 workers for 15 usable cores.

Measured rather than assumed — the T2 verification probe enumerated the editor's threads and found
**49 threads in the process, 16 of them `JobSystem`'s** (`GE Main` plus `GE Worker 1..15`). Jolt's
pool is most of the remainder.

**What it needs:** implement Jolt's `JPH::JobSystem` interface on top of `Core/JobSystem` — in
practice deriving from `JPH::JobSystemWithBarrier`, which supplies the barrier half and leaves the
queue half to the host scheduler — and hand that to `PhysicsScene` instead of
`JobSystemThreadPool`. See [physics.md](../engine/physics.md) for the current construction.

**Why it has not been urgent:** physics and job work rarely peak in the same window, so the
oversubscription has not shown up as a frame spike. It is still real, and it gets worse as either
side grows.

**What not to do instead:** shrinking either pool on a guess. The roadmap is explicit that
consolidation is the fix — two pools sized to *half* the cores each is a worse failure mode than
two pools sized to all of them, because it caps both subsystems even when only one is busy.

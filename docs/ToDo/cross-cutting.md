# ToDo — Cross-cutting

Gaps that belong to no single subsystem.

---

## Linux and macOS are unbuilt and unverified

The engine is written for all three platforms — `PlatformDetection.h` defines the macros,
`premake5.lua` carries the link lists, the shader toolchain has `.sh` twins of every `.bat`, and
platform-specific paths exist throughout (`pthread_setname_np`, GLFW native handles, the `metal`
shader profile). **None of it has been compiled or run.** Every verification claim in
[`docs/history/`](../history/) is Windows/x64/D3D11.

The two most likely places to break first, because they are compile-time rather than runtime
choices:

- **The shader profile set.** `ProfileDirectory()` in `Shader.cpp` asks for `compiled/metal/` on
  macOS; if the profile folder ↔ backend mapping and the compile script disagree, every shader
  silently fails to load. See [build-and-tooling.md](../engine/build-and-tooling.md).
- **Clip-space depth.** Handled: §9.3 made projections caps-driven and **OpenGL 3.3 now renders
  correctly on Windows**, which is the same backend a Linux build defaults to. That removes the
  largest known obstacle, though it says nothing about the platform layer itself (GLFW native
  handles, `pthread_setname_np`, file paths).

Not scheduled, because a platform you do not build is a platform you do not support, and deciding to
support one is a bigger call than a ToDo entry. Recorded so the claim "cross-platform" is read with
the right caveat.

## The frame loop is nearly uninstrumented

There are ~90 `GE_PROFILE_FUNCTION` scopes in the engine, but almost none in the per-frame path — so
with `GE_PROFILE` on, `GanymedEProfile-Runtime.json` is mostly asset and job activity rather than a
frame breakdown. A trace that looks empty is usually that, not a broken session.

Found during T2. Deliberately not fixed there: scattering scopes through the frame loop is a
different decision from wiring the scheduler's callbacks, and it runs into the same objection T2
recorded — `Instrumentor::WriteProfile` takes a process-wide mutex and flushes per record, which is
a poor fit for per-frame granularity.

**The real question behind this one is whether to adopt a frame profiler** (Tracy being the obvious
candidate) rather than to instrument more of the frame by hand.
[`THREADING_ROADMAP.md`](../history/THREADING_ROADMAP.md) deliberately kept that question separate
from T2, and it is still separate — but it is the decision that would resolve this item, so making
it is the next step rather than adding scopes.

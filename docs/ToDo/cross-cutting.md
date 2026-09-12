# ToDo — Cross-cutting

Gaps that belong to no single subsystem.

---

## macOS is unbuilt and unverified

**Linux is done** — built (Debug, Release, Dist) and run, with the status table and the conformance
findings in [build-and-tooling.md](../engine/build-and-tooling.md#platform-status).

macOS has still never been compiled. Every verification claim in [`docs/history/`](../history/) is
Windows, and now Linux; nothing is clang, Xcode, Metal or arm64. The places most likely to break,
narrowed by what Linux actually hit:

- **The shader profile set.** `ProfileDirectory()` in `Shader.cpp` asks for `compiled/metal/` on
  macOS; if the profile folder ↔ backend mapping and the compile script disagree, every shader
  silently fails to load. Linux did not test this — it shares the `glsl` profile with Windows'
  OpenGL path, and those shaders loaded unchanged.
- **The `angledIncludeDirs` / `xcode4` machinery** in the workspace premake exists solely for
  Xcode and has never been exercised. Same for the `.app`-bundle avoidance in `GanymedEditor`'s
  `kind`.
- **arm64.** The workspace pins `x86_64` and every platform repeats the AVX2 flag set. A native
  Apple-silicon build needs those dropped and `JPH_USE_*` swapped for Jolt's NEON path — that is a
  port, not a build fix.
- **The link lists.** Linux proved these go stale silently: `enkiTS` and `TextureEncode` were in
  the engine's `links` and nowhere else, and only GNU ld noticed. The macOS lists were fixed in the
  same change, but by inspection, not by linking.

Not scheduled, for the reason the Linux entry carried: a platform you do not build is a platform you
do not support, and deciding to support one is a bigger call than a ToDo entry.

## The Linux build is verified in WSL2, not on a real Linux machine

Distinct from the above, and smaller. The Linux build and run happened under WSL2 (Ubuntu 22.04,
gcc 11.4), which leaves three things genuinely untested rather than merely unmentioned:

- **Vulkan.** WSL has no Vulkan loader, so bgfx fell through to OpenGL. On a native Linux box
  Vulkan is the backend bgfx picks first, and it is therefore the one that matters most there.
  It *is* verified on Windows, pixel-identical to D3D11, so the risk is the platform glue
  (GLFW native handles, surface creation) rather than the renderer.
- **The GL driver stack.** WSLg's Mesa served GL through its **d3d12 gallium driver**
  (`D3D12 (Intel(R) UHD Graphics)`). Hardware-accelerated, but not what a native user runs.
- **Audio.** miniaudio selected its Null device because WSL exposes none, so ALSA and PulseAudio
  were never opened. It degraded cleanly, which is worth something, but it is not a test.

A single run on a native Linux box would close all three at once. Nothing here is known to be
broken; it is simply unmeasured, and recorded so the Linux row is not read as more than it is.

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

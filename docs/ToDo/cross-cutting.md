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

## A frame profiler (Tracy) is still worth considering

The frame loop **is** instrumented now, and the `Instrumentor` behind it was rewritten to afford it
(per-thread buffers, ~72 ns a scope against ~3–7 us before) — see
[build-and-tooling.md](../engine/build-and-tooling.md#profiling--debug-tooling). What is closed is
"there is no frame breakdown". What stays open is the question T2 kept separate, now with the cheap
option actually taken rather than assumed:

Tracy would add, and the current writer will not grow cheaply:

- **A live view while the app runs**, instead of open-the-app, close-it, load a JSON. This is the
  big one, and it is most of why people adopt it.
- **GPU zones.** The engine's GPU time comes from the bgfx stats overlay (F1) and sits in a
  different window from the CPU trace, so correlating a slow frame to a slow pass is done by eye.
- **Lock contention and allocation tracking**, neither of which this writer can see at all.
- **Statistics across frames** — the current workflow answers "what happened in this capture" by
  loading it into Python, which is how the numbers in the build doc were produced.

Against: it is a new third-party dependency plus premake integration plus a viewer binary to build,
and the chrome://tracing pipeline now does the thing it was failing to do. Nothing is blocked on it.

The honest trigger for revisiting: the first time a frame problem needs GPU and CPU on one timeline,
or the first time the capture-then-load loop is the slow part of an investigation.

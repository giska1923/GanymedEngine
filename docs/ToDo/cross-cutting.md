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

## Removing a project leaves its generated files behind

`GanymedEngine/extern/Glad/` still exists in working trees that predate the bgfx migration: a
`Glad.vcxproj`, its `.filters`, and `bin/`/`temp/` Debug folders. The Glad *project* was deleted
correctly — [`BGFX_MIGRATION.md`](../history/BGFX_MIGRATION.md) records it, nothing in
`premake5.lua` includes it, and the loader's headers are gone. What survived is generated output.

**A fresh clone never sees it**, which is why it went unnoticed for so long: `.gitignore` excludes
`**/extern/**/*.vcxproj`, `**/extern/**/bin/` and `*.make`, so none of it was ever tracked.
`git ls-files GanymedEngine/extern/Glad` returns nothing.

So this is not a repository bug and the fix is not a commit. The general shape is that **premake
never deletes project files for a project it stops generating, and git cannot clean what it does
not track** — so every project removed from the workspace leaves that residue on every machine
that built it before the removal. Glad is just the one that has sat there longest.

Checked at the same time, since it is the question the above raises: the twenty `.vcxproj`/`.make`
files at `extern/`'s root all correspond to live projects — `bx`, `bimg` and `bgfx` from
`bgfx.lua`, the other seven from their own `.lua`. Glad is the only orphan.

Options, in ascending order of effort:

- Delete the directory by hand on any machine that has it. No repo change, and it does not come
  back.
- `git clean -xdf` also does it, but it destroys every build output and the `.compiled/` asset
  caches with it — a full rebuild plus a texture recompile to remove one stale folder.
- Teach the generate scripts in `scripts/` to reap `extern/*.vcxproj` whose `.lua` no longer
  exists. Recorded for completeness, not recommended: projects leave this workspace rarely, and a
  script that deletes build files by pattern is a worse failure mode than the folder it cleans.

## Nothing installs the Linux system packages, and `setup_dependencies.sh` is stale

Found while verifying the Sandbox removal: a Linux build in the project's own WSL image now fails in
bgfx with

```
bgfx/3rdparty/khronos/vulkan-local/vulkan.h:52:10: fatal error: xcb/xcb.h: No such file or directory
```

`renderer_vk.h:15` defines `VK_USE_PLATFORM_XCB_KHR` unconditionally on Linux, so bgfx's Vulkan
path needs the xcb headers. Both `/usr/include/xcb/xcb.h` and `/usr/include/X11/Xlib.h` are absent
from that image — only the *runtime* libraries (`libx11-6`, `libx11-xcb1`) are installed, not the
`-dev` packages. GLFW's X11 backend needs `Xlib.h` for the same reason.

**No script in this repository installs them.** `scripts/setup_dependencies.sh` is named as though
it would, but it only runs `git submodule update --init --recursive` and then checks that submodule
files exist. The system-package step does not exist anywhere, so the Linux instructions are
incomplete by exactly the amount that stops a fresh machine from building — which is the same gap
recorded above under WSL-vs-native, seen from the other side.

While reading it, three separate staleness bugs in that one script:

- It checks for `GanymedEngine/extern/Glad/premake5.lua` and reports a **failure** if it is missing.
  Glad was deleted by the bgfx migration. On a correct fresh clone this script now says
  `❌ Glad premake5.lua missing` and prints the "some dependencies are still missing" path.
- It points at `./scripts/setup_premake_Unix.sh` and `./scripts/GenerateProjects_Unix.sh`. Neither
  exists; the real names are `setup_premake.sh` and `Linux_GenerateProjects.sh`.
- It never mentions the system packages above.

Unlike the untracked `extern/Glad/` folder in the entry above, **this one is committed**, so every
clone has it. Worth fixing together: add the apt/dnf package list, drop the Glad check, correct the
two script names.

## Nothing prunes stale objects out of the Linux static archives

The sharper-toothed relative of the two entries above, found while verifying the Sandbox removal.
A clean-looking Linux Debug build failed to link `GanymedEditor` with 22 undefined references to
`glad_gl*`, raised from `Platform/OpenGL/OpenGLTexture.cpp` — **a file the bgfx migration deleted**.
It is not on disk, not tracked, and no premake file mentions glad. What survived is nine object
files still sitting inside `bin/Debug-linux-x86_64/GanymedEngine/libGanymedEngine.a`:

```
OpenGLBuffer.o  OpenGLContext.o  OpenGLFramebuffer.o  OpenGLRendererAPI.o  OpenGLShader.o
OpenGLTexture.o OpenGLVertexArray.o  OpenGLEnvironment.o  OpenGLUniformBuffer.o
```

Cause: `ar -rcs` merges into an existing archive rather than rebuilding it, so a deleted source's
object is never evicted. Release and Dist were unaffected only because those object trees were
created after the migration. Deleting the three `bin`/`temp` Linux trees and rebuilding gives a
clean result in all three configurations, so **there is nothing to fix in the source** — the
question is whether the build should defend against it.

Options, none of them obviously right:

- **Nothing.** Document the trap (done — see
  [build-and-tooling.md](../engine/build-and-tooling.md)) and delete the tree when it bites. It has
  bitten once in the project's life.
- **Have the generate scripts `make clean` when the source list changes.** Correct, and it throws
  away a full dependency rebuild every time a file is added — minutes, for a problem measured in
  years.
- **Switch the archive rule to delete the `.a` first.** One line in the gmake template, but that
  template is premake's, not ours, so it means carrying a customisation in `vendor/premake`.

**What this finding actually costs is the confidence in an earlier claim.** `docs/history/` records
the Linux bring-up as "Debug, Release and Dist all build clean". That run was reading this archive,
so it did not link Debug from scratch, and the Debug result in that record is weaker than it reads.
The history file is immutable and correct as written, so the correction is recorded here instead.

## The first frame's timestep is over a second

Measured while verifying P0.2's rotation lock, from a Lua script logging its own `ts`:

```
f1 ts=1.3764    f2 ts=0.0472    f3 ts=0.0076    f4 ts=0.0039    f5 ts=0.0033
```

Frame 1 covers boot — asset scan, shader loads, the first mesh applies — and that whole interval
is delivered to gameplay as one timestep. Frame 2 is still 10x a normal frame.

Physics survives it: `PhysicsSystem` accumulates and clamps to `MaxStepsPerFrame`, so the
spiral-of-death guard absorbs the spike. **Gameplay does not.** Any script that accumulates time
sees more than a second elapse before it has run once, which silently broke this probe's own
`if t < 0.5 then push() end` gate - the gate was already false on the frame it first ran, so the
push never happened and the test reported a clean pass for the wrong reason. A cooldown, a spawn
timer, or a "wait half a second then do X" in the game will all misfire the same way.

The usual fix is to clamp the delta handed to layers - most engines cap it around 0.1-0.25 s - and
optionally to discard the first frame outright. Neither is done here.

Not scheduled, because the right shape of the fix is a real decision: clamping changes what `ts`
means for everyone, and a game that legitimately hitches wants to know. Recorded because it will be
met again in [PROVING_GROUND.md](PROVING_GROUND.md)'s P1, and the failure looks like a gameplay bug
rather than a frame-timing one.

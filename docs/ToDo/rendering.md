# ToDo — Rendering backends

The unfinished tail of `BGFX_MIGRATION.md` Phase 7, the last milestone in
[`docs/history/`](../history/BGFX_MIGRATION.md) still flagged in progress (`🚧 STEP 1 DONE`).

**§9.3 (caps-driven projection), §9.2 (backend selection) and the multi-backend bugs they exposed
are all done, and all four backends now render.** Measured on the same scene, same frame:

| Backend | State | vs D3D11 |
|---|---|---|
| D3D11 | Renders correctly | reference |
| D3D12 | Renders correctly | **pixel-identical** |
| Vulkan | Renders correctly | **pixel-identical** |
| OpenGL 3.3 | Renders correctly | within 5/255 |

Zero bgfx fatals on any of them. What is left is not backend bring-up any more — it is the feature
checks nobody has run on any backend.

---

## Vulkan: works, but bgfx cannot start it on this machine unaided

Not an engine bug, and nothing here to fix. `renderer_vk.cpp` enumerates into a fixed
`VkPhysicalDevice[4]`, clamps the count to that size, then treats the resulting `VK_INCOMPLETE` as a
fatal init error — the clamp and the check contradict each other, since `VK_INCOMPLETE` is exactly
what Vulkan returns when you ask for fewer devices than exist.

This dev machine reports **six** (discrete NVIDIA, integrated Intel, a Mesa Dozen entry for each,
and the Basic Render Driver), so bgfx bails with
`Init error: vkEnumeratePhysicalDevices failed 5: VK_INCOMPLETE` and substitutes D3D11. With
`VK_LOADER_DRIVERS_SELECT=*nv*` the loader exposes one device and Vulkan starts and renders
correctly.

`GanymedEngine/extern/` is a submodule and off-limits, so the options are: report it upstream, carry
a patch outside the submodule, or leave the env-var workaround documented — which is what
[rendering.md](../engine/rendering.md#backend-selection) does. Worth reporting upstream; the fix is
two lines (accept `VK_INCOMPLETE`, or size the array from the first query).

## The test matrix is finished, and it left one real item

**Async picking**: done, and it was broken on *every* backend rather than just GL — see
[Entity picking](../engine/rendering.md#entity-picking-async).

**sRGB**: verified. The manual-gamma pipeline renders identically on all four backends — a textured
lit scene with a sky gradient gives the same mean channel value to two decimal places, mean
per-pixel difference 0.000, max 4/255. Nothing to fix; the captures are a baseline for whenever the
real sRGB pipeline lands (which is [its own scoped change](../engine/rendering.md#colour-space),
deliberately not this).

**MSAA**: checked, and it **does not work** — see below.

> The R32I worry recorded here earlier was a false alarm: `FramebufferTextureFormat::RED_INTEGER`
> resolves to **R32F**, not R32I, so the engine never asks for an integer target and GL's emulated
> R32I never mattered. It was written down from bgfx's capability table without checking what the
> engine actually requests.

## MSAA: deferred, with the cause found and the fix known

**Parked deliberately.** The abort is diagnosed and the fix is a single line (below), but enabling
MSAA is a feature decision rather than a bug fix, and the question worth answering first is whether
it is wanted at all: **FXAA already ships and runs correctly on all four backends**, and a
deferred-style HDR pipeline carrying an entity-id attachment is exactly the shape that makes MSAA
awkward. Resume from here if that answer ever becomes "yes".


`FramebufferSpecification::Samples` and `Framebuffer.cpp`'s `MsaaFlag` look like a working knob.
Nothing has ever set `Samples`, and setting it aborts the engine: with `Samples = 4` on the scene
target the process dies inside bgfx during framebuffer construction, before a frame is drawn, exit
code 3, nothing in the log — a `BX_ASSERT`, which does not travel through the bgfx callback that
catches `Fatal::` codes.

Narrowed down, so nobody repeats it:

- **Not** the entity-ID attachment — colour + depth alone aborts too.
- **Not** the sampler flags — dropping `BGFX_SAMPLER_U_CLAMP|V_CLAMP` aborts too.
- **Not** a format capability gap — RGBA16F, R32F and D24S8 all advertise MSAA framebuffer support
  in bgfx's caps table on D3D11.

Finding the assertion needs a debugger on a bgfx Debug build; it is below the callback's reach.

**And there is a second problem waiting behind the first.** The entity-ID attachment cannot be
resolved by averaging samples — the average of two entity ids is not an entity id — so picking needs
either its own non-multisampled pass or a custom resolve. Any MSAA work has to answer that before it
is worth starting.

This is a feature, not a fix, and it is unscheduled. The field now carries a comment saying so, since
it otherwise reads as a supported option.

### Phase 0 was run, and it answered the question

**The assertion, captured verbatim:**

```
bgfx.cpp(5141): ASSERT isOk() -> ErrorAssert: 0x02006762
`Frame buffer depth MSAA texture cannot be resolved. It must be created with either
 `BGFX_TEXTURE_RT_WRITE_ONLY` or `BGFX_TEXTURE_MSAA_SAMPLE` flag.`

  bgfx::Context::createFrameBuffer      bgfx_p.h:5921
  GanymedE::Framebuffer::Build          Framebuffer.cpp:118
  GanymedE::SceneRenderer::SceneRenderer SceneRenderer.cpp:29
```

**It is the depth attachment** — not the entity-ID attachment, which is what the earlier narrowing
had wrongly implied. "Colour + depth alone also aborts" was true and misleading: depth was present
in both cases, and colour-only was never tested.

**The fix is one line**, and it is a no-op until something sets `Samples`:

```cpp
// in Framebuffer::Build, per attachment
uint64_t flags = rtFlags;
if (m_Specification.Samples > 1 && IsDepthFormat(attachmentSpec.TextureFormat))
    flags |= BGFX_TEXTURE_RT_WRITE_ONLY;
```

`RT_WRITE_ONLY` rather than `MSAA_SAMPLE` because nothing samples the **scene** depth. The shadow
cascades *do* sample theirs (`Renderer3D.cpp:744`), but those framebuffers are single-sample, so the
`Samples > 1` guard keeps them untouched.

**Verified with that line applied**, on D3D11: the framebuffer constructs, no assertions fire, the
editor runs to a clean exit, and **picking returns the same grid map as single-sample** — both
entities, correct ids, correct positions.

**What is still unknown**, and why this is not landed:

- **Edge behaviour under the MSAA resolve is untested.** A sweep across a silhouette edge returned
  only `1` and `-1`, never an intermediate — so the "averaging corrupts ids" concern below is
  *unproven and may be wrong* for an R32F target on D3D11. It needs a deliberate test (a known
  sub-pixel edge, raw float readback) rather than the inconclusive sweep that was run.
- **No visual confirmation** that MSAA is actually anti-aliasing anything.
- **Only D3D11 was tried.** OpenGL, Vulkan and D3D12 are untouched.

So the abort is solved and the remaining work is the design question in Phase 3 plus that
verification. The repository is unchanged — the one-line fix above is written down rather than
applied, because enabling MSAA is still a decision rather than a bug fix.

#### How the assertion was captured, for next time

`bx`'s `defaultAssertHandler` already writes the condition, message, location **and a 27-frame stack
trace** to `bx::getDebugOut()` — `OutputDebugString` on Windows. With no debugger attached and no
monitor running, Windows discards it, which is why the process appeared to die in silence.

No debugger tooling is installed on this machine (no `cdb`, no DebugView), so the capture used a
~70-line Python implementation of the DBWIN protocol: a 4 KiB shared section named `DBWIN_BUFFER`
holding `{DWORD pid; char msg[]}`, plus the `DBWIN_BUFFER_READY` / `DBWIN_DATA_READY` auto-reset
event pair. Run the monitor, launch the build, read the log. Only one monitor may exist
machine-wide, and an attached debugger takes precedence over it.

**That is worth keeping.** Any `BX_ASSERT` anywhere in bx or bgfx is invisible without it, and
Phase 1 below makes it permanent.

### The rest of the plan

Ordered by information per unit of effort. Phase 0 is done; Phase 1 remains worth doing on its own
merits.

#### Phase 0 — Listen to the assertion that is already being written — **DONE, see above**

The engine is not missing the diagnosis; it is discarding it. `bx`'s `defaultAssertHandler`
([`extern/bx/src/bx.cpp`](../../GanymedEngine/extern/bx/src/bx.cpp)) already formats the failing
condition, the message, the source location **and a stack trace**, then writes it to
`bx::getDebugOut()` — `OutputDebugString` on Windows. Nothing in this project reads that stream, so
the process appears to die in silence. The macro then calls `bx::debugBreak()`, which is the exit
code 3.

Two ways to read it, neither needing a code change:

1. **Attach a debugger.** Run `GanymedEditor` under Visual Studio with `Samples = 4` restored in
   `SceneRenderer.cpp`. The assert text appears in the Output window and `debugBreak()` halts on the
   failing line with a live call stack — which names the bgfx function, the condition, and the
   attachment being built.
2. **Or capture it without one.** Sysinternals **DebugView** (run as administrator, "Capture Win32"
   enabled) shows the same text for a normally launched build. Use this if the debugger's timing
   changes anything, or to keep a transcript.

**Expected shape of the answer:** a line reading `ASSERT <condition> -> <message>` plus
`renderer_d3d11.cpp` or `bgfx.cpp` and a line number. That is very likely the whole story, and
everything below becomes unnecessary.

#### Phase 1 — Make bx assertions permanently visible

Worth doing regardless of MSAA, and the exact counterpart of the `bgfx::CallbackI` that made the
OpenGL bugs findable: that callback catches `Fatal::` codes, and **`BX_ASSERT` is a separate
channel it never sees**.

`bx` exposes `bx::setAssertHandler(AssertHandlerFn)` ([`extern/bx/include/bx/bx.h`](../../GanymedEngine/extern/bx/include/bx/bx.h)):

```cpp
typedef bool (*AssertHandlerFn)(const Location&, uint32_t skip, const char* format, va_list);
```

Three things govern how it is used:

- **It can only be set once.** `setAssertHandler` warns and ignores the call if a non-default
  handler is already installed, so install it early and in one place — `BgfxContext::Init` before
  `bgfx::init`, or the `Application` constructor.
- **The return value decides whether the process breaks.** `true` reproduces today's behaviour
  (log, then `debugBreak`); returning **`false` logs and continues**, which is the interesting
  option here: it turns the abort into a warning and lets the run proceed so you can see whether
  anything downstream also complains, or whether MSAA otherwise works.
- **`Location` is just `{ const char* filePath; uint32_t line; }`**, so the handler can forward
  straight into `GE_CORE_ERROR` with `vsnprintf`, exactly like `BgfxContext`'s `traceVargs`.

**The one obstacle**, and it is the same one that pushed `Projection::` onto glm instead of
`bx::mtxProj`: every bx header fails with
`#error "When using MSVC you must set /Zc:__cplusplus compiler option."`. The engine **already
links `bx`** ([`GanymedEngine/premake5.lua`](../../GanymedEngine/premake5.lua)), so only the include
is a problem. Options, best first:

1. Add `buildoptions { "/Zc:__cplusplus" }` to the engine project under the Windows filter. It is a
   one-line premake change and the flag is *correct* — without it MSVC reports `__cplusplus` as
   `199711L` regardless of `/std:c++17`. Risk: it changes a compile-time constant engine-wide, so
   header-only dependencies that branch on `__cplusplus` (entt, glm, yaml-cpp) may take different
   paths. Rebuild all three configurations and re-run the backend sweep before trusting it.
2. Compile one small translation unit with the flag via a per-file `buildoptions`, keeping the rest
   of the engine unchanged. More premake machinery, smaller blast radius.
3. Declare `bx::setAssertHandler` and `bx::Location` by hand in one `.cpp` to avoid the include
   entirely. Avoid unless 1 and 2 are both rejected — it duplicates a vendored type's layout and
   will break silently on a submodule bump.

#### Phase 2 — Superseded by Phase 0

Isolate engine from bgfx build configuration first: **write the smallest possible repro in Sandbox**
— `bgfx::createTexture2D` ×2 with `BGFX_TEXTURE_RT_MSAA_X4`, then `bgfx::createFrameBuffer`, and
nothing else. If that aborts, the question is about this bgfx build and belongs upstream; if it
does not, the difference is in how `Framebuffer::Build` constructs things, and can be bisected
against the working single-sample path.

Hypotheses worth testing in that harness, most likely first:

| # | Hypothesis | Test |
|---|---|---|
| 1 | An MSAA attachment cannot also be sampled, and the scene colour target *is* sampled by the tonemap | Add `BGFX_TEXTURE_RT_WRITE_ONLY`; if creation then succeeds, this is it, and the post chain needs a resolve target |
| 2 | `createFrameBuffer(num, handles, false)` does not accept separately created MSAA textures | Use the `createFrameBuffer(width, height, format, flags)` overload instead |
| 3 | Attachments must agree on sample count in a way the depth target violates | Build colour-only at x4, then add depth |
| 4 | `_hasMips = false` / `_numLayers = 1` interacts badly with MSAA | Vary each independently |

Already ruled out, so do not spend time here: it is **not** the entity-ID attachment, **not** the
`BGFX_SAMPLER_*` flags, and **not** a format capability gap.

#### Phase 3 — The design question, which no amount of debugging removes

Construction is fixed, and picking demonstrably survives MSAA at the interior of a shape. What is
**not** established is the edge: the worry is that **the entity-ID attachment is resolved by
averaging samples** — the mean of two entity ids is not an entity id, and picking reads that target
through a 1×1 blit
([Entity picking](../engine/rendering.md#entity-picking-async)). Decide this *before* writing MSAA
code, because it shapes the design:

- **Separate the ID pass.** Render entity ids into their own single-sample target. Costs an extra
  pass over the scene; keeps picking exact and makes the MSAA target colour-only.
- **Resolve the ID target manually**, taking sample 0 rather than the average. Cheaper, and "sample
  0" is a defensible definition of what is under the cursor, but it needs a custom resolve rather
  than the automatic one.
- **Do not multisample the scene target at all** — apply MSAA only where it helps and picking does
  not look, or reach for a post-process AA instead. The engine already has FXAA in the post stack,
  which is why MSAA has never been missed.

That last option deserves an honest hearing before any of this starts: FXAA already ships and runs
on all four backends, and a deferred-style HDR pipeline with an id attachment is precisely the shape
that makes MSAA awkward. **"Is MSAA worth it here?" is a cheaper question to answer than "why does
bgfx assert?", and it may retire the item outright.**

## D3D12 and Vulkan take longer to the first frame

Not a bug. First non-empty frame in a **Debug** build: D3D11 **1.16–1.84 s**, Vulkan **3.37 s**,
D3D12 **5.51 s**. Both render correctly once they start. Almost certainly eager pipeline-state
compilation. Worth re-measuring in Release before deciding whether it needs a cache —
`cacheRead`/`cacheWrite` on the bgfx callback are the hook, and currently decline.

## A note for whoever tests the next thing

Three things made the OpenGL bugs findable, and they generalise:

1. **The bgfx callback** ([bgfx diagnostics](../engine/rendering.md#bgfx-diagnostics)). Before it
   existed, bgfx reported fatals to the debugger and nowhere else — a shader that failed to compile
   looked like a hang with a clean log, and this Vulkan failure would have been invisible too.
2. **Compare against D3D11 pixel for pixel.** "Black viewport" and "wrong viewport" need completely
   different investigations.
3. **Bisect with a constant-value shader.** Forcing the tonemap to output solid red proved in one
   run that the fullscreen quad, the composite target and the ImGui image were all fine, moving the
   search upstream to the uniforms in a single step. The same trick cracked picking: writing a
   constant `7` and reading back `55` turned "green is not written" into an arithmetic identity that
   named the blend equation exactly.
4. **Three agreeing backends make the fourth's disagreement meaningful.** The GL picking map came
   back vertically mirrored against D3D11/D3D12/Vulkan, which was the *expected* raw-target
   convention rather than a bug — but it was only readable as such because the other three agreed.

## Optional, and explicitly not scheduled

From the same section, listed so they are not rediscovered as if they were new: multithreaded render
(dropping the `renderFrame()` trick), compute-shader IBL bakes, `texturec`-preprocessed KTX textures
with mips, and occlusion queries. None of these is blocking anything.

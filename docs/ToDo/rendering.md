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

## MSAA: dead plumbing that aborts if used

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

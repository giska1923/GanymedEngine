# ToDo — Rendering backends

The unfinished tail of `BGFX_MIGRATION.md` Phase 7, the last milestone in
[`docs/history/`](../history/BGFX_MIGRATION.md) still flagged in progress (`🚧 STEP 1 DONE`).

**§9.3 (caps-driven projection), §9.2 (backend selection) and the multi-backend bugs they exposed
are all done.** Current state on Windows:

| Backend | State |
|---|---|
| D3D11 | Renders correctly |
| D3D12 | Renders correctly — **pixel-identical** to D3D11 |
| OpenGL 3.3 | Renders correctly — within 5/255 of D3D11 |
| Vulkan | **Untested** — no driver on the dev machine |

What remains is Vulkan, and the per-backend checks that only matter once someone runs them.

---

## Vulkan is untested here, not broken

`--renderer=vulkan` reports that bgfx substituted D3D11 because Vulkan could not be started — this
machine has no Vulkan driver. Nothing is known about whether the engine renders correctly on it.
Needs a machine with a Vulkan-capable driver.

It is the one backend that has never executed a frame, so it is also the one most likely to hold a
surprise of the kind OpenGL held: three separate bugs, none of which were visible until the backend
actually ran.

## The rest of the test matrix

Worth walking per backend, now that three of them render:

- **sRGB.** The pipeline tonemaps manually, so the backbuffer stays linear
  (`BGFX_RESET_SRGB_BACKBUFFER` off). Verify the look matches per backend.
- **R32I / async picking.** OpenGL reports R32I as *emulated* for 2D sampling (native only as a
  framebuffer attachment), so entity picking is the most likely thing to behave differently there.
  It has not been exercised on GL — the scene renders, but nothing has clicked on it.
- **MSAA**, which no backend has been checked with.

## A note for whoever runs the next backend

Three things made the OpenGL bugs findable, and they are worth reaching for first:

1. **The bgfx callback** ([bgfx diagnostics](../engine/rendering.md#bgfx-diagnostics)). Before it
   existed, bgfx reported fatals to the debugger and nowhere else, and a shader that failed to
   compile looked like a hang with a clean log.
2. **Compare against D3D11 on the same scene**, pixel for pixel. "Black viewport" and "wrong
   viewport" need completely different investigations.
3. **Bisect the pass chain with a constant-colour shader.** Forcing the tonemap to output solid red
   proved in one run that the fullscreen quad, the composite target and the ImGui image were all
   fine, which moved the search upstream to the uniforms in a single step.

## Optional, and explicitly not scheduled

From the same section, listed so they are not rediscovered as if they were new: multithreaded render
(dropping the `renderFrame()` trick), compute-shader IBL bakes, `texturec`-preprocessed KTX textures
with mips, and occlusion queries. None of these is blocking anything.

## D3D12 takes ~4.4 s to its first frame

Not a bug, and not blocking. First non-empty frame in a **Debug** build: D3D11 **1.16 s**, D3D12
**5.51 s**; the frame is pixel-identical once it arrives. Almost certainly eager pipeline-state
compilation. Worth re-measuring in Release before deciding whether it needs a PSO cache —
`cacheRead`/`cacheWrite` on the bgfx callback are the hook for one, and currently decline.

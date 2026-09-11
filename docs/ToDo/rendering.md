# ToDo — Rendering backends

The unfinished tail of `BGFX_MIGRATION.md` Phase 7, the last milestone in
[`docs/history/`](../history/BGFX_MIGRATION.md) still flagged in progress (`🚧 STEP 1 DONE`). Step 1
— removing OpenGL from the build — is done; what follows is multi-backend hardening.

**§9.3 (caps-driven projection) and §9.2 (backend selection) are both done** — see
[Projection matrices](../engine/rendering.md#projection-matrices) and
[Backend selection](../engine/rendering.md#backend-selection). What is left is making the other
backends actually work, which §9.2 turned from a hypothetical into a list of specific failures.

---

## Multi-backend validation

`--renderer=` made the other backends reachable, and they were run. On Windows, **D3D11 is the only
one that renders correctly today.** Each of the three below is a separate, reproducible failure.

### D3D12 renders an empty viewport

Selects cleanly, runs without a single bgfx error or warning in the log, and draws **nothing** into
the viewport — the scene panel is uniformly background-coloured where D3D11 shows the scene. The
editor chrome (ImGui) draws fine, so the failure is specific to the offscreen scene target rather
than to submission as a whole.

Reproduce: `GanymedEditor.exe --renderer=d3d12`.

Worth checking first, precisely because ImGui works and the scene does not: the HDR offscreen target
and its blit into the viewport image, and the R32I picking attachment
([rendering.md](../engine/rendering.md)) — an unsupported attachment format is the kind of thing that
invalidates a framebuffer quietly.

### OpenGL hangs at the IBL bake

Selects, and **correctly reports `homogeneousDepth=true` and `originBottomLeft=true`** — so §9.3's
`[-1,1]` path and the shaders' per-profile branches are live and answering correctly. That is the one
thing that could not be tested before backend selection existed. The glsl shader profile loads.

Then it hangs: the last log line is the FXAA shader, and it never reaches the `Equirect` shader that
D3D11 loads next, so it stops inside the environment bake. No error, no assert — it just stops.

**The likely cause is the GL context version.** bgfx reports `OpenGL 2.1`, because
`BGFX_CONFIG_RENDERER_OPENGL` is not set in
[`extern/bgfx.lua`](../../GanymedEngine/extern/bgfx.lua) and bgfx then defaults low. GL 2.1 has no
float cubemap render targets, which is exactly what the bake needs. Setting it to `43` and rebuilding
bgfx is the first thing to try — deliberately *not* done as part of §9.2, because it changes how the
vendored bgfx is built for **every** backend and would invalidate the D3D11 verification alongside it.

Reproduce: `GanymedEditor.exe --renderer=gl`, then kill it.

### Vulkan is untested here, not broken

`--renderer=vulkan` reports that bgfx substituted D3D11 because Vulkan could not be started — this
machine has no Vulkan driver. Nothing is known about whether the engine renders correctly on it.
Needs a machine with a Vulkan-capable driver.

## The rest of the test matrix

Once a backend renders at all, the per-backend gotchas the migration doc lists are still worth
walking:

- Depth range / clip space — handled by §9.3, and GL confirms the caps are read correctly, but no
  `[-1,1]` backend has yet rendered a frame.
- Render-target origin flip in every fullscreen pass.
- sRGB: the pipeline tonemaps manually, so the backbuffer stays linear
  (`BGFX_RESET_SRGB_BACKBUFFER` off) — verify the look matches per backend.
- R32I attachment support, which async picking depends on.

## Optional, and explicitly not scheduled

From the same section, listed so they are not rediscovered as if they were new: multithreaded render
(dropping the `renderFrame()` trick), compute-shader IBL bakes, `texturec`-preprocessed KTX textures
with mips, and occlusion queries. None of these is blocking anything.

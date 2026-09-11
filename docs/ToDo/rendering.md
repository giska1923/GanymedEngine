# ToDo — Rendering backends

Everything here is the unfinished tail of `BGFX_MIGRATION.md` Phase 7, the only milestone in
[`docs/history/`](../history/BGFX_MIGRATION.md) still flagged in progress (`🚧 STEP 1 DONE`). Step 1
— removing OpenGL from the build — is done; what follows is multi-backend hardening.

See [rendering.md](../engine/rendering.md) and [platform.md](../engine/platform.md) for how the
backend is brought up today.

---

## Caps-driven projection for `[-1,1]` depth backends (§9.3)

**This is a latent correctness bug, not a feature gap**, and it is the first thing to do here.

The whole project compiles with `GLM_FORCE_DEPTH_ZERO_TO_ONE` (a workspace define in
`premake5.lua`). That is a *compile-time* choice, so a backend that wants OpenGL's `[-1,1]` clip
depth does not fail loudly — it renders with wrong near-plane clipping and half the depth precision.
[`BgfxContext.cpp`](../../GanymedEngine/source/Platform/Bgfx/BgfxContext.cpp) currently detects the
situation and logs an error rather than handling it:

```cpp
if (caps->homogeneousDepth)
{
    GE_CORE_ERROR("Backend '{0}' expects [-1,1] clip depth, but glm is built for [0,1]. ...");
}
```

**What it needs:** a projection helper that takes `bgfx::getCaps()->homogeneousDepth` and builds the
matrix accordingly, replacing direct `glm::perspective`/`glm::ortho` calls on the render path. Two
call sites already do this by hand and are the model to generalise —
`ImGuiRendererBgfx.cpp` and `RmlUiRendererBgfx.cpp` both read `homogeneousDepth` from caps and pick
their near/far accordingly.

Related, from the same section: `SampleCascade`'s render-target origin flip should become
caps-driven at the same time rather than assuming a top-down origin.

## Backend selection (§9.2)

[`BgfxContext.cpp`](../../GanymedEngine/source/Platform/Bgfx/BgfxContext.cpp) still hardcodes the
auto-pick:

```cpp
init.type = bgfx::RendererType::Count; // auto-pick; configurable in Phase 7
```

**What it needs:** `--renderer=vulkan|d3d12|d3d11|gl` on the command line (or a `runtime.yaml` key —
[runtime.md](../runtime/runtime.md) already has a config file) feeding `init.type`, and a log line
with `bgfx::getRendererName()`. `Application::SetCommandLineArgs` already captures argv.

Worth doing *after* §9.3, not before: being able to select a broken backend is not an improvement.

## Multi-backend validation

Once the two above are in, run the matrix on Windows — D3D11, D3D12, Vulkan, OpenGL. The migration
doc lists the per-backend gotchas worth checking first:

- Depth range / clip space (fixed by §9.3, verify per backend).
- Render-target origin flip in every fullscreen pass.
- sRGB: the pipeline tonemaps manually, so the backbuffer stays linear
  (`BGFX_RESET_SRGB_BACKBUFFER` off) — verify the look matches per backend.
- R32I attachment support, which async picking depends on
  ([rendering.md](../engine/rendering.md)).

## Optional, and explicitly not scheduled

From the same section, listed so they are not rediscovered as if they were new: multithreaded
render (dropping the `renderFrame()` trick), compute-shader IBL bakes, `texturec`-preprocessed KTX
textures with mips, and occlusion queries. None of these is blocking anything.
